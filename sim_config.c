/**
 * @file    sim_config.c
 * @brief   INI parser for simulator configuration.
 * @date    2026-04
 */
#include "sim_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static char *cfg_trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) { *end = '\0'; end--; }
    return s;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
int sim_config_load(const char *filepath, SimConfig *cfg)
{
    FILE *fp;
    char  line[512];
    int   current_ch = -1;   /* -1 = [general], 0..2 = channel index */

    if (!filepath || !cfg) return -1;

    /* Defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->channel_count        = 1;
    cfg->transmit_interval_ms = 1;

    fp = fopen(filepath, "r");
    if (!fp)
    {
        fprintf(stderr, "[sim_config] Cannot open: %s\n", filepath);
        return -1;
    }

    while (fgets(line, sizeof(line), fp))
    {
        char *text = cfg_trim(line);

        /* Skip empty / comment */
        if (text[0] == '\0' || text[0] == '#' || text[0] == ';')
            continue;

        /* ---- Section header ---- */
        if (text[0] == '[')
        {
            char *end_br = strchr(text, ']');
            if (end_br) *end_br = '\0';
            text++;
            text = cfg_trim(text);

            if (strcmp(text, "general") == 0)
            {
                current_ch = -1;
            }
            else if (strncmp(text, "channel", 7) == 0)
            {
                int ch_num = atoi(text + 7);   /* "channel1" -> 1 */
                if (ch_num >= 1 && ch_num <= SIM_MAX_CHANNELS)
                    current_ch = ch_num - 1;
                else
                    current_ch = -2;  /* unknown, skip */
            }
            else
            {
                current_ch = -2;
            }
            continue;
        }

        /* ---- Key = Value ---- */
        {
            char *eq = strchr(text, '=');
            char *key, *val;

            if (!eq) continue;
            *eq  = '\0';
            key  = cfg_trim(text);
            val  = cfg_trim(eq + 1);

            if (current_ch == -1)
            {
                /* [general] */
                if (strcmp(key, "channel_count") == 0)
                {
                    int n = atoi(val);
                    if (n < 1)                n = 1;
                    if (n > SIM_MAX_CHANNELS) n = SIM_MAX_CHANNELS;
                    cfg->channel_count = n;
                }
                else if (strcmp(key, "transmit_interval_ms") == 0)
                {
                    int ms = atoi(val);
                    cfg->transmit_interval_ms = (ms < 1) ? 1 : ms;
                }
            }
            else if (current_ch >= 0 && current_ch < SIM_MAX_CHANNELS)
            {
                /* [channelN] */
                if (strcmp(key, "port") == 0)
                {
                    cfg->channels[current_ch].port = (uint16_t)atoi(val);
                }
                else if (strcmp(key, "can_file") == 0)
                {
                    strncpy(cfg->channels[current_ch].can_file, val,
                            SIM_MAX_PATH_LEN - 1);
                    cfg->channels[current_ch].can_file[SIM_MAX_PATH_LEN - 1] = '\0';
                }
            }
        }
    }

    fclose(fp);

    /* Validate required fields */
    for (int i = 0; i < cfg->channel_count; i++)
    {
        if (cfg->channels[i].port == 0)
        {
            fprintf(stderr, "[sim_config] Error: channel%d missing port.\n", i + 1);
            return -1;
        }
        if (cfg->channels[i].can_file[0] == '\0')
        {
            fprintf(stderr, "[sim_config] Error: channel%d missing can_file.\n", i + 1);
            return -1;
        }
    }

    return 0;
}

void sim_config_print(const SimConfig *cfg)
{
    if (!cfg) return;

    printf("[sim_config] channel_count=%d, transmit_interval=%d ms\n",
           cfg->channel_count, cfg->transmit_interval_ms);

    for (int i = 0; i < cfg->channel_count; i++)
    {
        printf("  Ch%d: port=%u  can_file=%s\n",
               i + 1,
               cfg->channels[i].port,
               cfg->channels[i].can_file);
    }
}