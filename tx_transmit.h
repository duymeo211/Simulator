/**
 * @file    tx_transmit.h
 * @brief   TX Transmit module: multi-channel CAN-to-TCP transmitter.
 * @date    2026-04
 */
#ifndef TX_TRANSMIT_H
#define TX_TRANSMIT_H

#include <stdint.h>
#include <stdbool.h>
#include "can_msg_queue.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

 /* ------------------------------------------------------------------ */
 /* Configuration                                                       */
 /* ------------------------------------------------------------------ */
#define TX_MAX_CHANNELS          3      /* max CAN channels              */
#define TCP_PACKAGE_MAX_LEN      1460   /* max TCP payload (1 Ethernet MSS) */

/* ------------------------------------------------------------------ */
/* TCP package layout (vCAN protocol):                                 */
/*                                                                     */
/*   [ Payload Length  : 2 bytes BE             ]                      */
/*   [ Round Count     : 4 bytes BE             ]                      */
/*   [ CAN Frame 1     : id(4)+len(1)+data      ]                     */
/*   [ CAN Frame 2     : id(4)+len(1)+data      ]                     */
/*   ...                                                               */
/*   Total <= TCP_PACKAGE_MAX_LEN bytes                                */
/* ------------------------------------------------------------------ */
#define TCP_PKG_PLDLEN_SIZE      2      /* Payload Length field (bytes)     */
#define TCP_PKG_ROUND_SIZE       4      /* Round Count field (bytes)        */
#define TCP_PKG_HEADER_SIZE      (TCP_PKG_PLDLEN_SIZE + TCP_PKG_ROUND_SIZE)

#define CAN_FRAME_MAX_ENCODED    (4u + 1u + CAN_MSG_MAX_DATA)

/* ------------------------------------------------------------------ */
/* TCP package buffer                                                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    uint8_t  data[TCP_PACKAGE_MAX_LEN];
    uint16_t len;
    int      msg_count;
    bool     in_use;
} TcpPackageBuffer;

/* ------------------------------------------------------------------ */
/* Send callback                                                       */
/* ------------------------------------------------------------------ */
typedef bool (*TxTransmitSendFn)(const uint8_t* data,
    uint16_t       len,
    void* user);

/* ------------------------------------------------------------------ */
/* Per-channel transmit context                                        */
/* ------------------------------------------------------------------ */
typedef struct
{
    CanMsgQueue* queue;
    TxTransmitSendFn  send_fn;
    void* send_user;
    uint32_t          round_count;
    bool              enabled;
} TxChannelCtx;

/* ------------------------------------------------------------------ */
/* TX Transmit instance (multi-channel, single thread)                 */
/* ------------------------------------------------------------------ */
typedef struct
{
    TxChannelCtx  channels[TX_MAX_CHANNELS];
    int           channel_count;
    int           interval_ms;
    bool          running;

#ifdef _WIN32
    HANDLE        thread;
#else
    pthread_t     thread;
#endif
} TxTransmit;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize TX Transmit instance.
 * @param tx             Transmit instance.
 * @param channel_count  Number of channels (1..TX_MAX_CHANNELS).
 * @param interval_ms    Thread wake interval in milliseconds.
 */
void tx_transmit_init(TxTransmit* tx, int channel_count, int interval_ms);

/**
 * @brief Configure one channel.
 * @param ch  Channel index (0-based).
 */
void tx_transmit_set_channel(TxTransmit* tx,
    int               ch,
    CanMsgQueue* queue,
    TxTransmitSendFn   send_fn,
    void* send_user);

int  tx_transmit_start(TxTransmit* tx);
void tx_transmit_stop(TxTransmit* tx);

#endif /* TX_TRANSMIT_H */