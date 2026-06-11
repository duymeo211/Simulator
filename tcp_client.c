/**
 * @file    tcp_client.c
 * @brief   Multi-channel TCP client: connect to simulator, RX/TX vCAN packages.
 * @date    2026-05
 */
#include "tcp_common.h"
#include "sim_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif

#ifdef _WIN32
 /**
  * @brief Resize a window by title, with retry (wait for window to appear).
  */
static void resize_window(const char* title, int width_px, int height_px)
{
    int retries = 10;
    HWND hwnd = NULL;

    for (int i = 0; i < retries; i++)
    {
        hwnd = FindWindowA(NULL, title);
        if (hwnd != NULL) break;
        Sleep(300);
    }

    if (hwnd == NULL) return;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    MoveWindow(hwnd, rc.left, rc.top, width_px, height_px, TRUE);
}
#endif
 
/* ------------------------------------------------------------------ */
/* Console print lock (prevent interleaved output from multiple threads) */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static CRITICAL_SECTION s_print_lock;
#else
static pthread_mutex_t  s_print_lock;
#endif

static FILE* s_rx_log = NULL;

/* ------------------------------------------------------------------ */
/* Socket send lock (safe full-duplex send from TX thread)            */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static CRITICAL_SECTION s_send_lock;
#else
static pthread_mutex_t  s_send_lock;
#endif

static void print_lock_init(void)
{
#ifdef _WIN32
    InitializeCriticalSection(&s_print_lock);
    InitializeCriticalSection(&s_send_lock);
#else
    pthread_mutex_init(&s_print_lock, NULL);
    pthread_mutex_init(&s_send_lock, NULL);
#endif
}

static void print_lock_destroy(void)
{
#ifdef _WIN32
    DeleteCriticalSection(&s_print_lock);
    DeleteCriticalSection(&s_send_lock);
#else
    pthread_mutex_destroy(&s_print_lock);
    pthread_mutex_destroy(&s_send_lock);
#endif
}

static void print_lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&s_print_lock);
#else
    pthread_mutex_lock(&s_print_lock);
#endif
}

static void print_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_print_lock);
#else
    pthread_mutex_unlock(&s_print_lock);
#endif
}

static void send_lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&s_send_lock);
#else
    pthread_mutex_lock(&s_send_lock);
#endif
}

static void send_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_send_lock);
#else
    pthread_mutex_unlock(&s_send_lock);
#endif
}

/* ------------------------------------------------------------------ */
/* Constants / Protocol                                                */
/* ------------------------------------------------------------------ */
#define RX_MAX_CAN_FRAMES    112
#define RX_MAX_DATA_BYTES    64
#define PKG_PLDLEN_SIZE      2
#define PKG_ROUND_SIZE       4
#define PKG_HDR_SIZE         (PKG_PLDLEN_SIZE + PKG_ROUND_SIZE)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static inline uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}

static inline uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void write_be16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void write_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* Build timestamp string once: "[HH:MM:SS.mmm]" */
static void build_timestamp(char* out, size_t out_size)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, out_size, "[%02u:%02u:%02u.%03u]",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec tspec;
    struct tm       tm_info;
    clock_gettime(CLOCK_REALTIME, &tspec);
    localtime_r(&tspec.tv_sec, &tm_info);
    snprintf(out, out_size, "[%02d:%02d:%02d.%03ld]",
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        tspec.tv_nsec / 1000000L);
#endif
}

/* ------------------------------------------------------------------ */
/* Read exactly 'len' bytes from socket (blocking until complete)     */
/* Returns: len on success, 0 on disconnect, -1 on error, -2 timeout  */
/* ------------------------------------------------------------------ */
static int recv_exact(socket_t s, uint8_t* buf, int len, int timeout_ms)
{
    int total = 0;

    while (total < len)
    {
        int n = recv_with_timeout(s, buf + total, len - total, timeout_ms);

        if (n == -2)
        {
            if (total == 0) return -2;
            continue;
        }
        if (n <= 0) return n;

        total += n;
    }

    return total;
}

