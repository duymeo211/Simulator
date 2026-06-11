/**
 * @file    rx_decoder.c
 * @brief   RX Decoder: streaming TCP decoder for vCAN packages (length-prefixed).
 *          - rx_debug.log: summary per package (realtime tail)
 *          - simulator_trace.log: RX detail (CAN frames) via sim_trace (async)
 * @date    2026-05
 */
#include "rx_decoder.h"

#include "sim_trace.h"
#include "sim_log.h"

#include <string.h>
#include <stdio.h>


 /* ================================================================== */
 /* Helpers                                                             */
 /* ================================================================== */

static inline uint16_t read_be16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}

static int rx_is_debug_target_id(uint32_t can_id)
{
    return (can_id == 0x38Eu ||
        can_id == 0x3E7u ||
        can_id == 0x58Au ||
        can_id == 0x3C0u);
}

static void rx_append_debug_line(char* debug_lines,
    size_t debug_lines_size,
    uint32_t can_id,
    uint8_t dlc,
    const uint8_t* data)
{
    int pos;
    int i;
    int written;

    if (debug_lines == NULL || debug_lines_size == 0 || data == NULL)
        return;

    pos = (int)strlen(debug_lines);
    if (pos >= (int)debug_lines_size - 1)
        return;

    written = snprintf(debug_lines + pos,
        debug_lines_size - (size_t)pos,
        "      0x%04X | %2u |",
        can_id, dlc);
    if (written < 0)
        return;

    pos += written;
    if (pos >= (int)debug_lines_size - 1)
        return;

    for (i = 0; i < (int)dlc; i++)
    {
        written = snprintf(debug_lines + pos,
            debug_lines_size - (size_t)pos,
            " %02X",
            data[i]);
        if (written < 0)
            return;

        pos += written;
        if (pos >= (int)debug_lines_size - 1)
            return;
    }

    if (pos < (int)debug_lines_size - 2)
    {
        debug_lines[pos++] = '\n';
        debug_lines[pos] = '\0';
    }
}

/* ================================================================== */
/* Internal: rx_debug.log output (summary, realtime)                   */
/* ================================================================== */

#define RX_DEBUG_MAX_LINES 50000
static unsigned int s_rx_line_count = 0;

static void rx_log_line(RxDecoder* dec, const char* line)
{
    if (dec == NULL || dec->log == NULL || line == NULL)
        return;

    /* Rotate by line count */
    if (s_rx_line_count >= RX_DEBUG_MAX_LINES)
    {
        freopen("log/rx_debug.log", "w", dec->log);
        s_rx_line_count = 0;

        fprintf(dec->log, "--- rx_debug.log reset (50k lines reached) ---\n");
    }

    fprintf(dec->log, "%s", line);
    fflush(dec->log);

    s_rx_line_count++;
}

/* ================================================================== */
/* Internal: trace output (simulator_trace.log, async)                  */
/* ================================================================== */

