/**
 * @file    main.c
 * @brief   Simulator main entry: multi-channel CAN TX over TCP.
 *          Scheduler + Poll in dedicated threads.
 *          Main thread only handles menu + connection change display.
 * @date    2026-04
 * @modified 2026-06
 */
#include "tcp_server.h"
#include "menu.h"
#include "tx_msg_reader.h"
#include "tx_scheduler.h"
#include "can_msg_queue.h"
#include "tx_transmit.h"
#include "sim_config.h"
#include "rx_decoder.h"
#include "sim_log.h"
#include "sim_trace.h"
#include "sim_state.h"
#include "log_utils.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ================================================================== */
/* Log paths                                                           */
/* ================================================================== */
#define LOG_DIR                 "log"
#define SIM_LOG_FILE            "log/simulator.log"
#define SIM_TRACE_FILE          "log/simulator_trace.log"
#define RX_DEBUG_LOG_FILE       "log/rx_debug.log"

/* ================================================================== */
/* Types (must be before static variables)                             */
/* ================================================================== */

typedef struct
{
    SimulatorCtrl* ctrl;
    RxDecoder      dec;
} RxChannelUser;

typedef struct
{
    TcpServer** servers;
    SimulatorCtrl* ctrl;
    int             ch_count;
    volatile bool   running;
    void* thread_handle;
} ServerPollCtx;

/* ================================================================== */
/* Static variables                                                    */
/* ================================================================== */

#ifdef _WIN32
static HANDLE s_rx_ps_process = NULL;
#endif

static TxSchedulerThread sched_thread = { 0 };
static ServerPollCtx     poll_ctx = { 0 };

/* ================================================================== */
/* Static helpers                                                      */
/* ================================================================== */

#ifdef _WIN32
/**
 * @brief Resize a window by title, with retry (wait for window to appear).
 */
static void resize_window(const char* title, int width_px, int height_px)
{
    int  retries = 10;
    HWND hwnd = NULL;

    for (int i = 0; i < retries; i++)
    {
        hwnd = FindWindowA(NULL, title);
        if (hwnd != NULL) break;
        Sleep(300);
    }

    if (hwnd == NULL) return;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    MoveWindow(hwnd, rc.left, rc.top, width_px, height_px, TRUE);
}
#endif

/* ------------------------------------------------------------------ */
/* Platform sleep helper                                               */
/* ------------------------------------------------------------------ */
static void main_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* ------------------------------------------------------------------ */
/* TCP send callback for TX Transmit                                   */
/* ------------------------------------------------------------------ */
static bool on_transmit_send(const uint8_t* data, uint16_t len, void* user)
{
    TcpServer* server = (TcpServer*)user;
    int result = tcp_server_send(server, data, (size_t)len);
    return (result == 0);
}

/* ------------------------------------------------------------------ */
/* RX callback (streaming decode + log)                                */
/* ------------------------------------------------------------------ */
static void on_rx(const uint8_t* data, size_t len, void* user)
{
    RxChannelUser* u = (RxChannelUser*)user;
    if (u == NULL || u->ctrl == NULL) return;

    if (!u->ctrl->rx_enabled) return;

    rx_decoder_feed(&u->dec, data, len);
}

/* ================================================================== */
/* Server Poll Thread — accept + recv for all channels                 */
/* ================================================================== */

