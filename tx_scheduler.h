/**
 * @file    tx_scheduler.h
 * @brief   TX Scheduler: time-based CAN message scheduling
 *          with dedicated thread.
 *
 * @details
 *  - Each channel has one TxScheduler instance.
 *  - Scheduler is "time-based", NOT tied to Sleep interval.
 *  - Thread polls every ~1ms but uses high-resolution timer
 *    to decide when to send messages.
 *
 *  Data flow:
 *      TxMsgList --> Scheduler --> CanMsgQueue --> TX Transmit
 *
 * @date    2026-04
 * @modified 2026-06
 */
#ifndef TX_SCHEDULER_H
#define TX_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include "tx_msg_reader.h"
#include "can_msg_queue.h"
#include "sim_mutex.h"

 /* ------------------------------------------------------------------ */
 /* Defines                                                             */
 /* ------------------------------------------------------------------ */

 /* Maximum channels supported by scheduler thread.
  * This is capacity only, actual channels = runtime config. */
#define TX_SCHED_THREAD_MAX_CH   8

  /* ------------------------------------------------------------------ */
  /* Per-message runtime state                                           */
  /* ------------------------------------------------------------------ */
typedef struct
{
    uint64_t last_sent_us;   /* Timestamp of last send (microseconds) */
} TxMsgState;

/* ------------------------------------------------------------------ */
/* TX Scheduler instance (one per channel, no thread)                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    TxMsgList* msg_list;          /* Pointer to TX message list (shared data) */
    TxMsgState   states[TX_MSG_MAX_MESSAGES]; /* Runtime state per message */
    CanMsgQueue* queue;             /* Output queue to TX transmit thread */
    bool         enabled;           /* Channel enable flag */

    /* Mutex protecting msg_list (shared with menu thread).
     * IMPORTANT:
     *   - This is NOT owned here.
     *   - It points to ctrl.tx_list_mutex[ch].
     */
    SimMutex* msg_list_mutex;

} TxScheduler;

/* ------------------------------------------------------------------ */
/* TX Scheduler Thread (handles all channels)                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    TxScheduler* scheds[TX_SCHED_THREAD_MAX_CH];

    /* Control layer data (read-only) */
    const bool* tx_enabled;     /* ctrl.tx_enabled[] */
    const bool* ch_connected;   /* ctrl.ch_connected[] */

    int           ch_count;
    volatile bool running;
    void* thread_handle;

} TxSchedulerThread;

/* ------------------------------------------------------------------ */
/* Scheduler logic API (pure logic)                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize scheduler.
 *
 * @param scheduler        Scheduler instance
 * @param msg_list         TX message list (shared)
 * @param msg_list_mutex   Mutex protecting msg_list
 * @param queue            Output queue
 */
void tx_scheduler_init(TxScheduler* scheduler,
    TxMsgList* msg_list,
    SimMutex* msg_list_mutex,
    CanMsgQueue* queue);

/**
 * @brief Scheduler tick (called ~1ms by thread).
 *
 * @details
 *  - Uses high-resolution timer internally.
 *  - Determines which messages are due for transmission.
 *  - Pushes messages to queue.
 *
 *  NOTE:
 *  - NOT tied to exact 1ms period.
 *  - Skip-to-now logic prevents burst sending.
 */
void tx_scheduler_tick(TxScheduler* scheduler);

/**
 * @brief Get current time (microseconds resolution).
 */
uint64_t tx_scheduler_get_time_us(void);

/* ------------------------------------------------------------------ */
/* Scheduler thread API                                                */
/* ------------------------------------------------------------------ */

void tx_scheduler_thread_init(TxSchedulerThread* ctx,
    TxScheduler        scheds[],
    int                ch_count,
    const bool* tx_enabled,
    const bool* ch_connected);

int tx_scheduler_thread_start(TxSchedulerThread* ctx);

void tx_scheduler_thread_stop(TxSchedulerThread* ctx);

#endif /* TX_SCHEDULER_H */