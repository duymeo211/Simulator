/**
 * @file    sim_log.c
 * @brief   Simulator system log implementation (low-volume, thread-safe).
 * @date    2026-05
 */
#include "sim_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

 /* ================================================================== */
 /* Static variables                                                   */
 /* ================================================================== */
static FILE* s_sys = NULL;

#ifdef _WIN32
static CRITICAL_SECTION s_lock;
static bool s_lock_inited = false;
#else
static pthread_mutex_t s_lock;
static bool s_lock_inited = false;
#endif

/* ================================================================== */
/* Internal helpers                                                   */
/* ================================================================== */
static void sys_timestamp(char* buf, size_t n)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "[%02u:%02u:%02u.%03u]",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm tm_info;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);
    snprintf(buf, n, "[%02d:%02d:%02d.%03ld]",
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        ts.tv_nsec / 1000000L);
#endif
}

static void sys_write(const char* lv, const char* fmt, va_list ap)
{
    if (s_sys == NULL) return;

    char ts[32];
    char msg[512];

    sys_timestamp(ts, sizeof(ts));
    vsnprintf(msg, sizeof(msg), fmt, ap);

#ifdef _WIN32
    EnterCriticalSection(&s_lock);
#else
    pthread_mutex_lock(&s_lock);
#endif

    fprintf(s_sys, "%s [%s] %s\n", ts, lv, msg);
    fflush(s_sys);

#ifdef _WIN32
    LeaveCriticalSection(&s_lock);
#else
    pthread_mutex_unlock(&s_lock);
#endif
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */
bool sim_log_init(const char* file_path)
{
    if (file_path == NULL) return false;

    s_sys = fopen(file_path, "w");
    if (s_sys == NULL) return false;

#ifdef _WIN32
    InitializeCriticalSection(&s_lock);
#else
    pthread_mutex_init(&s_lock, NULL);
#endif
    s_lock_inited = true;

    sim_log_info("System log initialized");
    return true;
}

void sim_log_stop(void)
{
    if (s_sys)
    {
        sim_log_info("System log stopped");
        fclose(s_sys);
        s_sys = NULL;
    }

    if (s_lock_inited)
    {
#ifdef _WIN32
        DeleteCriticalSection(&s_lock);
#else
        pthread_mutex_destroy(&s_lock);
#endif
        s_lock_inited = false;
    }
}

void sim_log_info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sys_write("INFO", fmt, ap);
    va_end(ap);
}

void sim_log_warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sys_write("WARN", fmt, ap);
    va_end(ap);
}

void sim_log_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sys_write("ERROR", fmt, ap);
    va_end(ap);
}