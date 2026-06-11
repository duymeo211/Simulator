/**
 * @file    tx_transmit.c
 * @brief   TX Transmit module: drain CAN message queues and send
 *          TCP packages. Includes async debug log (tx_debug.log)
 *          and detail log integration (SimulatorTxRx.log via log.h).
 * @date    2026-04
 */
#include "tx_transmit.h"
#include "sim_log.h"
#include "sim_trace.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif

 /* Forward declarations */
static void send_package(TxTransmit* tx, int ch, TcpPackageBuffer* pkg);

/* ================================================================== */
/* Async debug log (tx_debug.log — summary, realtime via PowerShell)  */
/* This is the EXISTING log system, kept as-is.                       */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Defines (local to this file, no conflict with log.h)               */
/* ------------------------------------------------------------------ */
#define TXLOG_QUEUE_MAX     256
#define TXLOG_LINE_MAX      320
#define TX_DEBUG_MAX_LINES 50000

/* ------------------------------------------------------------------ */
/* Internal types                                                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    char lines[TXLOG_QUEUE_MAX][TXLOG_LINE_MAX];
    int  head;
    int  tail;
    int  count;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    HANDLE           thread;
    HANDLE           event;
#else
    pthread_mutex_t  lock;
    pthread_t        thread;
    pthread_cond_t   cond;
#endif
    bool             running;
} TxDebugLog;

/* ------------------------------------------------------------------ */
/* Static variables                                                   */
/* ------------------------------------------------------------------ */
static TxDebugLog s_dbg = { 0 };
static FILE* s_tx_log = NULL;
static unsigned int s_tx_line_count = 0;
#ifdef _WIN32
static HANDLE     s_tx_ps_process = NULL;
#endif

/* ------------------------------------------------------------------ */
/* Debug log queue push                                               */
/* ------------------------------------------------------------------ */
static void txdbg_push(const char* line)
{
#ifdef _WIN32
    EnterCriticalSection(&s_dbg.lock);
#else
    pthread_mutex_lock(&s_dbg.lock);
#endif

    if (s_dbg.count < TXLOG_QUEUE_MAX)
    {
        strncpy(s_dbg.lines[s_dbg.tail], line, TXLOG_LINE_MAX - 1);
        s_dbg.lines[s_dbg.tail][TXLOG_LINE_MAX - 1] = '\0';
        s_dbg.tail = (s_dbg.tail + 1) % TXLOG_QUEUE_MAX;
        s_dbg.count++;
    }

#ifdef _WIN32
    LeaveCriticalSection(&s_dbg.lock);
    SetEvent(s_dbg.event);
#else
    pthread_cond_signal(&s_dbg.cond);
    pthread_mutex_unlock(&s_dbg.lock);
#endif
}

