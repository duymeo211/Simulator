/**
 * @file    can_msg_queue.h
 * @brief   Thread-safe circular queue for CAN messages.
 *
 *          Producer: TX Scheduler thread  (push)
 *          Consumer: TX Transmit thread   (pop)
 *
 *          Uses CriticalSection (Windows) or pthread_mutex (POSIX)
 *          for thread safety.
 *
 * @date    2026-04
 */
#ifndef CAN_MSG_QUEUE_H
#define CAN_MSG_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

 /* ------------------------------------------------------------------ */
 /* Defines                                                             */
 /* ------------------------------------------------------------------ */
#define CAN_MSG_QUEUE_MAX    300   /* max messages in queue       */
#define CAN_MSG_MAX_DATA     64    /* CAN FD max payload (bytes)  */

/* ------------------------------------------------------------------ */
/* CAN message entry                                                   */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint32_t id;                      /* CAN message ID             */
    uint8_t  length;                  /* actual data bytes (0..64)  */
    uint8_t  data[CAN_MSG_MAX_DATA];  /* payload                    */
} CanMsgEntry;

/* ------------------------------------------------------------------ */
/* Thread-safe circular queue                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    CanMsgEntry entries[CAN_MSG_QUEUE_MAX];
    int         head;     /* index to read  (consumer) */
    int         tail;     /* index to write (producer) */
    int         count;    /* number of items currently in queue */

#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t  lock;
#endif
} CanMsgQueue;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize the queue and its mutex.
 *        Must be called before any thread accesses the queue.
 */
void can_msg_queue_init(CanMsgQueue* q);

/**
 * @brief Destroy the queue and release mutex resources.
 *        Must be called after all threads have stopped.
 */
void can_msg_queue_destroy(CanMsgQueue* q);

/**
 * @brief Push one CAN message into the queue.
 *        Called by TX Scheduler thread (producer).
 * @return true if pushed, false if queue full.
 */
bool can_msg_queue_push(CanMsgQueue* q, const CanMsgEntry* entry);

/**
 * @brief Pop one CAN message from the queue.
 *        Called by TX Transmit thread (consumer).
 * @return true if popped, false if queue empty.
 */
bool can_msg_queue_pop(CanMsgQueue* q, CanMsgEntry* out_entry);

/**
 * @brief Return number of messages currently in queue (thread-safe).
 */
int can_msg_queue_count(CanMsgQueue* q);

#endif /* CAN_MSG_QUEUE_H */