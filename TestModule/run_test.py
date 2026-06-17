#!/usr/bin/env python3
"""
CAN Integration Test Runner
============================
Flow:
  1. Kill any leftover processes
  2. Clear old vSoC log
  3. Start simulator.exe  (TCP servers: 5000, 5001, 5002)
  4. Start vSoC_Test.exe  (connects to vECU on port 5003)
  5. Start vecu_vm2.exe   (bridges simulator ↔ vSoC)
  6. TX enabled automatically via stdin pipe (menu cmd file "3\n0\n")
  7. Run for <run_duration_sec> seconds
  8. Stop all processes
  9. Parse vSoC/rx_debug.log
 10. Validate each test case defined in test_cases.yaml
 11. Print console report + write text & HTML reports to results/

Usage
-----
  python run_test.py                       # use test_cases.yaml
  python run_test.py --config my.yaml      # custom config
  python run_test.py --parse-only          # skip running, just parse existing log
  python run_test.py --duration 30         # override run duration
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import threading
import time

import yaml

# ---------------------------------------------------------------------------
# Path setup
# ---------------------------------------------------------------------------
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from framework.process_mgr      import ProcessManager
from framework.vsoc_log_parser  import parse_log, summary
from framework.validator        import Validator
from framework.reporter         import (ConsoleReporter, TestCaseResult,
                                        write_text_report, write_html_report)


# ---------------------------------------------------------------------------
# Load config
# ---------------------------------------------------------------------------

def load_yaml(path: str) -> dict:
    with open(path, "r") as f:
        return yaml.safe_load(f)


def load_config(config_path: str, duration_override: float = None) -> dict:
    cfg      = load_yaml(config_path)
    settings = cfg.get("settings", {})
    return {
        "tests":       cfg.get("tests", []),
        "run_sec":     duration_override or settings.get("run_duration_sec", 15),
        "startup_sec": settings.get("startup_delay_sec", 5),
        "connect_sec": settings.get("connect_delay_sec", 5),
        "vsoc_log":    os.path.join(BASE_DIR, settings.get("vsoc_log", "vSoC/rx_debug.log")),
    }


# ---------------------------------------------------------------------------
# Clear old log (rotate, do not delete — keep one backup)
# ---------------------------------------------------------------------------

def clear_vsoc_log(log_path: str) -> None:
    if not os.path.exists(log_path):
        return
    backup = log_path + ".bak"
    if os.path.exists(backup):
        os.remove(backup)
    shutil.move(log_path, backup)
    print(f"  [Setup] Old log backed up → {backup}")


# ---------------------------------------------------------------------------
# Shared stack runner
# ---------------------------------------------------------------------------

def _run_stack(tests: list, run_sec: float, startup_sec: float,
               connect_sec: float, vsoc_log: str) -> None:
    """Start simulator stack, fire injections, wait, then stop."""
    pm = ProcessManager(
        base_dir      = BASE_DIR,
        startup_delay = startup_sec,
        connect_delay = connect_sec,
    )

    print("[Setup] Cleaning up stale processes...")
    pm.stop_all()
    time.sleep(1)
    clear_vsoc_log(vsoc_log)

    print("[Setup] Starting stack...")
    pm.start_all()

    rt_threads = []
    for test_def in tests:
        for rt in test_def.get("runtime_inject", []):
            can_id = int(rt["can_id"], 16) if isinstance(rt["can_id"], str) \
                     else int(rt["can_id"])
            data   = [int(v, 16) if isinstance(v, str) else int(v)
                      for v in rt["data"]]
            delay  = float(rt.get("delay_sec", 5.0))
            label  = test_def.get("name", "?")

            def _inject(pm=pm, can_id=can_id, data=data,
                        delay=delay, label=label):
                time.sleep(delay)
                print(f"[Runtime] Injecting 0x{can_id:03X} for '{label}' at t+{delay}s  "
                      f"data=[{' '.join(f'0x{b:02X}' for b in data)}]")
                pm.send_edit(can_id, data)

            t = threading.Thread(target=_inject, daemon=True)
            t.start()
            rt_threads.append(t)

    injections = sum(len(td.get("runtime_inject", [])) for td in tests)
    print(f"[Setup] Running {run_sec}s — {injections} injection(s) scheduled...")
    time.sleep(run_sec)

    for t in rt_threads:
        t.join(timeout=2)

    print("[Setup] Stopping all processes...")
    pm.stop_all()


# ---------------------------------------------------------------------------
# Main test loop
# ---------------------------------------------------------------------------

def run_inject_only(config_path: str, duration_override: float = None) -> int:
    """Start the stack, fire runtime injections, then stop. No log parsing."""
    c = load_config(config_path, duration_override)
    _run_stack(c["tests"], c["run_sec"], c["startup_sec"], c["connect_sec"], c["vsoc_log"])
    print("[Setup] Done. Log not parsed (inject-only mode).")
    return 0


def run_tests(config_path: str,
              parse_only:  bool  = False,
              duration_override: float = None) -> int:

    c = load_config(config_path, duration_override)
    tests, run_sec, startup_sec, connect_sec, vsoc_log = (
        c["tests"], c["run_sec"], c["startup_sec"], c["connect_sec"], c["vsoc_log"]
    )
    results_dir = os.path.join(BASE_DIR, "Results")

    console = ConsoleReporter(color=True)
    console.suite_header(len(tests))

    if not parse_only:
        _run_stack(tests, run_sec, startup_sec, connect_sec, vsoc_log)
        print("[Setup] Waiting for log flush...")
        time.sleep(2)
    else:
        print("[Setup] --parse-only mode: skipping process execution\n")

    # -----------------------------------------------------------------------
    # 2. Parse vSoC log
    # -----------------------------------------------------------------------
    print(f"[Parser] Reading {vsoc_log}")
    if not os.path.exists(vsoc_log):
        print(f"[Parser] ERROR: log file not found: {vsoc_log}")
        return 1

    all_frames = parse_log(vsoc_log)
    print(f"[Parser] {len(all_frames)} frames parsed")

    # Print a summary of what was found
    summ = summary(all_frames)
    print("[Parser] CAN ID summary:")
    for can_id, info in sorted(summ.items()):
        pages = sorted(info["pages"])
        print(f"         0x{can_id:03X}  count={info['count']:4d}  "
              f"DLC={info['dlc']:2d}  pages={pages}")
    print()

    # -----------------------------------------------------------------------
    # 3. Run validation and collect results
    # -----------------------------------------------------------------------
    all_results: list[TestCaseResult] = []
    validator = Validator(all_frames)

    for i, test_def in enumerate(tests, 1):
        name        = test_def.get("name", f"Test {i}")
        description = test_def.get("description", "")
        checks      = test_def.get("checks", [])

        check_results = validator.run_all(checks)
        result        = TestCaseResult(name, description, check_results)
        all_results.append(result)

        console.test_result(result, i, len(tests))

    console.suite_summary(all_results)

    # -----------------------------------------------------------------------
    # 4. Write reports
    # -----------------------------------------------------------------------
    txt_path  = write_text_report(all_results, results_dir)
    html_path = write_html_report(all_results, results_dir)
    print(f"  Text report : {txt_path}")
    print(f"  HTML report : {html_path}")
    print()

    failed = sum(1 for r in all_results if not r.passed)
    return 1 if failed > 0 else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="CAN Integration Test Runner")
    parser.add_argument(
        "--config", default=os.path.join(BASE_DIR, "Testcases/test_original.yaml"),
        help="Path to test_cases.yaml (default: test_cases.yaml)")
    parser.add_argument(
        "--parse-only", action="store_true",
        help="Skip launching processes; parse existing vSoC log only")
    parser.add_argument(
        "--inject-only", action="store_true",
        help="Start stack, fire runtime injections, then stop — skip log parsing")
    parser.add_argument(
        "--duration", type=float, default=None,
        help="Override run duration in seconds")
    args = parser.parse_args()

    if args.inject_only:
        return run_inject_only(
            config_path       = args.config,
            duration_override = args.duration,
        )

    return run_tests(
        config_path       = args.config,
        parse_only        = args.parse_only,
        duration_override = args.duration,
    )


if __name__ == "__main__":
    sys.exit(main())
