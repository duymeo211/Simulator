/**
 * @file    sim_trace.c
 * @brief   Simulator trace implementation (async queue + writer thread).
 *          Supports size-based log rotation:
 *            - 20 MB per file
 *            - keep 5 files total (including current file)
 * @date    2026-05
 */
#include "sim_trace.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#endif

 /* ================================================================== */
 /* Internal types                                                     */
 /* ================================================================== */
typedef struct
{
    char text[SIM_TRACE_LINE_MAX];
} TraceEntry;

typedef struct
{
    TraceEntry entries[SIM_TRACE_QUEUE_MAX];
    int head;
    int tail;
    int count;

#ifdef _WIN32
    CRITICAL_SECTION lock;
    HANDLE           thread;
    HANDLE           event;
#else
    pthread_mutex_t  lock;
    pthread_t        thread;
    pthread_cond_t   cond;
#endif
    volatile bool    running;
} TraceQueue;

/* ================================================================== */
/* Static variables                                                   */
/* ================================================================== */
static FILE* s_trace = NULL;
static TraceQueue s_q = { 0 };

/* Current trace file path (e.g. "simulator_trace.log") */
static char       s_trace_path[260] = { 0 };

/* Current active file size in bytes */
static uint32_t   s_trace_size = 0;

/* ================================================================== */
/* Internal helpers                                                   */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Build rotated file name                                            */
/* Example:                                                           */
/*   base = "simulator_trace.log", idx = 1                             */
/*   out  = "simulator_trace_01.log"                                   */
/* ------------------------------------------------------------------ */
static void build_rotated_name(const char* base_path, int idx,
    char* out, size_t out_size)
{
    const char* dot;
    size_t      base_len;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';

    if (base_path == NULL || base_path[0] == '\0') return;

    dot = strrchr(base_path, '.');

    /* If no extension, just append suffix */
    if (dot == NULL)
    {
        snprintf(out, out_size, "%s_%02d", base_path, idx);
        return;
    }

    base_len = (size_t)(dot - base_path);

    snprintf(out, out_size, "%.*s_%02d%s",
        (int)base_len, base_path, idx, dot);
}

/* ------------------------------------------------------------------ */
/* Rotate trace files                                                 */
/* Keeps exactly SIM_TRACE_ROTATE_MAX_FILES total files               */
/* including the current active file.                                 */
/* ------------------------------------------------------------------ */
static void rotate_trace_files(void)
{
    int  idx;
    char old_name[300];
    char new_name[300];

    if (s_trace != NULL)
    {
        fflush(s_trace);
        fclose(s_trace);
        s_trace = NULL;
    }

    /* Delete the oldest history file */
    if (SIM_TRACE_ROTATE_MAX_FILES >= 2)
    {
        build_rotated_name(s_trace_path,
            SIM_TRACE_ROTATE_MAX_FILES - 1,
            old_name, sizeof(old_name));
        remove(old_name);
    }

    /* Shift history files: _03 -> _04, _02 -> _03, _01 -> _02 */
    for (idx = SIM_TRACE_ROTATE_MAX_FILES - 2; idx >= 1; idx--)
    {
        build_rotated_name(s_trace_path, idx, old_name, sizeof(old_name));
        build_rotated_name(s_trace_path, idx + 1, new_name, sizeof(new_name));

        remove(new_name);      /* safe on Windows */
        rename(old_name, new_name);
    }

    /* Current -> _01 */
    build_rotated_name(s_trace_path, 1, new_name, sizeof(new_name));
    remove(new_name);
    rename(s_trace_path, new_name);

    /* Open new current file */
    s_trace = fopen(s_trace_path, "w");
    s_trace_size = 0;
}

/* ------------------------------------------------------------------ */
/* Write one line to active trace file (called only by writer thread) */
/* Handles size-based rotation before writing.                        */
/* ------------------------------------------------------------------ */
static void trace_write_line(const char* line)
{
    uint32_t line_len;

    if (s_trace == NULL || line == NULL) return;

    line_len = (uint32_t)strlen(line) + 1u;   /* include '\n' */

    /* Rotate before writing the line that would exceed the limit */
    if ((s_trace_size + line_len) > SIM_TRACE_ROTATE_MAX_SIZE)
    {
        rotate_trace_files();
        if (s_trace == NULL) return;
    }

    fprintf(s_trace, "%s\n", line);
    s_trace_size += line_len;
}

/* ================================================================== */
/* Timestamp                                                          */
/* ================================================================== */
void sim_trace_timestamp(char* buf, int buf_size)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, (size_t)buf_size, "[%02u:%02u:%02u.%03u]",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm tm_info;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);
    snprintf(buf, (size_t)buf_size, "[%02d:%02d:%02d.%03ld]",
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        ts.tv_nsec / 1000000L);
#endif
}