/* ------------------------------------------------------------------ */
/* Debug log writer thread                                            */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static DWORD WINAPI txdbg_thread_func(LPVOID arg)
#else
static void* txdbg_thread_func(void* arg)
#endif
{
    (void)arg;

    while (s_dbg.running || s_dbg.count > 0)
    {
#ifdef _WIN32
        WaitForSingleObject(s_dbg.event, 100);
        EnterCriticalSection(&s_dbg.lock);
#else
        pthread_mutex_lock(&s_dbg.lock);
        pthread_cond_wait(&s_dbg.cond, &s_dbg.lock);
#endif

        while (s_dbg.count > 0)
        {
            char line[TXLOG_LINE_MAX];
            strncpy(line, s_dbg.lines[s_dbg.head], TXLOG_LINE_MAX - 1);
            line[TXLOG_LINE_MAX - 1] = '\0';
            s_dbg.head = (s_dbg.head + 1) % TXLOG_QUEUE_MAX;
            s_dbg.count--;

#ifdef _WIN32
            LeaveCriticalSection(&s_dbg.lock);
#else
            pthread_mutex_unlock(&s_dbg.lock);
#endif
            if (s_tx_log != NULL)
            {
                fprintf(s_tx_log, "%s", line);
                fflush(s_tx_log);
            }
#ifdef _WIN32
            EnterCriticalSection(&s_dbg.lock);
#else
            pthread_mutex_lock(&s_dbg.lock);
#endif
        }

#ifdef _WIN32
        LeaveCriticalSection(&s_dbg.lock);
#else
        pthread_mutex_unlock(&s_dbg.lock);
#endif
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Debug log open / close / print                                     */
/* ------------------------------------------------------------------ */
static void txdbg_open(void)
{
    s_tx_log = fopen("log/tx_debug.log", "w");
    if (s_tx_log == NULL)
    {
        fprintf(stderr, "[TX Transmit] Cannot open tx_debug.log\n");
        return;
    }

#ifdef _WIN32
    InitializeCriticalSection(&s_dbg.lock);
    s_dbg.event = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
    pthread_mutex_init(&s_dbg.lock, NULL);
    pthread_cond_init(&s_dbg.cond, NULL);
#endif
    s_dbg.running = true;

#ifdef _WIN32
    s_dbg.thread = CreateThread(NULL, 0, txdbg_thread_func,
        NULL, 0, NULL);
#else
    pthread_create(&s_dbg.thread, NULL, txdbg_thread_func, NULL);
#endif

#ifdef _WIN32
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        char cmd[] = "powershell.exe -NoExit -Command \""
            "$Host.UI.RawUI.WindowTitle='TX Log'; "
            "Get-Content 'log\\tx_debug.log' -Wait\"";

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
            CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
        {
            s_tx_ps_process = pi.hProcess;
            CloseHandle(pi.hThread);
        }
    }
#endif
}

static void txdbg_close(void)
{
    s_dbg.running = false;

#ifdef _WIN32
    SetEvent(s_dbg.event);
    WaitForSingleObject(s_dbg.thread, 3000);
    CloseHandle(s_dbg.thread);
    CloseHandle(s_dbg.event);
    DeleteCriticalSection(&s_dbg.lock);
#else
    pthread_cond_signal(&s_dbg.cond);
    pthread_join(s_dbg.thread, NULL);
    pthread_mutex_destroy(&s_dbg.lock);
    pthread_cond_destroy(&s_dbg.cond);
#endif

#ifdef _WIN32
    if (s_tx_ps_process != NULL)
    {
        TerminateProcess(s_tx_ps_process, 0);
        CloseHandle(s_tx_ps_process);
        s_tx_ps_process = NULL;
    }
#endif
    if (s_tx_log != NULL)
    {
        fclose(s_tx_log);
        s_tx_log = NULL;
    }
}

static void txdbg_print(const char* fmt, ...)
{
    char    msg_buf[256];
    char    out_buf[TXLOG_LINE_MAX];
    va_list args;

    if (!s_tx_log) return;

    /* Rotate by line count */
    if (s_tx_line_count >= TX_DEBUG_MAX_LINES)
    {
        freopen("log/tx_debug.log", "w", s_tx_log);
        s_tx_line_count = 0;

        fprintf(s_tx_log, "--- tx_debug.log reset (50k lines reached) ---\n");
        fflush(s_tx_log);
    }

    /* Format message */
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Add timestamp */
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);

    snprintf(out_buf, sizeof(out_buf),
        "[%02u:%02u:%02u.%03u] %s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg_buf);
#else
    struct timespec ts;
    struct tm       tm_info;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);

    snprintf(out_buf, sizeof(out_buf),
        "[%02d:%02d:%02d.%03ld] %s",
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        ts.tv_nsec / 1000000L,
        msg_buf);
#endif

    /* Push to async queue */
    txdbg_push(out_buf);

    s_tx_line_count++;
}

/* ================================================================== */
/* Detail logging to SimulatorTxRx.log (via log module)               */
/* Parses the TCP package buffer and logs each CAN frame.             */
/* ================================================================== */

