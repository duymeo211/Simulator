/**
 * @file    sim_trace.h
 * @brief   Simulator TX/RX detail trace (async queue) -> simulator_trace.log
 * @date    2026-05
 */
#ifndef SIM_TRACE_H
#define SIM_TRACE_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Defines                                                            */
/* ------------------------------------------------------------------ */
#define SIM_TRACE_LINE_MAX     512
#define SIM_TRACE_QUEUE_MAX    4096
#define SIM_TRACE_BYTES_LINE   32    /* bytes per line when dumping CANFD */

/* ------------------------------------------------------------------ */
/* Trace header format (shared between TX and RX)                     */
/* ------------------------------------------------------------------ */
#define TRACE_HEADER_FMT \
    "%s [Ch%d %s] Round=%-6u | %3d msg(s) | %4u bytes"

#define TRACE_HEADER(buf, size, ts, ch, dir, round, count, bytes) \
    snprintf(buf, size, TRACE_HEADER_FMT, ts, ch, dir, round, count, bytes)

/* ------------------------------------------------------------------ */
/* File rotation settings                                             */
/* ------------------------------------------------------------------ */
/* Total number of files kept, including the current active file.     */
/* Example with value 5:                                              */
/*   simulator_trace.log                                               */
/*   simulator_trace_01.log                                            */
/*   simulator_trace_02.log                                            */
/*   simulator_trace_03.log                                            */
/*   simulator_trace_04.log                                            */
/* ------------------------------------------------------------------ */
#define SIM_TRACE_ROTATE_MAX_SIZE   (20u * 1024u * 1024u)    /* 20 MB */
#define SIM_TRACE_ROTATE_MAX_FILES  5                  /* total files */

/* ------------------------------------------------------------------ */
/* Lifecycle API                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialize trace module (open file + start writer thread).
 * @param  file_path Output file path (e.g. "simulator_trace.log")
 * @return true on success, false on failure.
 */
bool sim_trace_init(const char *file_path);

/**
 * @brief Stop trace module (flush remaining queue + close).
 */
void sim_trace_stop(void);

/* ------------------------------------------------------------------ */
/* Write API (non-blocking for producer threads)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Push one pre-formatted trace line (async).
 *        NOTE: line must NOT include trailing '\n'.
 */
void sim_trace_write(const char *line);

/**
 * @brief Create timestamp string "[HH:MM:SS.mmm]".
 */
void sim_trace_timestamp(char *buf, int buf_size);

/**
 * @brief Format CAN frame with 4-space indent and multi-line for CANFD.
 *
 * Output example:
 *     0x00B8 | 64 | 00 00 ... (32 bytes)
 *                   00 00 ... (32 bytes)
 */
void sim_trace_format_can(char *out, int out_size,
                          uint32_t id, uint8_t dlc,
                          const uint8_t *data);

#endif /* SIM_TRACE_H */