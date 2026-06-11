/**
 * @file    can_msg_queue.c
 * @brief   Thread-safe circular queue for CAN messages.
 *
 *          Lock strategy: CriticalSection (Windows) / pthread_mutex (POSIX).
 *          All public functions acquire lock before accessing shared state.
 *
 * @date    2026-04
 */
#include "can_msg_queue.h"
#include <string.h>
#include <stdio.h>

 /* ================================================================== */
 /* Init / Destroy                                                      */
 /* ================================================================== */

void can_msg_queue_init(CanMsgQueue* q)
{
    if (q == NULL) return;

    memset(q, 0, sizeof(*q));

#ifdef _WIN32
    InitializeCriticalSection(&q->lock);
#else
    pthread_mutex_init(&q->lock, NULL);
#endif
}

void can_msg_queue_destroy(CanMsgQueue* q)
{
    if (q == NULL) return;

#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_mutex_destroy(&q->lock);
#endif
}

/* ================================================================== */
/* Push — Producer (TX Scheduler thread)                               */
/* ================================================================== */

bool can_msg_queue_push(CanMsgQueue* q, const CanMsgEntry* entry)
{
    bool success = false;

    if (q == NULL || entry == NULL) return false;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < CAN_MSG_QUEUE_MAX)
    {
        memcpy(&q->entries[q->tail], entry, sizeof(CanMsgEntry));
        q->tail = (q->tail + 1) % CAN_MSG_QUEUE_MAX;
        q->count++;
        success = true;
    }
    else
    {
        fprintf(stderr, "[CanMsgQueue] Warning: queue full, message dropped.\n");
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    return success;
}

/* ================================================================== */
/* Pop — Consumer (TX Transmit thread)                                 */
/* ================================================================== */

bool can_msg_queue_pop(CanMsgQueue* q, CanMsgEntry* out_entry)
{
    bool success = false;

    if (q == NULL || out_entry == NULL) return false;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count > 0)
    {
        memcpy(out_entry, &q->entries[q->head], sizeof(CanMsgEntry));
        q->head = (q->head + 1) % CAN_MSG_QUEUE_MAX;
        q->count--;
        success = true;
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    return success;
}

/* ================================================================== */
/* Count — thread-safe read                                            */
/* ================================================================== */

int can_msg_queue_count(CanMsgQueue* q)
{
    int count = 0;

    if (q == NULL) return 0;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    count = q->count;

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    return count;
}