#ifdef _WIN32
static DWORD WINAPI server_poll_thread_func(LPVOID param)
#else
static void* server_poll_thread_func(void* param)
#endif
{
    ServerPollCtx* ctx = (ServerPollCtx*)param;
    int i;

    printf("[Poll Thread] Started\n");

    while (ctx->running)
    {
        for (i = 0; i < ctx->ch_count; i++)
        {
            tcp_server_poll(ctx->servers[i]);
            ctx->ctrl->ch_connected[i] =
                tcp_server_is_client_connected(ctx->servers[i]);
        }

        main_sleep_ms(1);
    }

    printf("[Poll Thread] Stopped\n");

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int main(void)
{
    static SimulatorCtrl ctrl = { 0 };
    static TxScheduler   scheds[SIM_MAX_CHANNELS] = { 0 };
    static CanMsgQueue   queues[SIM_MAX_CHANNELS] = { 0 };
    static TxTransmit    transmit = { 0 };

    TcpServer* servers[SIM_MAX_CHANNELS] = { NULL };
    TcpServerConfig srv_cfg = { 0 };
    bool            prev_ch_connected[SIM_MAX_CHANNELS] = { false };
    int             ch_count = 0;
    int             i;

    FILE* rx_log = NULL;
    RxChannelUser rx_users[SIM_MAX_CHANNELS] = { 0 };

#ifdef _WIN32
    printf("\033]0;Simulator\007");
    fflush(stdout);
    Sleep(500);
    resize_window("Simulator", 1000, 400);
#endif

    printf("Welcome Simulator v0.4\n");

    /* -------------------------------------------------------------- */
    /* Prepare log folder                                              */
    /* -------------------------------------------------------------- */
    log_prepare_folder(LOG_DIR);

    if (!sim_log_init(SIM_LOG_FILE))
    {
        printf("[main] Warning: cannot open %s\n", SIM_LOG_FILE);
    }

    if (!sim_trace_init(SIM_TRACE_FILE))
    {
        printf("[main] Warning: cannot open %s\n", SIM_TRACE_FILE);
    }

    /* Global lifecycle: INIT */
    sim_set_state(SIM_STATE_INIT);

    /* -------------------------------------------------------------- */
    /* Open RX debug log (summary, realtime)                           */
    /* -------------------------------------------------------------- */
    rx_log = fopen(RX_DEBUG_LOG_FILE, "w");
    if (rx_log == NULL)
    {
        printf("[main] Warning: cannot open %s (RX logging disabled)\n",
            RX_DEBUG_LOG_FILE);
    }

#ifdef _WIN32
    if (rx_log != NULL)
    {
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;

        char cmd[] = "powershell.exe -NoExit -Command \""
            "$Host.UI.RawUI.WindowTitle='RX Log'; "
            "Get-Content 'log\\rx_debug.log' -Wait\"";

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));

        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
            CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
        {
            s_rx_ps_process = pi.hProcess;
            CloseHandle(pi.hThread);
        }

        resize_window("RX Log", 1000, 250);
    }
