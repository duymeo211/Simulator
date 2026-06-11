/**
 * @file    tx_scheduler.c
 * @brief   TX Scheduler: time-based CAN message scheduling
 *
 * @details
 *  Design:
 *  - Thread uses Sleep(1) for polling (~1ms tick)
 *  - High-resolution timer (QueryPerformanceCounter) determines timing
 *  - Scheduler is independent of Sleep accuracy
 *
 *  Critical section:
 *  - msg_list is shared with menu thread
 *  - MUST lock when reading message content
 *
 * @date    2026-04
 * @modified 2026-06
 */

#include "tx_scheduler.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")
#else
#include <time.h>
#include <unistd.h>
#endif

 /* ================================================================== */
 /* High-resolution timer                                              */
 /* ================================================================== */

 /**
  * @brief Get current time in microseconds.
  *
  * Uses high-resolution performance counter.
  */
uint64_t tx_scheduler_get_time_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart * 1000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL);
#endif
}

/* ================================================================== */
/* Scheduler init                                                     */
/* ================================================================== */

void tx_scheduler_init(TxScheduler* scheduler,
    TxMsgList* msg_list,
    SimMutex* msg_list_mutex,
    CanMsgQueue* queue)
{
    uint64_t now;
    int i;

    if (scheduler == NULL || msg_list == NULL || queue == NULL)
    {
        return;
    }

    memset(scheduler, 0, sizeof(*scheduler));

    scheduler->msg_list = msg_list;
    scheduler->msg_list_mutex = msg_list_mutex;
    scheduler->queue = queue;
    scheduler->enabled = false;

    now = tx_scheduler_get_time_us();

    /* Initialize timers so first send happens AFTER one full cycle */
    for (i = 0; i < msg_list->message_count; i++)
    {
        scheduler->states[i].last_sent_us = now;
    }
}

/* ================================================================== */
/* Scheduler tick                                                     */
/* ================================================================== */

/**
 * @brief Scheduler tick (called ~1ms)
 *
 * Key idea:
 *  - Copy message under lock (very short critical section)
 *  - Process outside lock (avoid blocking menu thread)
 */
void tx_scheduler_tick(TxScheduler* scheduler)
{
    uint64_t now;
    int i;

    if (scheduler == NULL || !scheduler->enabled)
    {
        return;
    }

    /* Read time once per tick */
    now = tx_scheduler_get_time_us();

    for (i = 0; i < scheduler->msg_list->message_count; i++)
    {
        TxMsg msg;   /* LOCAL COPY (important!) */
        TxMsgState* state;
        uint64_t cycle_us;
        uint64_t elapsed;
        CanMsgEntry entry;

        /* ---------------------------------------------------------- */
        /* CRITICAL SECTION: copy shared message                      */
        /* ---------------------------------------------------------- */
        /* WHY:
         *  - msg_list is shared with menu thread (user edit)
         *  - without lock → data corruption possible
         *
         * DESIGN:
         *  - lock only for memcpy
         *  - do NOT keep lock during processing
         */
        if (scheduler->msg_list_mutex != NULL)
        {
            sim_mutex_lock(scheduler->msg_list_mutex);
        }

        msg = scheduler->msg_list->messages[i];

        if (scheduler->msg_list_mutex != NULL)
        {
            sim_mutex_unlock(scheduler->msg_list_mutex);
        }

        /* ---------------------------------------------------------- */
        /* Time calculation                                           */
        /* ---------------------------------------------------------- */
        state = &scheduler->states[i];
        cycle_us = (uint64_t)msg.cycle_ms * 1000ULL;
        elapsed = now - state->last_sent_us;

        if (cycle_us == 0)
        {
            continue; /* skip non-cyclic messages */
        }

        if (elapsed < cycle_us)
        {
            continue; /* not yet due */
        }

        /* ---------------------------------------------------------- */
        /* Build CAN entry                                            */
        /* ---------------------------------------------------------- */
        entry.id = msg.id;
        entry.length = msg.length;
        memcpy(entry.data, msg.data, msg.length);

        /* Push to TX queue */
        if (!can_msg_queue_push(scheduler->queue, &entry))
        {
            fprintf(stderr,
                "[TX Scheduler] Warning: queue full, msg '%s' (ID=0x%04X) skipped.\n",
                msg.name, msg.id);
        }

        /* ---------------------------------------------------------- */
        /* Update timing                                              */
        /* ---------------------------------------------------------- */

        /* Advance by one cycle (avoid drift) */
        state->last_sent_us += cycle_us;

        /* Guard: if delayed too much, skip forward */
        if (now - state->last_sent_us > cycle_us)
        {
            state->last_sent_us = now;
        }
    }
}

/* ================================================================== */
/* Scheduler thread                                                   */
/* ================================================================== */

#ifdef _WIN32
static DWORD WINAPI tx_scheduler_thread_func(LPVOID param)
{
    TxSchedulerThread* ctx = (TxSchedulerThread*)param;
    int i;

    /* Improve Sleep(1) resolution (~1ms instead of ~16ms) */
    timeBeginPeriod(1);

    printf("[Scheduler Thread] Started (Sleep=1ms, time-based)\n");

    while (ctx->running)
    {
        for (i = 0; i < ctx->ch_count; i++)
        {
            /* Sync enable flag from control layer */
            ctx->scheds[i]->enabled =
                ctx->tx_enabled[i] && ctx->ch_connected[i];

            /* Run scheduling logic */
            tx_scheduler_tick(ctx->scheds[i]);
        }

        /* Polling interval (NOT timing source) */
        Sleep(1);
    }

    timeEndPeriod(1);

    printf("[Scheduler Thread] Stopped\n");
    return 0;
}
#endif

void tx_scheduler_thread_init(TxSchedulerThread* ctx,
    TxScheduler        scheds[],
    int                ch_count,
    const bool* tx_enabled,
    const bool* ch_connected)
{
    int i;

    if (ctx == NULL) return;

    memset(ctx, 0, sizeof(*ctx));

    ctx->ch_count = (ch_count > TX_SCHED_THREAD_MAX_CH)
        ? TX_SCHED_THREAD_MAX_CH : ch_count;

    ctx->tx_enabled = tx_enabled;
    ctx->ch_connected = ch_connected;
    ctx->running = false;

    for (i = 0; i < ctx->ch_count; i++)
    {
        ctx->scheds[i] = &scheds[i];
    }
}

int tx_scheduler_thread_start(TxSchedulerThread* ctx)
{
    if (ctx == NULL) return -1;

    ctx->running = true;

#ifdef _WIN32
    ctx->thread_handle = CreateThread(
        NULL, 0, tx_scheduler_thread_func, ctx, 0, NULL);

    if (ctx->thread_handle == NULL)
    {
        ctx->running = false;
        fprintf(stderr, "[Scheduler Thread] CreateThread failed\n");
        return -1;
    }
#endif

    return 0;
}

void tx_scheduler_thread_stop(TxSchedulerThread* ctx)
{
    if (ctx == NULL) return;

    ctx->running = false;

#ifdef _WIN32
    if (ctx->thread_handle != NULL)
    {
        WaitForSingleObject((HANDLE)ctx->thread_handle, 2000);
        CloseHandle((HANDLE)ctx->thread_handle);
        ctx->thread_handle = NULL;
    }
#endif
}