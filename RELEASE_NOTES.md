# Release Notes — Simulator v3.0.5.0

**Release Date:** 2026-06-11

---

## Overview

Simulator v3.0.5.0 is the initial public release of a multi-channel CAN bus message simulator built on a TCP-based client/server architecture. It is designed for testing and validating CAN-connected applications without requiring physical hardware.

---

## Key Features

### Multi-Channel CAN Simulation
- Supports up to **3 active channels** (maximum 8 configurable) simultaneously
- Each channel maps to a dedicated TCP port and CAN bus configuration:
  | Channel | Port | Bus |
  |---------|------|-----|
  | 1 | 5000 | G2M-1 |
  | 2 | 5001 | G2M-2 |
  | 3 | 5002 | G5M  |

### CAN FD Support
- Transmits and receives **CAN Flexible Data-rate (CAN FD)** frames
- Pre-loaded message datasets for G2M-1, G2M-2, and G5M bus configurations

### Precise Time-Based Scheduling
- Dedicated TX scheduler thread with **~1 ms polling interval**
- Microsecond-resolution timing using high-resolution platform timers (POSIX `clock_gettime` on Linux, `QueryPerformanceCounter` on Windows)
- Configurable transmit interval via `config.ini`

### TCP Client/Server Architecture
- Simulator runs as a **multi-client TCP server** — connect any number of test clients to any channel port
- Companion **TCP client** binary included for quick connectivity testing
- Server polling thread handles connection events without blocking the scheduler

### Cross-Platform Support
- Builds on **Linux/Unix** (POSIX sockets) and **Windows** (Winsock2)
- CMake 3.20+ build system with C11 standard

### Interactive Menu Interface
- Real-time control of the simulator from the main thread menu
- Start/stop transmission, inspect state, and view live statistics without restarting

### Logging & Tracing
- **Simulator log** — general runtime events and errors
- **Trace log** — per-message transmission records
- **RX debug log** — decoded incoming CAN frames for diagnostics

---

## Architecture

```
Main Thread (menu + state)
    │
    ├── TX Scheduler Thread  ──►  TCP Server Poll Thread
    │       (timing engine)              (client I/O)
    │
    └── sim_state (mutex-protected shared state)
```

---

## Configuration

Edit `config.ini` to adjust channels, ports, and timing before starting the simulator:

```ini
; example — 3-channel setup
[channel1]
port = 5000
bus  = G2M-1

[channel2]
port = 5001
bus  = G2M-2

[channel3]
port = 5002
bus  = G5M

[scheduler]
interval_ms = 1
```

---

## Build

```bash
cmake -B build -S . && cmake --build build
```

Executables produced:
- `simulator` — the server
- `tcp_client` — the test client

---

## Known Limitations

- Maximum 8 channels enforced at compile time (`MAX_CHANNELS`)
- Windows build requires Winsock2; ensure the Windows SDK is available
- No TLS/authentication on TCP connections — intended for local/lab use only

---

## Changelog

| Version | Date | Notes |
|---------|------|-------|
| 3.0.5.0 | 2026-06-11 | Initial release |
