# CAN Integration Test Module — Release Note

**Version:** 1.0.0
**Date:** 2026-06-17
**Status:** Initial Release

---

## Overview

The CAN Integration Test Module is a Python-based automated test framework for validating CAN-FD message flow through a three-component virtual network: **Simulator** → **vECU** → **vSoC**. It orchestrates process lifecycle, collects reception logs, validates frame data and timing, and produces console, text, and HTML reports.

---

## What's Included

### Framework (`framework/`)

|       Module         | Description                                                                                       |
|----------------------|---------------------------------------------------------------------------------------------------|
| `\process_mgr.py`    | Starts/stops Simulator, vECU, and vSoC executables; sends runtime TX-edit commands via stdin pipe |
| `vsoc_log_parser.py` | Parses `vSoC/rx_debug.log` into typed `VsocFrame` objects (timestamp, page, CAN ID, DLC, payload) |
| `validator.py`       | Executes validation checks from YAML config against parsed frames                                 |
| `reporter.py`        | Generates ANSI console, plain-text, and HTML reports                                              |
| `can_lookup.py`      | Resolves CAN ID → (channel, message index) from `config.ini` and `.can` files                     |

### Test Runner (`run_test.py`)

Single entry point. Supports three modes:

```bash
python run_test.py                          # Full run (default test config)
python run_test.py --config <yaml>          # Custom test config
python run_test.py --parse-only             # Validate existing log only
python run_test.py --duration <sec>         # Override run duration
```

### Test Cases (`Testcases/`)

| File                | Test Cases   | Strategy                                          |
|---------------------|--------------|---------------------------------------------------|
| `test_original.yaml`|     7        | Factory `.can` data validation                    |
| `test_runtime.yaml` |     3        | Runtime TX payload injection at scheduled offsets |

---

## Supported Check Types

| Check Type        | What It Validates                                                                        |
|-------------------|------------------------------------------------------------------------------------------|
| `presence`        | CAN ID appears at least once                                                             |
| `count`           | CAN ID appears at least N times                                                          |
| `data_byte`       | Specific byte offset equals expected value                                               |
| `data_mask`       | Masked byte comparison across payload                                                    |
| `data_all_bytes`  | All received frames carry an exact full payload                                          |
| `data_consistent` | Payload does not change across all frames                                                |
| `interval`        | Mean gap between frames is within ± tolerance (ms)                                       |
| `data_window`     | Payload check scoped to a time window (supports `after_sec`, `before_sec`, `skip_count`) |
| `no_unexpected`   | CAN ID must NOT appear (inverse presence check)                                          |

---

## Report Outputs

- **Console** — ANSI color-coded (green ✓ / red ✗), printed during run
- **Text** — `Results/test_report_<YYYYMMDD_HHMMSS>.txt`
- **HTML** — `Results/test_report_<YYYYMMDD_HHMMSS>.html`

---

## File Structure

```
TestModule/
├── run_test.py
├── run_test.bat
├── TEST_DESIGN.md
├── RELEASE_NOTE.md
├── framework/
│   ├── __init__.py
│   ├── can_lookup.py
│   ├── process_mgr.py
│   ├── reporter.py
│   ├── validator.py
│   └── vsoc_log_parser.py
├── Testcases/
│   ├── test_original.yaml
│   └── test_runtime.yaml
├── Simulator/
│   ├── simulator.exe
│   ├── config.ini
│   └── input/
│       ├── tx_CANFD_G2M-1_BUS.can
│       ├── tx_CANFD_G2M-2_BUS.can
│       └── tx_CANFD_G5M_BUS.can
├── vECU/
│   ├── vecu_vm2.exe
│   └── config.ini
├── vSoC/
│   ├── vSoC_Test.exe
│   ├── config.ini
│   └── rx_debug.log
└── Results/
    └── test_report_<YYYYMMDD_HHMMSS>.[txt|html]
```
