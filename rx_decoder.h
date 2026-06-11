/**
 * @file    rx_decoder.h
 * @brief   RX Decoder: streaming TCP decoder for vCAN packages (length-prefixed).
 *          - rx_debug.log: summary per TCP package (realtime tail)
 *          - simulator_trace.log: full RX detail (CAN frames) via sim_trace (async)
 * @date    2026-05
 */
#ifndef RX_DECODER_H
#define RX_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Size of internal streaming buffer (must hold worst-case burst). */
#ifndef RX_DECODER_BUF_SIZE
#define RX_DECODER_BUF_SIZE   8192
#endif

/* Max IDs printed per package (default). */
#ifndef RX_DECODER_MAX_IDS
#define RX_DECODER_MAX_IDS    10
#endif

    typedef struct
    {
        int      ch_num;        /* 1-based for display */
        FILE* log;           /* rx_debug.log handle (opened by caller) */
        int      max_ids;       /* max CAN IDs to print per package */
        uint8_t  buf[RX_DECODER_BUF_SIZE];
        size_t   used;

        /* Stats (optional, for debug) */
        uint32_t pkgs_ok;
        uint32_t pkgs_err;
        uint32_t bytes_drop;
    } RxDecoder;

    /**
     * @brief Initialize decoder.
     * @param dec      Decoder instance.
     * @param ch_num   1-based channel number for display.
     * @param log      Log file handle (can be NULL to disable logging).
     * @param max_ids  Max number of CAN IDs to print per package (<=0 uses default).
     */
    void rx_decoder_init(RxDecoder* dec, int ch_num, FILE* log, int max_ids);

    /**
     * @brief Feed bytes from TCP stream into decoder.
     *        Can handle partial packets and multiple packets per call.
     */
    void rx_decoder_feed(RxDecoder* dec, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RX_DECODER_H */