#endif

    ctrl.running = true;

    for (i = 0; i < SIM_MAX_CHANNELS; i++)
    {
        ctrl.tx_enabled[i] = false;
        sim_mutex_init(&ctrl.tx_list_mutex[i]);
    }

    ctrl.rx_enabled = true;

    /* -------------------------------------------------------------- */
    /* Load config                                                     */
    /* -------------------------------------------------------------- */
    if (sim_config_load("config.ini", &ctrl.sim_cfg) != 0)
    {
        printf("[main] Failed to load config.ini\n");
        goto cleanup_early;
    }

    sim_config_print(&ctrl.sim_cfg);
    ch_count = ctrl.sim_cfg.channel_count;

    /* -------------------------------------------------------------- */
    /* Load TX message lists for all channels                          */
    /* -------------------------------------------------------------- */
    for (i = 0; i < ch_count; i++)
    {
        if (tx_msg_reader_load(ctrl.sim_cfg.channels[i].can_file,
            &ctrl.tx_lists[i]) != 0)
        {
            printf("[main] Warning: Ch%d: %s not loaded.\n",
                i + 1, ctrl.sim_cfg.channels[i].can_file);
        }
    }

    /* -------------------------------------------------------------- */
    /* Init queues                                                     */
    /* -------------------------------------------------------------- */
    for (i = 0; i < ch_count; i++)
    {
        can_msg_queue_init(&queues[i]);
    }

    /* -------------------------------------------------------------- */
    /* Init schedulers (one per channel)                               */
    /* -------------------------------------------------------------- */
    for (i = 0; i < ch_count; i++)
    {
        tx_scheduler_init(&scheds[i], &ctrl.tx_lists[i],
            &ctrl.tx_list_mutex[i], &queues[i]);
    }

    /* -------------------------------------------------------------- */
    /* Start Scheduler Thread                                         */
    /* -------------------------------------------------------------- */
    tx_scheduler_thread_init(&sched_thread, scheds, ch_count,
        ctrl.tx_enabled, ctrl.ch_connected);

    if (tx_scheduler_thread_start(&sched_thread) != 0)
    {
        printf("[main] Failed to start Scheduler thread\n");
        goto cleanup_early;
    }

    /* -------------------------------------------------------------- */
    /* Init TCP servers (one per channel)                              */
    /* -------------------------------------------------------------- */
    for (i = 0; i < ch_count; i++)
    {
        memset(&srv_cfg, 0, sizeof(srv_cfg));
        srv_cfg.bind_ip = "127.0.0.1";
        srv_cfg.port = ctrl.sim_cfg.channels[i].port;
        srv_cfg.rx_timeout_ms = 0;
        srv_cfg.ch_num = i + 1;

        rx_users[i].ctrl = &ctrl;
        rx_decoder_init(&rx_users[i].dec, i + 1, rx_log, 10);

        srv_cfg.on_rx = on_rx;
        srv_cfg.user = &rx_users[i];

        servers[i] = tcp_server_create(&srv_cfg);
        if (servers[i] == NULL)
        {
            printf("[main] Ch%d: create server failed (port %u)\n",
                i + 1, ctrl.sim_cfg.channels[i].port);
            goto cleanup_early;
        }

        if (tcp_server_start(servers[i]) != 0)
        {
            printf("[main] Ch%d: start server failed (port %u)\n",
                i + 1, ctrl.sim_cfg.channels[i].port);
            goto cleanup_early;
        }

        printf("[main] Ch%d: TCP server at 127.0.0.1:%u\n",
            i + 1, ctrl.sim_cfg.channels[i].port);
    }

    /* Initial per-channel state: DISCONNECTED */
    for (i = 0; i < ch_count; i++)
    {
        ch_set_state(i + 1, CH_STATE_DISCONNECTED);
    }

    /* -------------------------------------------------------------- */
    /* Start Server Poll Thread                                        */
    /* -------------------------------------------------------------- */
    poll_ctx.servers = servers;
    poll_ctx.ctrl = &ctrl;
    poll_ctx.ch_count = ch_count;
    poll_ctx.running = true;

#ifdef _WIN32
    poll_ctx.thread_handle = CreateThread(
        NULL, 0, server_poll_thread_func, &poll_ctx, 0, NULL);
    if (poll_ctx.thread_handle == NULL)
    {
        printf("[main] Failed to start Poll thread\n");
        goto cleanup_early;
    }
#else
    poll_ctx.thread_handle = NULL;
#endif

    /* -------------------------------------------------------------- */
    /* Init TX Transmit (multi-channel, single thread)                 */
    /* -------------------------------------------------------------- */
    tx_transmit_init(&transmit, ch_count, ctrl.sim_cfg.transmit_interval_ms);

    for (i = 0; i < ch_count; i++)
    {
        tx_transmit_set_channel(&transmit, i, &queues[i],
            on_transmit_send, servers[i]);
    }

    if (tx_transmit_start(&transmit) != 0)
    {
        printf("[main] Failed to start TX Transmit thread\n");
        goto cleanup_early;
    }

#ifdef _WIN32
    resize_window("TX Log", 1000, 250);
