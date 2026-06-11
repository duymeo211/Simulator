/**
 * @file    tcp_server.h
 * @brief   Single-client TCP server for CAN simulator.
 *
 *          Each TcpServer instance manages one listening port
 *          and accepts at most one client connection.
 *          The server is poll-based: caller must invoke
 *          tcp_server_poll() periodically to accept connections
 *          and receive data.
 *
 * @date    2026-04
 */
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* ------------------------------------------------------------------ */
    /* Opaque type                                                        */
    /* ------------------------------------------------------------------ */
    typedef struct TcpServer TcpServer;

    /* ------------------------------------------------------------------ */
    /* RX callback                                                        */
    /* ------------------------------------------------------------------ */

    /** @brief Invoked when data is received from connected client. */
    typedef void (*tcp_on_rx_fn)(const uint8_t* data, size_t len, void* user);

    /* ------------------------------------------------------------------ */
    /* Configuration                                                      */
    /* ------------------------------------------------------------------ */
    typedef struct
    {
        const char* bind_ip;        /* e.g. "127.0.0.1" or "0.0.0.0"           */
        uint16_t     port;           /* listening port                           */
        int          rx_timeout_ms;  /* recv timeout in poll (0 = non-blocking) */
        int          ch_num;         /* 1-based channel number for logging/state */
        tcp_on_rx_fn on_rx;          /* RX callback (NULL if not needed)         */
        void* user;           /* user context passed to callback          */
    } TcpServerConfig;

    /* ------------------------------------------------------------------ */
    /* Lifecycle API                                                      */
    /* ------------------------------------------------------------------ */

    /** @brief Allocate and initialize a server instance. */
    TcpServer* tcp_server_create(const TcpServerConfig* cfg);

    /** @brief Stop and free a server instance. */
    void tcp_server_destroy(TcpServer* s);

    /** @brief Create listening socket and start accepting. */
    int tcp_server_start(TcpServer* s);

    /** @brief Close all sockets and clean up networking. */
    void tcp_server_stop(TcpServer* s);

    /* ------------------------------------------------------------------ */
    /* Runtime API                                                        */
    /* ------------------------------------------------------------------ */

    /**
     * @brief Poll: call periodically (e.g. every 1-10 ms).
     *        - Accepts new client if pending.
     *        - Receives data if available.
     * @return 0 on success, <0 on error.
     */
    int tcp_server_poll(TcpServer* s);

    /**
     * @brief Send data to the currently connected client.
     * @return 0 on success, <0 on failure.
     */
    int tcp_server_send(TcpServer* s, const uint8_t* data, size_t len);

    /** @brief Check if a client is currently connected. */
    int tcp_server_is_client_connected(const TcpServer* s);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SERVER_H */