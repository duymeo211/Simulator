/**
 * @file    tcp_server.c
 * @brief   Single-client TCP server for CAN simulator.
 *
 *          Poll-based design: no internal threads.
 *          Caller must invoke tcp_server_poll() periodically
 *          to accept connections and receive data.
 *
 *          This module owns per-channel TCP connection state:
 *          DISCONNECTED <-> CONNECTED
 *
 * @date    2026-04
 */
#include "tcp_server.h"
#include "tcp_common.h"
#include "sim_log.h"
#include "sim_state.h"

#include <stdlib.h>
#include <string.h>

 /* ================================================================== */
 /* Internal struct                                                    */
 /* ================================================================== */

struct TcpServer
{
    TcpServerConfig cfg;
    socket_t        listen_sock;    /* listening socket (accept)       */
    socket_t        client_sock;    /* connected client (recv / send)  */
    int             net_inited;     /* flag: networking initialized    */
};

/* ================================================================== */
/* Static helpers                                                     */
/* ================================================================== */

/**
 * @brief Create and bind a listening socket.
 * @return Valid socket on success, socket_invalid on failure.
 */
static socket_t create_listen_socket(const char* bind_ip, uint16_t port)
{
    socket_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == socket_invalid)
    {
        print_last_error("socket() failed");
        return socket_invalid;
    }

    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&opt, (int)sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = (bind_ip && bind_ip[0])
        ? inet_addr(bind_ip)
        : htonl(INADDR_ANY);

    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) == socket_error)
    {
        print_last_error("bind() failed");
        close_socket(ls);
        return socket_invalid;
    }

    if (listen(ls, 4) == socket_error)
    {
        print_last_error("listen() failed");
        close_socket(ls);
        return socket_invalid;
    }

    return ls;
}

/**
 * @brief Non-blocking accept: check for pending client connection.
 *        Only accepts one client at a time.
 * @return 0 on success (or no pending), <0 on error.
 */
static int accept_if_any(TcpServer* s)
{
    if (s->listen_sock == socket_invalid) return 0;
    if (s->client_sock != socket_invalid) return 0; /* already connected */

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s->listen_sock, &rfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0; /* non-blocking poll */

    int r = select((int)(s->listen_sock + 1), &rfds, NULL, NULL, &tv);
    if (r == 0) return 0; /* no pending connection */
    if (r == socket_error)
    {
        sim_log_warn("Ch%d TCP select() failed while waiting for client",
            s->cfg.ch_num);
        return -1;
    }

    struct sockaddr_in client_addr;
#ifdef _WIN32
    int clen = (int)sizeof(client_addr);
#else
    socklen_t clen = (socklen_t)sizeof(client_addr);
#endif

    socket_t cs = accept(s->listen_sock,
        (struct sockaddr*)&client_addr, &clen);
    if (cs == socket_invalid)
    {
        sim_log_warn("Ch%d TCP accept() failed", s->cfg.ch_num);
        return -2;
    }

    /* Set short send timeout to avoid blocking TX thread */
    {
        int timeout_ms = 20;
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO,
            (const char*)&timeout_ms, sizeof(timeout_ms));
    }

    s->client_sock = cs;

    sim_log_info("Ch%d TCP client connected", s->cfg.ch_num);
    ch_set_state(s->cfg.ch_num, CH_STATE_CONNECTED);

    return 0;
}

/* ================================================================== */
/* Lifecycle API                                                      */
/* ================================================================== */

TcpServer* tcp_server_create(const TcpServerConfig* cfg)
{
    if (cfg == NULL) return NULL;

    TcpServer* s = (TcpServer*)calloc(1, sizeof(TcpServer));
    if (s == NULL) return NULL;

    s->cfg = *cfg;
    s->listen_sock = socket_invalid;
    s->client_sock = socket_invalid;
    s->net_inited = 0;

    sim_log_info("Ch%d TCP server object created (port=%u)",
        s->cfg.ch_num, s->cfg.port);

    return s;
}

void tcp_server_destroy(TcpServer* s)
{
    if (s == NULL) return;

    tcp_server_stop(s);

    sim_log_info("Ch%d TCP server object destroyed", s->cfg.ch_num);
    free(s);
}

