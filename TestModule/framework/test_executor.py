"""
test_executor.py
----------------
Runs a single YAML test case definition against a live system.

Flow
----
1. For each runtime_inject entry:
     wait delay_sec → call sim_editor.send_edit(can_id, data)
2. Wait settle_sec for frames to propagate into the log
3. Parse the vSoC rx_debug.log
4. Run all checks via Validator
5. Return a TestCaseResult

The checks (data_byte, data_window, …) are already implemented in
validator.py — this file only adds the timing/inject layer on top.
"""
from __future__ import annotations

import time
from typing import Any, Callable, Dict, List

from .vsoc_log_parser import VsocFrame
from .validator import Validator
from .reporter import TestCaseResult


class TestExecutor:
    """
    Parameters
    ----------
    sim_editor
        Any object with send_edit(can_id: int, data: list[int]) -> bool.
    log_path : str
        Path to the log file to validate against.
    log_parser : Callable[[str], List[VsocFrame]]
        Function that reads log_path and returns VsocFrame list.
        Use parse_logcat from logcat_parser.py (default in test_runner.py)
        or parse_log from vsoc_log_parser.py for rx_debug.log.
    settle_sec : float
        Seconds to wait after last inject before parsing the log.
    """

    def __init__(self,
                 sim_editor,
                 log_path:   str,
                 log_parser: Callable[[str], List[VsocFrame]],
                 settle_sec: float = 2.0):
        self._editor   = sim_editor
        self._log_path = log_path
        self._parser   = log_parser
        self._settle   = settle_sec

    def run(self, test_case: Dict[str, Any]) -> TestCaseResult:
        name        = test_case.get("name", "unnamed")
        description = test_case.get("description", "").strip()

        # ---- runtime_inject ----
        for inject in test_case.get("runtime_inject", []):
            can_id    = int(inject["can_id"], 16)
            delay_sec = float(inject.get("delay_sec", 0))
            data      = [
                int(b, 16) if isinstance(b, str) else int(b)
                for b in inject["data"]
            ]

            if delay_sec > 0:
                print(f"  [EXEC] Waiting {delay_sec}s before injecting "
                      f"0x{can_id:03X} ...")
                time.sleep(delay_sec)

            sent = self._editor.send_edit(can_id, data)
            if sent:
                preview = " ".join(f"{b:02X}" for b in data[:8])
                print(f"  [EXEC] Injected 0x{can_id:03X}: {preview} ...")

        # ---- settle ----
        if self._settle > 0:
            print(f"  [EXEC] Settling {self._settle}s for frames to propagate ...")
            time.sleep(self._settle)

        # ---- parse log ----
        frames = self._parser(self._log_path)
        print(f"  [EXEC] Parsed {len(frames)} frame(s) from log")

        # ---- validate ----
        check_results = Validator(frames).run_all(test_case.get("checks", []))

        return TestCaseResult(name, description, check_results)