static void rx_trace_packet(int ch_num,
    uint32_t round_count,
    int can_count,
    uint32_t bytes_total,
    const char* ids_str,
    int truncated,
    const uint8_t* payload,
    uint16_t payload_len)
{
    char ts[32];
    char line[SIM_TRACE_LINE_MAX];
    int  offset;

    if (payload == NULL) return;

    /* Header line */
    sim_trace_timestamp(ts, (int)sizeof(ts));


    TRACE_HEADER(line, sizeof(line),
        ts, ch_num, "RX",
        round_count, can_count, bytes_total);

    /* Add IDs */
    snprintf(line + strlen(line),
        sizeof(line) - strlen(line),
        " | IDs: %s%s",
        (ids_str && ids_str[0]) ? ids_str : "(none)",
        truncated ? " ..." : "");

    sim_trace_write(line);

    /* Dump frames: payload = [round(4)] + frames... */
    offset = 4;

    while ((uint16_t)(offset + 5u) <= payload_len)
    {
        uint32_t can_id = read_be32(payload + offset);
        uint8_t  dlc;

        offset += 4;
        dlc = payload[offset];
        offset += 1;

        if ((uint16_t)(offset + dlc) > payload_len)
        {
            sim_log_warn("RX trace stopped: invalid frame length (Ch%d, Round=%u)",
                ch_num, round_count);
            break;
        }

        sim_trace_format_can(line, (int)sizeof(line),
            can_id, dlc, payload + offset);
        sim_trace_write(line);

        offset += dlc;
    }
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void rx_decoder_init(RxDecoder* dec, int ch_num, FILE* log, int max_ids)
{
    if (dec == NULL) return;

    memset(dec, 0, sizeof(*dec));

    dec->ch_num = (ch_num <= 0) ? 1 : ch_num;
    dec->log = log;
    dec->max_ids = (max_ids <= 0) ? RX_DECODER_MAX_IDS : max_ids;
    dec->used = 0;
}

/* ================================================================== */
/* Core streaming parser                                               */
/* Packet format (same as TX):                                         */
/*   [ Payload Length : 2 bytes BE ]  // bytes after this field        */
/*   [ Round Count    : 4 bytes BE ]                                   */
/*   [ CAN frames...  : id(4)+len(1)+data(len) repeated ]              */
/* Total packet bytes = 2 + payload_len                                */
/* ================================================================== */

void rx_decoder_feed(RxDecoder* dec, const uint8_t* data, size_t len)
{
    if (dec == NULL || data == NULL || len == 0) return;

    /* ------------------------------------------------------------------ */
    /* 1) Append into internal buffer with overflow handling               */
    /* ------------------------------------------------------------------ */
    if (dec->used + len > RX_DECODER_BUF_SIZE)
    {
        dec->pkgs_err++;
        dec->bytes_drop += (uint32_t)dec->used;
        dec->used = 0;

        sim_log_warn("RX buffer overflow: drop existing buffer (Ch%d)", dec->ch_num);

        /* If incoming chunk itself is bigger than buffer, keep only the tail */
        if (len > RX_DECODER_BUF_SIZE)
        {
            data = data + (len - RX_DECODER_BUF_SIZE);
            len = RX_DECODER_BUF_SIZE;
            dec->bytes_drop += (uint32_t)(len); /* approximate dropped head */
        }
    }

    memcpy(dec->buf + dec->used, data, len);
    dec->used += len;

    /* ------------------------------------------------------------------ */
    /* 2) Parse as many complete packets as possible                       */
    /* ------------------------------------------------------------------ */
    while (dec->used >= 2)
    {
        uint16_t payload_len = read_be16(dec->buf);
        size_t   total_len = (size_t)2u + (size_t)payload_len;

        /* Basic sanity: payload must at least contain round_count (4 bytes) */
        if (payload_len < 4)
        {
            dec->pkgs_err++;
            sim_log_warn("RX invalid payload_len=%u (Ch%d) -> resync by 1 byte",
                payload_len, dec->ch_num);

            memmove(dec->buf, dec->buf + 1, dec->used - 1);
            dec->used -= 1;
            continue;
        }

        /* Prevent ridiculous lengths from locking the parser */
        if (total_len > RX_DECODER_BUF_SIZE)
        {
            dec->pkgs_err++;
            sim_log_warn("RX too large total_len=%zu (Ch%d) -> resync by 1 byte",
                total_len, dec->ch_num);

            memmove(dec->buf, dec->buf + 1, dec->used - 1);
            dec->used -= 1;
            continue;
        }

        if (dec->used < total_len)
        {
            /* Need more bytes */
            break;
        }

        /* We have a full packet in dec->buf[0..total_len-1] */
        {
            const uint8_t* payload = dec->buf + 2;
            int            offset = 0;

            uint32_t round_count = read_be32(payload + offset);
            offset += 4;

            int can_count = 0;
            int truncated = 0;
            int parse_ok = 1;

            /* Build ID list (up to max_ids) */
            char ids_str[256];
            int  ids_pos = 0;
            int  ids_kept = 0;

            /* Debug lines printed under summary */
            char debug_lines[1024];

            ids_str[0] = '\0';
            debug_lines[0] = '\0';

            while ((size_t)(offset + 5) <= (size_t)payload_len)
            {
                uint32_t can_id;
                uint8_t  dlc;
                const uint8_t* frame_data;

                can_id = read_be32(payload + offset);
                offset += 4;

                dlc = payload[offset];
                offset += 1;

                if ((size_t)(offset + dlc) > (size_t)payload_len)
                {
                    dec->pkgs_err++;
                    sim_log_warn("RX corrupt packet: frame exceeds payload (Ch%d, Round=%u)",
                        dec->ch_num, round_count);
                    parse_ok = 0;
                    break;
                }

                frame_data = payload + offset;
                can_count++;

                if (ids_kept < dec->max_ids)
                {
                    if (ids_kept > 0 && ids_pos < (int)sizeof(ids_str) - 1)
                    {
                        ids_str[ids_pos++] = ' ';
                        ids_str[ids_pos] = '\0';
                    }

                    ids_pos += snprintf(ids_str + ids_pos,
                        sizeof(ids_str) - (size_t)ids_pos,
                        "0x%X", can_id);

                    if (ids_pos < 0) ids_pos = 0;
                    if (ids_pos >= (int)sizeof(ids_str))
                    {
                        ids_pos = (int)sizeof(ids_str) - 1;
                        ids_str[ids_pos] = '\0';
                    }
                    ids_kept++;
                }
                else
                {
                    truncated = 1;
                }

                /* Collect debug lines for selected CAN IDs */
                if (rx_is_debug_target_id(can_id))
                {
                    rx_append_debug_line(debug_lines,
                        sizeof(debug_lines),
                        can_id,
                        dlc,
                        frame_data);
                }

                offset += dlc;
            }

            /* ------------------------------- */
            /* rx_debug.log: summary line       */
            /* ------------------------------- */
            {
                char ts[32];
                char line[512];
                uint32_t bytes_total = (uint32_t)total_len;
                if (debug_lines[0] != '\0')
                {
                    /* Reuse sim_trace timestamp format to match client style */
                    sim_trace_timestamp(ts, (int)sizeof(ts));

                    //snprintf(line, sizeof(line),
                    //    "%s [Ch%d RX] Round=%-6u | %3d msg(s) | %4u bytes | IDs: %s%s\n",
                    //    ts, dec->ch_num, round_count, can_count, bytes_total,
                    //    ids_str[0] ? ids_str : "(none)",
                    //    truncated ? " ..." : "");

                    snprintf(line, sizeof(line),
                        "%s [Ch%d RX] Round=%-6u | %3d msg(s)\n",
                        ts, dec->ch_num, round_count, can_count);

                    rx_log_line(dec, line);
               
                    rx_log_line(dec, debug_lines);
                }

                /* -------------------------------------------- */
                /* simulator_trace.log: full frames (async trace) */
                /* -------------------------------------------- */
                if (parse_ok)
                {
                    rx_trace_packet(dec->ch_num,
                        round_count,
                        can_count,
                        bytes_total,
                        ids_str,
                        truncated,
                        payload,
                        payload_len);
                }
            }

            if (parse_ok)
            {
                dec->pkgs_ok++;
            }
        }

        /* Consume this packet from buffer */
        if (dec->used > total_len)
        {
            memmove(dec->buf, dec->buf + total_len, dec->used - total_len);
        }
        dec->used -= total_len;
    }
}