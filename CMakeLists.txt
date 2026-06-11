cmake_minimum_required(VERSION 3.20)
project(simulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)


# ---------- simulator (SERVER) ----------
add_executable(simulator
    main.c
    menu.c
    sim_config.c
    tx_msg_reader.c
    tx_scheduler.c
    can_msg_queue.c
    tx_transmit.c
    tcp_server.c
    rx_decoder.c
    sim_log.c
    sim_trace.c
    sim_state.c
    log_utils.c
)

target_include_directories(simulator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------- tcp_client (CLIENT) ----------
add_executable(tcp_client
    tcp_client.c
    sim_config.c
)

target_include_directories(tcp_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------- Windows: link Winsock + winmm ----------
if (WIN32)
    target_link_libraries(simulator ws2_32 winmm)
    target_link_libraries(tcp_client ws2_32)
endif()

# ---------- Copy input/ and config.ini to output dir after build ----------
add_custom_command(TARGET simulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/input
        $<TARGET_FILE_DIR:simulator>/input
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini
        $<TARGET_FILE_DIR:simulator>/config.ini
    COMMENT "Copying input/ and config.ini to output directory..."
)