static socket_t connect_to_server(const char* server_ip, uint16_t port)
{
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == socket_invalid)
    {
        print_last_error("socket() failed");
        return socket_invalid;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == socket_error)
    {
        print_last_error("connect() failed");
        close_socket(s);
        return socket_invalid;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Per-channel context                                                 */
/* ------------------------------------------------------------------ */
typedef struct
{
    int         ch_num;
    socket_t    sock;
    const char* server_ip;
    uint16_t    port;
    volatile bool running;

    bool        tx_enabled;
    uint32_t    tx_round;

    /* ADD: per-channel TX params */
    uint32_t    tx_can_id;
    uint8_t     tx_dlc;
    uint32_t    tx_cycle_ms;
    uint8_t     tx_data[64]; /* enough for CANFD demo */

#ifdef _WIN32
    HANDLE      rx_thread;
    HANDLE      tx_thread;
#else
    pthread_t   rx_thread;
    pthread_t   tx_thread;
#endif
} ChannelCtx;
/* ------------------------------------------------------------------ */
/* RX: parse and print package summary (IDs only)                      */
/* Console has [Client], file does NOT need [Client]                   */
/* ------------------------------------------------------------------ */
static void parse_and_print_package(int ch_num,
    const uint8_t* payload,
    int payload_len)
{
    int offset = 0;

    if (payload_len < 4)
    {
        char ts[32];
        build_timestamp(ts, sizeof(ts));
        print_lock();
        printf("%s [Client][Ch%d RX] Package too short (%d bytes)\n",
            ts, ch_num, payload_len);
        print_unlock();
        return;
    }

    uint32_t round_count = read_be32(&payload[offset]);
    offset += 4;

    uint32_t can_ids[RX_MAX_CAN_FRAMES];
    int      can_lens[RX_MAX_CAN_FRAMES];
    uint8_t  can_data[RX_MAX_CAN_FRAMES][RX_MAX_DATA_BYTES];
    int      can_count = 0;

    while (offset + 5 <= payload_len)
    {
        uint32_t can_id = read_be32(&payload[offset]);
        offset += 4;

        uint8_t data_len = payload[offset];
        offset += 1;

        if (data_len > RX_MAX_DATA_BYTES) break;
        if (offset + data_len > payload_len) break;

        if (can_count < RX_MAX_CAN_FRAMES)
        {
            can_ids[can_count] = can_id;
            can_lens[can_count] = data_len;
            memcpy(can_data[can_count], &payload[offset], data_len);
            can_count++;
        }

        offset += data_len;
    }

    char ts[32];
    build_timestamp(ts, sizeof(ts));

    /* ---- Console: summary + IDs (max 10) ---- */
    print_lock();

    printf("%s [Client][Ch%d RX] Round=%-6u | %3d msg(s) | %4u bytes | IDs:\n",
        ts, ch_num, round_count, can_count,
        (unsigned)(payload_len + PKG_PLDLEN_SIZE));
    for (int i = 0; i < can_count && i < 10; i++)
        printf(" 0x%X", can_ids[i]);
    if (can_count > 10) printf(" ...");
    printf("\n");

    /* ---- File: summary + detail ---- */
    if (s_rx_log != NULL)
    {
        fprintf(s_rx_log,
            "%s [Ch%d RX] Round=%-6u | %3d msg(s) | %4u bytes\n",
            ts, ch_num, round_count, can_count,
            (unsigned)(payload_len + PKG_PLDLEN_SIZE));

        for (int i = 0; i < can_count; i++)
        {
            fprintf(s_rx_log, "  0x%04X | %2d |",
                can_ids[i], can_lens[i]);

            for (int j = 0; j < can_lens[i]; j++)
            {
                if (j == 32)
                    fprintf(s_rx_log, "\n               ");
                fprintf(s_rx_log, " %02X", can_data[i][j]);
            }
            fprintf(s_rx_log, "\n");
        }

        fflush(s_rx_log);
    }

    print_unlock();
}

/* ------------------------------------------------------------------ */
/* RX thread — one per channel                                         */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static DWORD WINAPI rx_thread_func(LPVOID arg)
#else
static void* rx_thread_func(void* arg)
#endif
{
    ChannelCtx* ctx = (ChannelCtx*)arg;
    uint8_t payload[2048];

    /* Connect */
    ctx->sock = connect_to_server(ctx->server_ip, ctx->port);
    if (ctx->sock == socket_invalid)
    {
        char ts[32];
        build_timestamp(ts, sizeof(ts));

        print_lock();
        printf("%s [Client][Ch%d] Failed to connect to %s:%u\n",
            ts, ctx->ch_num, ctx->server_ip, ctx->port);
        print_unlock();

        ctx->running = false;
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    {
        char ts[32];
        build_timestamp(ts, sizeof(ts));
        print_lock();
        printf("%s [Client][Ch%d] Connected to %s:%u\n",
            ts, ctx->ch_num, ctx->server_ip, ctx->port);
        print_unlock();
    }

    /* Receive loop */
    while (ctx->running)
    {
        uint8_t hdr[PKG_PLDLEN_SIZE];
        int n = recv_exact(ctx->sock, hdr, PKG_PLDLEN_SIZE, 200);

        if (n == -2) continue;       /* timeout, retry */
        if (n <= 0)                  /* disconnect or error */
        {
            char ts[32];
            build_timestamp(ts, sizeof(ts));

            print_lock();
            printf("%s [Client][Ch%d] Server disconnected.\n", ts, ctx->ch_num);
            print_unlock();
            break;
        }

        uint16_t payload_len = read_be16(hdr);

        if (payload_len == 0 || payload_len > sizeof(payload))
        {
            char ts[32];
            build_timestamp(ts, sizeof(ts));

            print_lock();
            printf("%s [Client][Ch%d RX] Error: invalid payload length %u.\n",
                ts, ctx->ch_num, payload_len);
            print_unlock();
            break;
        }

        n = recv_exact(ctx->sock, payload, (int)payload_len, 200);
        if (n <= 0)
        {
            char ts[32];
            build_timestamp(ts, sizeof(ts));

            print_lock();
            printf("%s [Client][Ch%d RX] Error: failed to read payload (%u expected).\n",
                ts, ctx->ch_num, payload_len);
            print_unlock();
            break;
        }

        parse_and_print_package(ctx->ch_num, payload, payload_len);
    }

    send_lock();
    close_socket(ctx->sock);
    ctx->sock = socket_invalid;
    send_lock();
    ctx->running = false;

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* TX thread — send demo message per-channel (full-duplex on same socket) */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static DWORD WINAPI tx_thread_func(LPVOID arg)
#else
static void* tx_thread_func(void* arg)
#endif
{
    ChannelCtx* ctx = (ChannelCtx*)arg;

    /* Buffer: header(6) + one frame: id(4)+len(1)+data(64 max) */
    uint8_t pkg[PKG_HDR_SIZE + 4 + 1 + 64];

    while (ctx->running)
    {
        if (!ctx->tx_enabled || ctx->sock == socket_invalid
            || ctx->tx_dlc == 0 || ctx->tx_cycle_ms == 0)
        {
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50 * 1000);
#endif
            continue;
        }

        ctx->tx_round++;

        /* Build packet:
         *   [payload_len : 2 BE]
         *   [round_count : 4 BE]
         *   [can_id      : 4 BE]
         *   [dlc         : 1   ]
         *   [data        : dlc ]
         */
        uint16_t payload_len = (uint16_t)(4 + 4 + 1 + ctx->tx_dlc);
        write_be16(pkg, payload_len);
        write_be32(pkg + 2, ctx->tx_round);

        write_be32(pkg + PKG_HDR_SIZE, ctx->tx_can_id);
        pkg[PKG_HDR_SIZE + 4] = (uint8_t)ctx->tx_dlc;
        memcpy(pkg + PKG_HDR_SIZE + 5, ctx->tx_data, ctx->tx_dlc);

        int total_len = (int)(PKG_PLDLEN_SIZE + payload_len);

        /* Send (with lock for full-duplex safety) */
        send_lock();
        int rc = send_all(ctx->sock, pkg, (size_t)total_len);
        send_unlock();

        /* Log */
        char ts[32];
        build_timestamp(ts, sizeof(ts));

        print_lock();

        if (rc == 0)
        {
            /* Console: same format as RX summary */
            printf("%s [Client][Ch%d TX] Round=%-6u |  1 msg(s) | %4d bytes | IDs: 0x%X\n",
                ts, ctx->ch_num, (unsigned)ctx->tx_round,
                total_len, (unsigned)ctx->tx_can_id);

            /* File: summary + detail (no [Client] prefix) */
            if (s_rx_log != NULL)
            {
                fprintf(s_rx_log,
                    "%s [Ch%d TX] Round=%-6u |  1 msg(s) | %4d bytes\n",
                    ts, ctx->ch_num, (unsigned)ctx->tx_round, total_len);

                fprintf(s_rx_log, "    0x%04X | %2u |",
                    (unsigned)(ctx->tx_can_id & 0xFFFFu),
                    (unsigned)ctx->tx_dlc);

                for (int j = 0; j < (int)ctx->tx_dlc; j++)
                    fprintf(s_rx_log, " %02X", ctx->tx_data[j]);

                fprintf(s_rx_log, "\n");
                fflush(s_rx_log);
            }
        }
        else
        {
            printf("%s [Client][Ch%d TX] Send failed (rc=%d)\n",
                ts, ctx->ch_num, rc);
            print_last_error("[Client] send_all failed");
        }

        print_unlock();

        /* Sleep theo cycle riêng của channel */
#ifdef _WIN32
        Sleep((DWORD)ctx->tx_cycle_ms);
#else
        usleep((useconds_t)ctx->tx_cycle_ms * 1000);
#endif
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Demo TX config: set per-channel TX params                           */
/*   - Ch1 (index 0): AVN1S04  ID=0x038E DLC=8 cycle=1000ms data=00..  */
/*   - Ch3 (index 2): AVN1S32  ID=0x017F DLC=8 cycle=100ms  data=03..  */
/* Other channels: TX disabled                                         */
/* ------------------------------------------------------------------ */
static void client_demo_init_tx_params(ChannelCtx* ctx, int ch_index)
{
    if (ctx == NULL) return;

    /* Default: TX disabled */
    ctx->tx_enabled = false;
    ctx->tx_round = 0;
    ctx->tx_can_id = 0;
    ctx->tx_dlc = 0;
    ctx->tx_cycle_ms = 0;
    memset(ctx->tx_data, 0, sizeof(ctx->tx_data));

    /* Ch1 = CANFD_G2M-1_BUS : AVN1S04 */
    if (ch_index == 0)
    {
        ctx->tx_enabled = true;
        ctx->tx_can_id = 0x0000038EU; /* AVN1S04 */
        ctx->tx_dlc = 8;
        ctx->tx_cycle_ms = 1000;
        memset(ctx->tx_data, 0x00, ctx->tx_dlc);
        return;
    }

    /* Ch3 = CANFD_G5M_BUS : AVN1S32 */
    if (ch_index == 2)
    {
        ctx->tx_enabled = true;
        ctx->tx_can_id = 0x0000017FU; /* AVN1S32 */
        ctx->tx_dlc = 8;
        ctx->tx_cycle_ms = 100;
        memset(ctx->tx_data, 0x03, ctx->tx_dlc);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv)
{
#ifdef _WIN32
    /* Escape sequence works reliably in Windows Terminal */
    printf("\033]0;Client\007");
    fflush(stdout);
    Sleep(500);
    resize_window("Client", 1000, 400);
#endif

    const char* ip = "127.0.0.1";
    const char* config_path = "config.ini";
    SimConfig   sim_cfg;
    int         ch_count;
    int         i;

    ChannelCtx ch[SIM_MAX_CHANNELS];
    memset(ch, 0, sizeof(ch));

    if (argc >= 2) ip = argv[1];
    if (argc >= 3) config_path = argv[2];

    if (net_init() != 0)
    {
        print_last_error("net_init() failed");
        return 1;
    }

    print_lock_init();

    /* RX log file */
    s_rx_log = fopen("log/client.log", "w");
    if (s_rx_log == NULL)
    {
        print_lock();
        printf("[Client] Warning: cannot open client.log\n");
        print_unlock();
    }

    /* ---- Load config to get ports ---- */
    if (sim_config_load(config_path, &sim_cfg) != 0)
    {
        print_lock();
        printf("[Client] Failed to load %s\n", config_path);
        print_unlock();

        if (s_rx_log) fclose(s_rx_log);
        print_lock_destroy();
        net_cleanup();
        return 1;
    }

    ch_count = sim_cfg.channel_count;

    print_lock();
    printf("[Client] === TCP Client (multi-channel) ===\n");
    printf("[Client] Server IP: %s | Channels: %d\n", ip, ch_count);
    for (i = 0; i < ch_count; i++)
    {
        printf("[Client]   Ch%d: port %u\n", i + 1, sim_cfg.channels[i].port);
    }

    printf("[Client] TX demo:\n");
    printf("[Client]   Ch1: AVN1S04  ID=0x038E DLC=8 cycle=1000ms\n");
    printf("[Client]   Ch3: AVN1S32  ID=0x017F DLC=8 cycle=100ms data=03..03\n");
    print_unlock();

    /* ---- Launch RX + TX threads ---- */
    for (i = 0; i < ch_count; i++)
    {
        ch[i].ch_num = i + 1;
        ch[i].server_ip = ip;
        ch[i].port = sim_cfg.channels[i].port;
        ch[i].running = true;
        ch[i].sock = socket_invalid;
        ch[i].tx_round = 0;

        client_demo_init_tx_params(&ch[i], i);

#ifdef _WIN32
        ch[i].rx_thread = CreateThread(NULL, 0, rx_thread_func, &ch[i], 0, NULL);
        if (ch[i].rx_thread == NULL)
        {
            print_lock();
            printf("[Client][Ch%d] Failed to create RX thread.\n", i + 1);
            print_unlock();
            ch[i].running = false;
            continue;
        }

        ch[i].tx_thread = CreateThread(NULL, 0, tx_thread_func, &ch[i], 0, NULL);
        if (ch[i].tx_thread == NULL)
        {
            print_lock();
            printf("[Client][Ch%d] Failed to create TX thread.\n", i + 1);
            print_unlock();
        }
#else
        if (pthread_create(&ch[i].rx_thread, NULL, rx_thread_func, &ch[i]) != 0)
        {
            print_lock();
            printf("[Client][Ch%d] Failed to create RX thread.\n", i + 1);
            print_unlock();
            ch[i].running = false;
            continue;
        }

        if (pthread_create(&ch[i].tx_thread, NULL, tx_thread_func, &ch[i]) != 0)
        {
            print_lock();
            printf("[Client][Ch%d] Failed to create TX thread.\n", i + 1);
            print_unlock();
        }
#endif
    }

    /* ---- Wait for user to press ENTER ---- */
    print_lock();
    printf("\n[Client] All channels started. Press ENTER to quit...\n");
    print_unlock();

#ifdef _WIN32
    while (1)
    {
        if (_kbhit())
        {
            int c = _getch();
            if (c == '\r' || c == '\n')
                break;
        }
        Sleep(50);
    }
#else
    getchar();
#endif

    /* ---- Signal all threads to stop ---- */
    for (i = 0; i < ch_count; i++)
    {
        ch[i].running = false;
    }

    /* ---- Wait for threads to finish ---- */
#ifdef _WIN32
    for (i = 0; i < ch_count; i++)
    {
        if (ch[i].rx_thread)
        {
            WaitForSingleObject(ch[i].rx_thread, 2000);
            CloseHandle(ch[i].rx_thread);
            ch[i].rx_thread = NULL;
        }
        if (ch[i].tx_thread)
        {
            WaitForSingleObject(ch[i].tx_thread, 2000);
            CloseHandle(ch[i].tx_thread);
            ch[i].tx_thread = NULL;
        }
        if (ch[i].sock != socket_invalid)
        {
            close_socket(ch[i].sock);
            ch[i].sock = socket_invalid;
        }
    }
#else
    for (i = 0; i < ch_count; i++)
    {
        pthread_join(ch[i].rx_thread, NULL);
        pthread_join(ch[i].tx_thread, NULL);
        if (ch[i].sock != socket_invalid)
        {
            close_socket(ch[i].sock);
            ch[i].sock = socket_invalid;
        }
    }
#endif

    if (s_rx_log != NULL)
    {
        fclose(s_rx_log);
        s_rx_log = NULL;
    }

    print_lock_destroy();
    net_cleanup();

    print_lock();
    printf("[Client] Stopped.\n");
    print_unlock();

    return 0;
}
