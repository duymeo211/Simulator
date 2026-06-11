/**
 * @file    menu.h
 * @brief   Simulator menu and runtime control.
 * @date    2026-04
 */
#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include "tx_msg_reader.h"
#include "sim_config.h"
#include "sim_mutex.h"

typedef struct
{
    bool      tx_enabled[SIM_MAX_CHANNELS];
    bool      rx_enabled;
    bool      running;
    SimConfig sim_cfg;
    TxMsgList tx_lists[SIM_MAX_CHANNELS];
    bool      ch_connected[SIM_MAX_CHANNELS];   /* per-channel TCP status */

    /* Protect tx_lists[ch] shared between menu thread and scheduler thread */
    SimMutex  tx_list_mutex[SIM_MAX_CHANNELS];
} SimulatorCtrl;

void menu_print(const SimulatorCtrl* ctrl);
void menu_handle_choice(int choice, SimulatorCtrl* ctrl);
bool menu_try_read_choice(int* out_choice);

#endif /* MENU_H */