#endif

    /* Global lifecycle: RUNNING */
    sim_set_state(SIM_STATE_RUNNING);

    /* -------------------------------------------------------------- */
    /* Print menu                                                      */
    /* -------------------------------------------------------------- */
    menu_print(&ctrl);

    /* ============================================================== */
    /* Main loop — only menu + connection change display              */
    /* TX Scheduler, TX Transmit, TCP Poll all run in their own       */
    /* threads                                                        */
    /* ============================================================== */
    while (ctrl.running)
    {
        int  choice = -1;
        bool any_change = false;

        /* Detect per-channel connection changes (read-only) */
        for (i = 0; i < ch_count; i++)
        {
            if (ctrl.ch_connected[i] != prev_ch_connected[i])
            {
                prev_ch_connected[i] = ctrl.ch_connected[i];

                if (ctrl.ch_connected[i])
                {
                    printf("\n[Server] Ch%d: Client connected.\n", i + 1);
                }
                else
                {
                    printf("\n[Server] Ch%d: Client disconnected.\n", i + 1);
                }

                any_change = true;
            }
        }

        if (any_change)
        {
            menu_print(&ctrl);
        }

        /* Menu input (can block — no longer affects TX or RX) */
        if (menu_try_read_choice(&choice))
        {
            menu_handle_choice(choice, &ctrl);
            if (ctrl.running)
            {
                menu_print(&ctrl);
            }
        }

        main_sleep_ms(10);
    }

    /* ============================================================== */
    /* Cleanup                                                        */
    /* ============================================================== */

    /* Global lifecycle: SHUTDOWN */
    sim_set_state(SIM_STATE_SHUTDOWN);

    /* Stop poll thread first (stop recv before destroying servers) */
    poll_ctx.running = false;

#ifdef _WIN32
    if (poll_ctx.thread_handle != NULL)
    {
        WaitForSingleObject((HANDLE)poll_ctx.thread_handle, 2000);
        CloseHandle((HANDLE)poll_ctx.thread_handle);
        poll_ctx.thread_handle = NULL;
    }
#endif

    /* Force all channels to DISCONNECTED during shutdown */
    for (i = 0; i < ch_count; i++)
    {
        ch_set_state(i + 1, CH_STATE_DISCONNECTED);
    }

    tx_scheduler_thread_stop(&sched_thread);
    tx_transmit_stop(&transmit);

    for (i = 0; i < ch_count; i++)
    {
        tcp_server_destroy(servers[i]);
        can_msg_queue_destroy(&queues[i]);
        sim_mutex_destroy(&ctrl.tx_list_mutex[i]);
    }

    if (rx_log != NULL)
    {
#ifdef _WIN32
        if (s_rx_ps_process != NULL)
        {
            TerminateProcess(s_rx_ps_process, 0);
            CloseHandle(s_rx_ps_process);
            s_rx_ps_process = NULL;
        }
#endif
        fclose(rx_log);
        rx_log = NULL;
    }

    sim_trace_stop();
    sim_log_stop();

    printf("[main] Simulator stopped.\n");
    return 0;

    /* ============================================================== */
    /* Early cleanup (on init failure)                                */
    /* ============================================================== */
cleanup_early:
    sim_set_state(SIM_STATE_SHUTDOWN);

    poll_ctx.running = false;

#ifdef _WIN32
    if (poll_ctx.thread_handle != NULL)
    {
        WaitForSingleObject((HANDLE)poll_ctx.thread_handle, 2000);
        CloseHandle((HANDLE)poll_ctx.thread_handle);
        poll_ctx.thread_handle = NULL;
    }
#endif

    /* Force all channels to DISCONNECTED */
    for (i = 0; i < ch_count; i++)
    {
        ch_set_state(i + 1, CH_STATE_DISCONNECTED);
    }

    tx_scheduler_thread_stop(&sched_thread);

    for (i = 0; i < ch_count; i++)
    {
        if (servers[i] != NULL)
        {
            tcp_server_destroy(servers[i]);
        }
        can_msg_queue_destroy(&queues[i]);
        sim_mutex_destroy(&ctrl.tx_list_mutex[i]);
    }

    if (rx_log != NULL)
    {
#ifdef _WIN32
        if (s_rx_ps_process != NULL)
        {
            TerminateProcess(s_rx_ps_process, 0);
            CloseHandle(s_rx_ps_process);
            s_rx_ps_process = NULL;
        }
#endif
        fclose(rx_log);
        rx_log = NULL;
    }

    sim_trace_stop();
    sim_log_stop();

    return 1;
}