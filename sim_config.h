/**
 * @file    sim_config.h
 * @brief   Simulator configuration: INI file reader and config structs.
 * @date    2026-04
 */
#ifndef SIM_CONFIG_H
#define SIM_CONFIG_H

#include <stdint.h>

#define SIM_MAX_CHANNELS    3
#define SIM_MAX_PATH_LEN    260

/* Per-channel configuration read from INI */
typedef struct
{
    uint16_t port;
    char     can_file[SIM_MAX_PATH_LEN];
} SimChannelConfig;

/* Top-level simulator configuration */
typedef struct
{
    int               channel_count;          /* 1..SIM_MAX_CHANNELS */
    int               transmit_interval_ms;   /* TX thread interval  */
    SimChannelConfig  channels[SIM_MAX_CHANNELS];
} SimConfig;

/**
 * @brief  Load simulator configuration from INI file.
 * @param  filepath  Path to config.ini.
 * @param  cfg       Output configuration.
 * @return 0 on success, -1 on failure.
 */
int sim_config_load(const char *filepath, SimConfig *cfg);

/**
 * @brief  Print loaded configuration to stdout (debug).
 */
void sim_config_print(const SimConfig *cfg);

#endif /* SIM_CONFIG_H */