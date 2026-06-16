# CAN Integration Test Design

## 1. Overview

This document describes the automated test framework that validates end-to-end CAN message flow through the chain:

```
simulator.exe  →  vecu_vm2.exe  →  vSoC_Test.exe  →  rx_debug.log  →  PASS / FAIL
```

The framework is fully automated — no human interaction required. It starts all processes, optionally injects test data, collects log data, validates each check, and produces console + HTML + text reports.

---

## 2. System Under Test

| Component | Executable | Role |
|-----------|-----------|------|
| Simulator | `simulator.exe` | TCP server (ports 5000–5002). Generates periodic CAN-FD frames defined in `.can` config files and transmits over a custom vCAN-over-TCP protocol. |
| vECU | `Release\vecu_vm2.exe` | Bridge. Connects to the simulator TCP server and forwards CAN frames to the vSoC layer. |
| vSoC | `vSoC\vSoC_Test.exe` | Receiver. Logs every incoming CAN frame to `vSoC\rx_debug.log`. |

### 2.1 Network Topology

```
Test Module --> simulator.exe                  vecu_vm2.exe              vSoC_Test.exe
                TCP :5000 (ch1) ──────────────→ bridges ─────────────────→ :5003
                TCP :5001 (ch2) ──────────────↗
                TCP :5002 (ch3) ──────────────↗
```

### 2.2 vCAN Frame Format (over TCP)

Each TCP packet carries one or more CAN-FD frames using length-prefixed framing:

```
[ payload_len : 2B big-endian ]
[ round_count : 4B big-endian ]
[ can_id      : 4B big-endian ]
[ dlc         : 1B            ]
[ data        : dlc bytes     ]  (up to 64 bytes for CAN-FD)
```

### 2.3 Channel to `.can` File Mapping

| Channel | TCP Port | `.can` File |
|---------|----------|------------|
| ch1 | 5000 | `input/tx_CANFD_G2M-1_BUS.can` |
| ch2 | 5001 | `input/tx_CANFD_G2M-2_BUS.can` |
| ch3 | 5002 | `input/tx_CANFD_G5M_BUS.can` |

---

## 3. TX Enable Mechanism

The simulator reads menu commands from **stdin** when not connected to a terminal (`_isatty` returns false). The Python framework keeps stdin open as a **persistent pipe** so commands can be sent both at startup and during the run.

**Startup sequence** (written immediately after process start):
```
3   →  menu option: Start TX
0   →  channel selection: All channels
```

**Runtime edit sequence** (written at any point during the run via `send_edit()`):
```
2           →  menu option: Edit TX
{channel}   →  channel number (1, 2, or 3)
{msg_index} →  message index within the channel's .can file (1-based)
AA BB CC …  →  new payload as space-separated hex bytes
```

The channel and message index for any CAN ID are resolved automatically by `can_lookup.py`, which scans `config.ini` and all `.can` files.

---

## 4. Test Execution Flow

```
run_test.py
│
├── 1. Kill stale processes (taskkill /F /T /PID — closes console windows too)
├── 2. Rotate old rx_debug.log → rx_debug.log.bak
│
├── 3. Apply data_inject patches (modify .can files before simulator starts)
│
├── 4. Start simulator.exe       (stdin = persistent PIPE, TX enable sent immediately)
│      └── wait startup_delay_sec (default 5 s)
│
├── 5. Start vSoC_Test.exe       (new console window)
│      └── wait 3 s
│
├── 6. Start vecu_vm2.exe        (new console window)
│      └── wait connect_delay_sec (default 5 s)
│
├── 7. Schedule runtime_inject threads (each sleeps delay_sec then sends Edit TX)
│
├── 8. Run for run_duration_sec  (default 15 s)
│      └── CAN frames flow: simulator → vECU → vSoC → rx_debug.log
│
├── 9. Stop all processes (kill by PID tree → closes all console windows)
│      └── Restore .can files patched by data_inject
│      └── Wait 2 s for log flush
│
├── 10. Parse vSoC/rx_debug.log
│
├── 11. Run validation checks against parsed frames
│
└── 12. Write reports → results/test_report_<timestamp>.txt / .html
```

---

## 5. vSoC Log Format

```
[HH:MM:SS.mmm] [RX] Page-N | ID=0xXXX | DLC=N | AA BB CC ...
```

| Field | Description |
|-------|-------------|
| `HH:MM:SS.mmm` | Wall-clock timestamp, millisecond resolution |
| `Page-N` | vSoC channel page: 0, 1, 2 (correspond to simulator ch1, ch2, ch3) |
| `ID=0xXXX` | CAN ID in hex |
| `DLC=N` | Data length code (up to 64 for CAN-FD) |
| `AA BB ...` | Payload bytes in hex, space-separated |

Each CAN ID is logged once per page per cycle — with 3 pages, each 1 000 ms cycle produces 3 log entries for any given CAN ID.

---

## 6. Test Suite Structure

Three separate YAML files group tests by injection strategy. Each file is a self-contained run — select with `--config`.

| File | Strategy | Typical duration |
|------|----------|-----------------|
| `test_original.yaml` | No injection — factory `.can` file data | 15 s |
| `test_data_inject.yaml` | Patch `.can` files before simulator starts | 15 s |
| `test_runtime.yaml` | Send Edit TX commands during the run | 20 s |

---

## 7. How to Run

```bat
REM Default test (original .can data, 15 s)
python run_test.py --config Testcases/test_original.yaml

REM Runtime data injection test (needs 20 s)
python run_test.py --config Testcases/test_runtime.yaml --duration 20

REM Parse existing log only — no process start
python run_test.py --config test_original.yaml --parse-only
```

The same commands work via `run_test.bat` — all arguments are passed through.

Reports are written to `results/`:
- `results/test_report_<timestamp>.txt` — plain text
- `results/test_report_<timestamp>.html` — color-coded HTML

Simulator stdout is captured to `log/sim_stdout.log` for debugging.

---

## 8. Framework File Structure

```
TestModule/
├── run_test.py                   # entry point and orchestrator
├── run_test.bat                  # batch launcher (passes args to run_test.py)
│
├── Testcases/
|   ├── test_original.yaml        # TC-OR: default .can file validation
│   └── test_runtime.yaml         # TC-RT: live Edit TX during run
│
├── simulator.exe                 # must be built from current source (post 2026-06-11)
├── config.ini                    # simulator channel/port/.can file mapping
│
├── framework/
│   ├── process_mgr.py            # start/stop processes, send_edit() via stdin pipe
│   ├── vsoc_log_parser.py        # parse rx_debug.log → List[VsocFrame]
│   ├── validator.py              # run checks → List[CheckResult]
│   ├── reporter.py               # console (ANSI) + HTML + text reports
│   └── can_lookup.py             # resolve CAN ID → (channel, msg_index)
│
├── input/
│   ├── tx_CANFD_G2M-1_BUS.can   # ch1 message definitions
│   ├── tx_CANFD_G2M-2_BUS.can   # ch2 message definitions
│   └── tx_CANFD_G5M_BUS.can     # ch3 message definitions
│
├── Release/
│   └── vecu_vm2.exe
├── vSoC/
│   ├── vSoC_Test.exe
│   └── rx_debug.log              # written during run, parsed after stop
├── log/
│   └── sim_stdout.log            # simulator console output captured for debug
└── results/
    └── test_report_<ts>.html     # generated after each run
```

---