/* ================================================================== */
/* Queue helpers                                                      */
/* ================================================================== */
static void trace_queue_push(const char* line)
{
    if (line == NULL) return;

#ifdef _WIN32
    EnterCriticalSection(&s_q.lock);
#else
    pthread_mutex_lock(&s_q.lock);
#endif

    if (s_q.count < SIM_TRACE_QUEUE_MAX)
    {
        strncpy(s_q.entries[s_q.tail].text, line, SIM_TRACE_LINE_MAX - 1);
        s_q.entries[s_q.tail].text[SIM_TRACE_LINE_MAX - 1] = '\0';
        s_q.tail = (s_q.tail + 1) % SIM_TRACE_QUEUE_MAX;
        s_q.count++;
    }

#ifdef _WIN32
    LeaveCriticalSection(&s_q.lock);
    SetEvent(s_q.event);
#else
    pthread_cond_signal(&s_q.cond);
    pthread_mutex_unlock(&s_q.lock);
#endif
}

static bool trace_queue_pop(TraceEntry* out)
{
    bool ok = false;

#ifdef _WIN32
    EnterCriticalSection(&s_q.lock);
#else
    pthread_mutex_lock(&s_q.lock);
#endif

    if (s_q.count > 0)
    {
        *out = s_q.entries[s_q.head];
        s_q.head = (s_q.head + 1) % SIM_TRACE_QUEUE_MAX;
        s_q.count--;
        ok = true;
    }

#ifdef _WIN32
    LeaveCriticalSection(&s_q.lock);
#else
    pthread_mutex_unlock(&s_q.lock);
#endif

    return ok;
}

/* ================================================================== */
/* Writer thread                                                      */
/* ================================================================== */
#ifdef _WIN32
static DWORD WINAPI trace_thread_func(LPVOID arg)
#else
static void* trace_thread_func(void* arg)
#endif
{
    (void)arg;

    while (s_q.running || s_q.count > 0)
    {
#ifdef _WIN32
        WaitForSingleObject(s_q.event, 100);
#else
        pthread_mutex_lock(&s_q.lock);
        pthread_cond_wait(&s_q.cond, &s_q.lock);
        pthread_mutex_unlock(&s_q.lock);
#endif

        /* Batch drain */
        {
            TraceEntry e;
            while (trace_queue_pop(&e))
            {
                trace_write_line(e.text);
            }

            if (s_trace != NULL)
            {
                fflush(s_trace);
            }
        }
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */
bool sim_trace_init(const char* file_path)
{
    if (file_path == NULL) return false;

    memset(s_trace_path, 0, sizeof(s_trace_path));
    strncpy(s_trace_path, file_path, sizeof(s_trace_path) - 1);

    s_trace = fopen(s_trace_path, "w");
    if (s_trace == NULL) return false;

    s_trace_size = 0;
    memset(&s_q, 0, sizeof(s_q));

#ifdef _WIN32
    InitializeCriticalSection(&s_q.lock);
    s_q.event = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
    pthread_mutex_init(&s_q.lock, NULL);
    pthread_cond_init(&s_q.cond, NULL);
#endif

    s_q.running = true;

#ifdef _WIN32
    s_q.thread = CreateThread(NULL, 0, trace_thread_func, NULL, 0, NULL);
    if (s_q.thread == NULL) return false;
#else
    pthread_create(&s_q.thread, NULL, trace_thread_func, NULL);
#endif

    return true;
}

void sim_trace_stop(void)
{
    s_q.running = false;

#ifdef _WIN32
    SetEvent(s_q.event);
    if (s_q.thread)
    {
        WaitForSingleObject(s_q.thread, 3000);
        CloseHandle(s_q.thread);
    }
    if (s_q.event)
    {
        CloseHandle(s_q.event);
    }
    DeleteCriticalSection(&s_q.lock);
#else
    pthread_cond_signal(&s_q.cond);
    pthread_join(s_q.thread, NULL);
    pthread_mutex_destroy(&s_q.lock);
    pthread_cond_destroy(&s_q.cond);
#endif

    /* Final drain */
    if (s_trace != NULL)
    {
        TraceEntry e;
        while (trace_queue_pop(&e))
        {
            trace_write_line(e.text);
        }

        fflush(s_trace);
        fclose(s_trace);
        s_trace = NULL;
    }
}

void sim_trace_write(const char* line)
{
    trace_queue_push(line);
}

/* ------------------------------------------------------------------ */
/* Format CAN frame (4-space indent, 32 bytes/line)                   */
/* ------------------------------------------------------------------ */
void sim_trace_format_can(char* out, int out_size,
    uint32_t id, uint8_t dlc,
    const uint8_t* data)
{
    int pos = 0;
    int i;

    if (out == NULL || out_size <= 0) return;

    /* Header: 4-space indent */
    pos += snprintf(out + pos, (size_t)(out_size - pos),
        "    0x%04X | %2u | ", id, (unsigned)dlc);

    if (data == NULL || dlc == 0) return;

    for (i = 0; i < (int)dlc && pos < out_size - 4; i++)
    {
        if (i > 0 && (i % SIM_TRACE_BYTES_LINE) == 0)
        {
            /* 18 spaces aligns with data column */
            pos += snprintf(out + pos, (size_t)(out_size - pos),
                "\n                  ");
        }

        pos += snprintf(out + pos, (size_t)(out_size - pos),
            "%02X%s", data[i],
            (i == (int)dlc - 1) ? "" : " ");
    }
}