static void sim_trace_write_package(int ch_num, uint32_t round,
    const TcpPackageBuffer* pkg)
{
    //char line[LOG_ENTRY_MAX_LEN];
    char line[SIM_TRACE_LINE_MAX];
    char ts[32];

    sim_trace_timestamp(ts, sizeof(ts));

    /* Header line */

    TRACE_HEADER(line, sizeof(line),
        ts, ch_num, "TX", round, pkg->msg_count, pkg->len);

    sim_trace_write(line);

    /* Parse CAN frames from encoded TCP package buffer             */
    /* Layout per frame: ID(4) + LEN(1) + DATA(LEN)                */
    uint16_t pos = TCP_PKG_HEADER_SIZE;

    while (pos + 5 <= pkg->len)
    {
        uint32_t id = ((uint32_t)pkg->data[pos] << 24) |
            ((uint32_t)pkg->data[pos + 1] << 16) |
            ((uint32_t)pkg->data[pos + 2] << 8) |
            (uint32_t)pkg->data[pos + 3];
        uint8_t  flen = pkg->data[pos + 4];
        pos += 5;

        if (pos + flen > pkg->len) break;

        sim_trace_format_can(line, sizeof(line),
            id, flen, &pkg->data[pos]);
        sim_trace_write(line);

        pos += flen;
    }
}

/* ================================================================== */
/* Internal helpers                                                   */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Platform sleep                                                     */
/* ------------------------------------------------------------------ */
static void transmit_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}
/* ------------------------------------------------------------------ */
/* Internal: write package header into buffer                          */
/*                                                                     */
/*   buf[0..1] = Payload Length (big-endian)                           */
/*   buf[2..5] = Round Count (big-endian)                              */
/*                                                                     */
/*   payload_len = bytes after Payload Length field                    */
/*               = Round Count(4) + CAN data bytes                     */
/* ------------------------------------------------------------------ */
static void encode_pkg_header(uint8_t* buf,
    uint16_t payload_len,
    uint32_t round_count)
{
    buf[0] = (uint8_t)((payload_len >> 8) & 0xFF);
    buf[1] = (uint8_t)(payload_len & 0xFF);

    buf[2] = (uint8_t)((round_count >> 24) & 0xFF);
    buf[3] = (uint8_t)((round_count >> 16) & 0xFF);
    buf[4] = (uint8_t)((round_count >> 8) & 0xFF);
    buf[5] = (uint8_t)(round_count & 0xFF);
}

static uint16_t encode_can_frame(const CanMsgEntry* msg,
    uint8_t* buf,
    uint16_t          remaining)
{
    uint16_t frame_size = (uint16_t)(4u + 1u + (uint16_t)msg->length);

    if (frame_size > remaining)
    {
        return 0;
    }

    buf[0] = (uint8_t)((msg->id >> 24) & 0xFF);
    buf[1] = (uint8_t)((msg->id >> 16) & 0xFF);
    buf[2] = (uint8_t)((msg->id >> 8) & 0xFF);
    buf[3] = (uint8_t)(msg->id & 0xFF);
    buf[4] = msg->length;

    memcpy(buf + 5, msg->data, msg->length);

    return frame_size;
}

/* ================================================================== */
/* Detail TX log (SimulatorTxRx.log)                                  */
/* - Header line giống style client                                   */
/* - Dump every CAN frame, indent 4 spaces                            */
/* ================================================================== */
static void log_tx_detail_package(int ch_num,
    uint32_t round_count,
    const TcpPackageBuffer* pkg)
{
    char ts[32];
    //char line[LOG_ENTRY_MAX_LEN];
    char line[SIM_TRACE_LINE_MAX];

    if (pkg == NULL) return;

    /* Header line */
    sim_trace_timestamp(ts, (int)sizeof(ts));
    snprintf(line, sizeof(line),
        "%s [Ch%d TX] Round=%-6u | %3d msg(s) | %4u bytes",
        ts, ch_num, round_count, pkg->msg_count, pkg->len);
    sim_trace_write(line);

    /* Parse frames from pkg: [id(4) + len(1) + data(len)] ... */
    uint16_t pos = TCP_PKG_HEADER_SIZE;

    while ((uint16_t)(pos + 5u) <= pkg->len)
    {
        uint32_t id = ((uint32_t)pkg->data[pos] << 24) |
            ((uint32_t)pkg->data[pos + 1] << 16) |
            ((uint32_t)pkg->data[pos + 2] << 8) |
            (uint32_t)pkg->data[pos + 3];

        uint8_t dlc = pkg->data[pos + 4];
        pos = (uint16_t)(pos + 5u);

        if ((uint16_t)(pos + dlc) > pkg->len)
        {
            sim_log_warn(
                "TX detail parse stopped: invalid frame length (Ch%d, Round=%u)",
                ch_num, round_count);
            break;
        }

        /* Dump CAN frame with 4-space indent (multi-line for >32 bytes) */
        sim_trace_format_can(line, (int)sizeof(line), id, dlc, &pkg->data[pos]);
        sim_trace_write(line);

        pos = (uint16_t)(pos + dlc);
    }
}

