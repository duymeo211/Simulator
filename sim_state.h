/**
 * @file    sim_state.h
 * @brief   Simulator global and per-channel state machine definitions.
 * @date    2026-05
 */
#ifndef SIM_STATE_H
#define SIM_STATE_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Global Simulator State                                             */
/* ------------------------------------------------------------------ */
typedef enum
{
    SIM_STATE_INIT = 0,
    SIM_STATE_RUNNING,
    SIM_STATE_SHUTDOWN
} SimState;

/* ------------------------------------------------------------------ */
/* Per-channel State                                                  */
/* ------------------------------------------------------------------ */
typedef enum
{
    CH_STATE_DISCONNECTED = 0,
    CH_STATE_CONNECTED
} ChannelState;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

void sim_set_state(SimState new_state);
void ch_set_state(int ch, ChannelState new_state);

const char* sim_state_str(SimState s);
const char* ch_state_str(ChannelState s);

#endif