int tcp_server_start(TcpServer* s)
{
    if (s == NULL) return -1;

    if (!s->net_inited)
    {
        if (net_init() != 0)
        {
            print_last_error("net_init() failed");
            sim_log_error("Ch%d TCP net_init() failed", s->cfg.ch_num);
            return -2;
        }

        s->net_inited = 1;
        sim_log_info("Ch%d TCP networking initialized", s->cfg.ch_num);
    }

    if (s->listen_sock != socket_invalid)
    {
        return 0; /* already started */
    }

    {
        const char* ip = s->cfg.bind_ip ? s->cfg.bind_ip : "0.0.0.0";
        s->listen_sock = create_listen_socket(ip, s->cfg.port);
    }

    if (s->listen_sock == socket_invalid)
    {
        sim_log_error("Ch%d TCP server start failed (port=%u)",
            s->cfg.ch_num, s->cfg.port);
        return -3;
    }

    sim_log_info("Ch%d TCP server started at %s:%u",
        s->cfg.ch_num,
        s->cfg.bind_ip ? s->cfg.bind_ip : "0.0.0.0",
        s->cfg.port);

    return 0;
}

void tcp_server_stop(TcpServer* s)
{
    if (s == NULL) return;

    if (s->client_sock != socket_invalid)
    {
        close_socket(s->client_sock);
        s->client_sock = socket_invalid;

        sim_log_info("Ch%d TCP client socket closed", s->cfg.ch_num);
        ch_set_state(s->cfg.ch_num, CH_STATE_DISCONNECTED);
    }

    if (s->listen_sock != socket_invalid)
    {
        close_socket(s->listen_sock);
        s->listen_sock = socket_invalid;

        sim_log_info("Ch%d TCP listen socket closed", s->cfg.ch_num);
    }

    if (s->net_inited)
    {
        net_cleanup();
        s->net_inited = 0;

        sim_log_info("Ch%d TCP networking cleaned up", s->cfg.ch_num);
    }
}

/* ================================================================== */
/* Runtime API                                                        */
/* ================================================================== */

/**
 * @brief Poll server: accept + recv + callback.
 *        Must be called periodically from a dedicated thread.
 */
int tcp_server_poll(TcpServer* s)
{
    if (s == NULL) return -1;

    /* Accept new client if any (non-blocking) */
    {
        int ar = accept_if_any(s);
        if (ar < 0) return ar;
    }

    /* Recv data from connected client */
    if (s->client_sock != socket_invalid)
    {
        uint8_t buf[2048];
        int     to = (s->cfg.rx_timeout_ms > 0) ? s->cfg.rx_timeout_ms : 0;
        int     n;

        if (to == 0)
        {
            n = recv_with_timeout(s->client_sock, buf, (int)sizeof(buf), 0);
            if (n == -2) return 0; /* no data available */
        }
        else
        {
            n = recv_with_timeout(s->client_sock, buf, (int)sizeof(buf), to);
            if (n == -2) return 0; /* timeout */
        }

        if (n < 0)
        {
            print_last_error("[tcp_server] recv() failed");
            sim_log_warn("Ch%d TCP recv() failed, client disconnected",
                s->cfg.ch_num);

            close_socket(s->client_sock);
            s->client_sock = socket_invalid;
            ch_set_state(s->cfg.ch_num, CH_STATE_DISCONNECTED);
            return -2;
        }

        if (n == 0)
        {
            printf("[tcp_server] Client disconnected\n");
            sim_log_info("Ch%d TCP client disconnected", s->cfg.ch_num);

            close_socket(s->client_sock);
            s->client_sock = socket_invalid;
            ch_set_state(s->cfg.ch_num, CH_STATE_DISCONNECTED);
            return 0;
        }

        /* Invoke RX callback with received data */
        if (s->cfg.on_rx != NULL)
        {
            s->cfg.on_rx(buf, (size_t)n, s->cfg.user);
        }
    }

    return 0;
}

int tcp_server_send(TcpServer* s, const uint8_t* data, size_t len)
{
    if (s == NULL || data == NULL || len == 0) return -1;
    if (s->client_sock == socket_invalid) return -2;

    {
        int r = send_all(s->client_sock, data, len);
        if (r != 0)
        {
            print_last_error("[tcp_server] send() failed");
            sim_log_warn("Ch%d TCP send() failed, client disconnected",
                s->cfg.ch_num);

            close_socket(s->client_sock);
            s->client_sock = socket_invalid;
            ch_set_state(s->cfg.ch_num, CH_STATE_DISCONNECTED);
            return -3;
        }
    }

    return 0;
}

int tcp_server_is_client_connected(const TcpServer* s)
{
    return (s && s->client_sock != socket_invalid) ? 1 : 0;
}