/* ================================================================== */
/* CHANGED: drain / send now take channel index                        */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Internal: drain one channel's queue, build and send packages        */
/* ------------------------------------------------------------------ */
static void drain_and_send(TxTransmit* tx, int ch)
{
    TxChannelCtx* ctx = &tx->channels[ch];
    int           ch_num = ch + 1;

    CanMsgEntry pending_entry;
    bool        has_pending = false;
    int         total_pkg_sent = 0;

    while (has_pending || (can_msg_queue_count(ctx->queue) > 0))
    {
        TcpPackageBuffer pkg = { 0 };
        uint16_t         pos = TCP_PKG_HEADER_SIZE;
        int              msg_count = 0;

        /* ---- Add pending message from previous full package ---- */
        if (has_pending)
        {
            uint16_t remaining = (uint16_t)(TCP_PACKAGE_MAX_LEN - pos);
            uint16_t frame_size = encode_can_frame(&pending_entry,
                pkg.data + pos,
                remaining);
            if (frame_size > 0)
            {
                pos = (uint16_t)(pos + frame_size);
                msg_count++;
            }
            else
            {
                txdbg_print("[TX Ch%d] Error: msg ID=0x%04X too large for package, dropped.\n",
                    ch_num, pending_entry.id);
                sim_log_error(
                    "TX drop: msg ID=0x%04X too large for package (Ch%d)",
                    pending_entry.id, ch_num);
            }
            has_pending = false;
        }

        /* ---- Pop from queue until package full or queue empty ---- */
        {
            CanMsgEntry entry;
            while (can_msg_queue_pop(ctx->queue, &entry))
            {
                uint16_t remaining = (uint16_t)(TCP_PACKAGE_MAX_LEN - pos);
                uint16_t frame_size = encode_can_frame(&entry,
                    pkg.data + pos,
                    remaining);
                if (frame_size > 0)
                {
                    pos = (uint16_t)(pos + frame_size);
                    msg_count++;
                }
                else
                {
                    pending_entry = entry;
                    has_pending = true;
                    break;
                }
            }
        }

        /* ---- Encode header and send package ---- */
        if (msg_count > 0)
        {
            ctx->round_count++;

            uint16_t payload_len = (uint16_t)(pos - TCP_PKG_PLDLEN_SIZE);
            encode_pkg_header(pkg.data, payload_len, ctx->round_count);

            pkg.len = pos;
            pkg.msg_count = msg_count;

            send_package(tx, ch, &pkg);
            total_pkg_sent++;
        }
    }

    if (total_pkg_sent > 1)
    {
        txdbg_print("[Ch%d TX] Tick summary: %d packages sent.\n",
            ch_num, total_pkg_sent);
    }
}

