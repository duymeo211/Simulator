#!/usr/bin/env python3
"""
CAN Integration Test Runner

Flow
----
  1. Kill leftover processes
  2. Clear old vSoC log (rotate to .bak)
  3. Start CanSimulatorCs engine via DLL  (TCP: 5000 / 5001 / 5002)
  4. Start vSoC_Test.exe
  5. Start vecu_vm2.exe
  6. Run for <run_duration_sec> seconds, then stop
  7. Parse vSoC/rx_debug.log
  8. Validate each test case
  9. Print report + write text & HTML to Results/

Usage
-----
  python run_test.py
  python run_test.py --config Testcases/my.yaml
  python run_test.py --parse-only
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import time

import threading
import yaml

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from framework.process_mgr     import ProcessManager
from framework.vsoc_log_parser import parse_log, summary
from framework.validator       import Validator
from framework.reporter        import (ConsoleReporter, TestCaseResult,
                                       write_text_report, write_html_report)

DEFAULT_CONFIG = os.path.join(BASE_DIR, "Testcases", "test_runtime - Copy.yaml")


def _normalize_test(test: dict) -> dict:
    test = dict(test)
    default_can_id = test.get("message_id")
    delay = float(test.get("delay_sec", 5.0))

    checks = []
    for chk in test.get("checks", []):
        chk = dict(chk)
        if "can_id" not in chk and default_can_id is not None:
            chk["can_id"] = default_can_id
        if chk.get("type") == "data_byte" and isinstance(chk.get("expected"), list):
            chk["type"]      = "data_window"
            chk["after_sec"] = delay + 4.0
        checks.append(chk)
    test["checks"] = checks
    return test


def load_config(config_path: str) -> dict:
    with open(config_path) as f:
        tests = yaml.safe_load(f)   # always a flat list of test dicts

    return {
        "tests":       [_normalize_test(t) for t in tests],
        "run_sec":     15,
        "startup_sec": 5,
        "connect_sec": 5,
        "vsoc_log":    os.path.join(BASE_DIR, "vSoC/rx_debug.log"),
    }


def _clear_log(log_path: str) -> None:
    if not os.path.exists(log_path):
        return
    backup = log_path + ".bak"
    if os.path.exists(backup):
        os.remove(backup)
    shutil.move(log_path, backup)
    print(f"  [Setup] Log rotated → {backup}")


def _fire_injections(pm: ProcessManager, tests: list) -> list:
    threads = []
    for test_def in tests:
        if "message_id" not in test_def or "send" not in test_def:
            continue
        can_id = int(test_def["message_id"], 16) if isinstance(test_def["message_id"], str) else int(test_def["message_id"])
        data   = [int(v, 16) if isinstance(v, str) else int(v) for v in test_def["send"]]
        delay  = float(test_def.get("delay_sec", 5.0))

        def _send(can_id=can_id, data=data, delay=delay):
            time.sleep(delay)
            print(f"[Inject] 0x{can_id:03X} at t+{delay}s")
            pm.send_edit(can_id, data)

        t = threading.Thread(target=_send, daemon=True)
        t.start()
        threads.append(t)
    return threads


def _run_stack(c: dict) -> None:
    pm = ProcessManager(base_dir=BASE_DIR,
                        startup_delay=c["startup_sec"],
                        connect_delay=c["connect_sec"])

    print("[Setup] Cleaning up stale processes...")
    pm.stop_all()
    time.sleep(1)
    _clear_log(c["vsoc_log"])

    print("[Setup] Starting stack...")
    pm.start_all()

    threads = _fire_injections(pm, c["tests"])
    print(f"[Setup] Running {c['run_sec']}s...")
    time.sleep(c["run_sec"])

    for t in threads:
        t.join(timeout=2)

    print("[Setup] Stopping...")
    pm.stop_all()


def run_tests(config_path: str, parse_only: bool = False) -> int:
    c = load_config(config_path)
    console = ConsoleReporter(color=True)
    console.suite_header(len(c["tests"]))

    if not parse_only:
        _run_stack(c)
        time.sleep(2)
    else:
        print("[Setup] --parse-only: skipping stack\n")

    vsoc_log = c["vsoc_log"]
    print(f"[Parser] Reading {vsoc_log}")
    if not os.path.exists(vsoc_log):
        print(f"[Parser] ERROR: log not found: {vsoc_log}")
        return 1

    all_frames = parse_log(vsoc_log)
    print(f"[Parser] {len(all_frames)} frames parsed")

    print("[Parser] CAN ID summary:")
    for can_id, info in sorted(summary(all_frames).items()):
        print(f"         0x{can_id:03X}  count={info['count']:4d}  "
              f"DLC={info['dlc']:2d}  pages={sorted(info['pages'])}")
    print()

    validator   = Validator(all_frames)
    all_results: list[TestCaseResult] = []
    for i, test_def in enumerate(c["tests"], 1):
        result = TestCaseResult(
            test_def.get("name", f"Test {i}"),
            test_def.get("description", ""),
            validator.run_all(test_def.get("checks", [])),
        )
        all_results.append(result)
        console.test_result(result, i, len(c["tests"]))

    console.suite_summary(all_results)

    results_dir = os.path.join(BASE_DIR, "Results")
    print(f"  Text : {write_text_report(all_results, results_dir)}")
    print(f"  HTML : {write_html_report(all_results, results_dir)}\n")

    return 1 if any(not r.passed for r in all_results) else 0


def main() -> int:
    p = argparse.ArgumentParser(description="CAN Integration Test Runner")
    p.add_argument("--config", default=DEFAULT_CONFIG)
    p.add_argument("--parse-only", action="store_true")
    args = p.parse_args()
    return run_tests(args.config, args.parse_only)


if __name__ == "__main__":
    sys.exit(main())
