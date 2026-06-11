/**
 * @file    sim_state.c
 * @brief   Simulator state machine implementation with lifecycle logging.
 * @date    2026-05
 */
#include "sim_state.h"
#include "sim_log.h"

 /* ================================================================== */
 /* Defines                                                            */
 /* ================================================================== */

#define MAX_CHANNELS  4   /* Support up to 3 channels (1-based index) */

/* ================================================================== */
/* Static variables                                                   */
/* ================================================================== */

/* Global simulator state */
static SimState g_sim_state = SIM_STATE_INIT;
static int      g_sim_initialized = 0;

/* Per-channel states */
static ChannelState g_ch_state[MAX_CHANNELS];
static int          g_ch_initialized[MAX_CHANNELS];

/* ================================================================== */
/* String helpers                                                     */
/* ================================================================== */

const char* sim_state_str(SimState s)
{
    switch (s)
    {
    case SIM_STATE_INIT:     return "INIT";
    case SIM_STATE_RUNNING:  return "RUNNING";
    case SIM_STATE_SHUTDOWN: return "SHUTDOWN";
    default:                 return "UNKNOWN";
    }
}

const char* ch_state_str(ChannelState s)
{
    switch (s)
    {
    case CH_STATE_DISCONNECTED: return "DISCONNECTED";
    case CH_STATE_CONNECTED:    return "CONNECTED";
    default:                    return "UNKNOWN";
    }
}

/* ================================================================== */
/* Global state                                                       */
/* ================================================================== */

void sim_set_state(SimState new_state)
{
    /* First-time initialization */
    if (!g_sim_initialized)
    {
        g_sim_state = new_state;
        g_sim_initialized = 1;

        sim_log_info("Simulator state initialized: %s",
            sim_state_str(new_state));
        return;
    }

    /* Ignore duplicate */
    if (g_sim_state == new_state)
        return;

    sim_log_info("Simulator state: %s -> %s",
        sim_state_str(g_sim_state),
        sim_state_str(new_state));

    g_sim_state = new_state;
}

/* ================================================================== */
/* Channel state                                                      */
/* ================================================================== */
/* Channel index:
 * - Arrays use 0-based index (0..N-1)
 * - ch_set_state uses 1-based (Ch1..ChN)
 * - Caller must convert: ch = index + 1
 */

void ch_set_state(int ch, ChannelState new_state)
{
    /* Validate channel index (1-based) */
    if (ch <= 0 || ch >= MAX_CHANNELS)
        return;

    /* First-time initialization */
    if (!g_ch_initialized[ch])
    {
        g_ch_state[ch] = new_state;
        g_ch_initialized[ch] = 1;

        sim_log_info("Ch%d state initialized: %s",
            ch,
            ch_state_str(new_state));
        return;
    }

    /* Ignore duplicate */
    if (g_ch_state[ch] == new_state)
        return;

    sim_log_info("Ch%d state: %s -> %s",
        ch,
        ch_state_str(g_ch_state[ch]),
        ch_state_str(new_state));

    g_ch_state[ch] = new_state;
}