/* ------------------------------------------------------------------ */
/* Internal: send package with retry, drop on failure                  */
/* ------------------------------------------------------------------ */
static void send_package(TxTransmit* tx, int ch, TcpPackageBuffer* pkg)
{
    TxChannelCtx* ctx = &tx->channels[ch];
    int           ch_num = ch + 1;

    bool ok = ctx->send_fn(pkg->data, pkg->len, ctx->send_user);

    if (ok)
    {
        /* Summary log (existing realtime TX debug) */
        txdbg_print("[Ch%d TX] Round=%-6u | %3d msg(s) | %4u bytes\n",
            ch_num,
            ctx->round_count,
            pkg->msg_count,
            pkg->len);

        /* Detail log (async, merged TX/RX file) */
        log_tx_detail_package(ch_num, ctx->round_count, pkg);

        return;
    }

    /* Send failed */
    txdbg_print("[TX Ch%d] Warning: send failed, round=%u (%d msg) DROPPED.\n",
        ch_num, ctx->round_count, pkg->msg_count);

    sim_log_warn(
        "TX send failed: Ch%d Round=%u (%d msg) dropped",
        ch_num, ctx->round_count, pkg->msg_count);

    /* Flush stale messages from queue */
    {
        CanMsgEntry discard;
        int         flushed = 0;
        char        ids[128];
        int         pos = 0;

        memset(ids, 0, sizeof(ids));

        while (can_msg_queue_pop(ctx->queue, &discard))
        {
            if (flushed < 10)
            {
                pos += snprintf(ids + pos, sizeof(ids) - (size_t)pos,
                    "0x%04X ", discard.id);
            }
            flushed++;
        }

        if (flushed > 0)
        {
            txdbg_print("[TX Ch%d] Flushed MsgIDs: %s%s(%d total)\n",
                ch_num, ids,
                flushed > 10 ? "... " : "",
                flushed);

            sim_log_warn(
                "TX flush: Ch%d flushed=%d (first IDs: %s%s)",
                ch_num, flushed, ids, flushed > 10 ? "..." : "");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Thread function — iterates all channels                             */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static DWORD WINAPI tx_transmit_thread(LPVOID arg)
#else
static void* tx_transmit_thread(void* arg)
#endif
{
    TxTransmit* tx = (TxTransmit*)arg;
    int i;

    txdbg_open();
    txdbg_print("[TX Transmit] Thread started (%d ch, interval=%d ms).\n",
        tx->channel_count, tx->interval_ms);

    sim_log_info(
        "TX Transmit thread started (ch=%d, interval=%d ms)",
        tx->channel_count, tx->interval_ms);

    while (tx->running)
    {
        for (i = 0; i < tx->channel_count; i++)
        {
            drain_and_send(tx, i);   /* always drain, empty queue = no-op */
        }

        transmit_sleep_ms(tx->interval_ms);
    }

    txdbg_print("[TX Transmit] Thread stopped.\n");
    sim_log_info( "TX Transmit thread stopped");

    txdbg_close();

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void tx_transmit_init(TxTransmit* tx, int channel_count, int interval_ms)
{
    if (tx == NULL) return;

    memset(tx, 0, sizeof(*tx));

    if (channel_count < 1)                channel_count = 1;
    if (channel_count > TX_MAX_CHANNELS)  channel_count = TX_MAX_CHANNELS;
    if (interval_ms < 1)                  interval_ms = 1;

    tx->channel_count = channel_count;
    tx->interval_ms = interval_ms;
    tx->running = false;
}

void tx_transmit_set_channel(TxTransmit* tx,
    int         ch,
    CanMsgQueue* queue,
    TxTransmitSendFn send_fn,
    void* send_user)
{
    if (tx == NULL || ch < 0 || ch >= tx->channel_count) return;

    tx->channels[ch].queue = queue;
    tx->channels[ch].send_fn = send_fn;
    tx->channels[ch].send_user = send_user;
    tx->channels[ch].round_count = 0;
    tx->channels[ch].enabled = false;
}

int tx_transmit_start(TxTransmit* tx)
{
    if (tx == NULL) return -1;

    tx->running = true;

#ifdef _WIN32
    timeBeginPeriod(1);

    tx->thread = CreateThread(NULL, 0, tx_transmit_thread, tx, 0, NULL);
    if (tx->thread == NULL)
    {
        fprintf(stderr, "[TX Transmit] Failed to create thread.\n");
        sim_log_error( "TX Transmit CreateThread failed");
        timeEndPeriod(1);
        tx->running = false;
        return -1;
    }
#else
    if (pthread_create(&tx->thread, NULL, tx_transmit_thread, tx) != 0)
    {
        fprintf(stderr, "[TX Transmit] Failed to create thread.\n");
        sim_log_error( "TX Transmit pthread_create failed");
        tx->running = false;
        return -1;
    }
#endif

    printf("[TX Transmit] Thread started (%d ch, interval: %d ms).\n",
        tx->channel_count, tx->interval_ms);
    return 0;
}

void tx_transmit_stop(TxTransmit* tx)
{
    if (tx == NULL) return;

    tx->running = false;

#ifdef _WIN32
    if (tx->thread != NULL)
    {
        WaitForSingleObject(tx->thread, 3000);
        CloseHandle(tx->thread);
        tx->thread = NULL;
    }
    timeEndPeriod(1);
#else
    pthread_join(tx->thread, NULL);
#endif

    printf("[TX Transmit] Thread stopped.\n");
}