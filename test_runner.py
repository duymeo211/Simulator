"""
test_runner.py
--------------
Bridge between run_auto.py and TestModule/framework.

Exposes two things:

  SimEditor        - wraps an existing sim_proc so send_edit() can be
                     called from run_auto.py without duplicating logic.

  run_test_case()  - loads a YAML test case, runs runtime_inject +
                     checks, returns a TestCaseResult ready for reporting.

Usage in run_auto.py
--------------------
    import yaml
    from test_runner import run_test_case
    from framework.reporter import ConsoleReporter, write_text_report

    with open("test_cases/runtime_inject.yaml") as f:
        test_case = yaml.safe_load(f)

    result = run_test_case(sim_proc, test_case)
    ConsoleReporter().test_result(result, 1, 1)
"""
from __future__ import annotations

import os
import sys
from typing import Any, Dict

# ---- make the framework package importable ----
_TESTMODULE_ROOT = os.path.join(os.path.dirname(__file__), "TestModule")
if _TESTMODULE_ROOT not in sys.path:
    sys.path.insert(0, _TESTMODULE_ROOT)

from framework.process_mgr import ProcessManager                        # noqa: E402
from framework.test_executor import TestExecutor                        # noqa: E402
from framework.logcat_parser import parse_logcat                        # noqa: E402
from framework.reporter import (                                        # noqa: E402
    TestCaseResult,
    ConsoleReporter,
    write_text_report,
)

# ---- paths ----
_BASE_DIR    = _TESTMODULE_ROOT
_LOGCAT_PATH = r"C:\Development\Simulator\log\logcat.txt"


# =============================================================================
# SimEditor
# =============================================================================

class SimEditor:
    """
    Injects an existing simulator Popen handle into ProcessManager so
    send_edit() can be used from run_auto.py.

    Parameters
    ----------
    sim_proc : subprocess.Popen
        The running simulator process opened with stdin=PIPE in run_auto.py.
    """

    def __init__(self, sim_proc):
        self._pm = ProcessManager(base_dir=_BASE_DIR)
        self._pm._sim_proc = sim_proc   # inject — PM does not own this process

    def send_edit(self, can_id: int, data: list) -> bool:
        """
        Change a CAN message payload at runtime.

        Parameters
        ----------
        can_id : int    e.g. 0x381
        data   : list   new payload bytes, e.g. [0x11, 0x22, 0x33, 0x44, ...]
        """
        return self._pm.send_edit(can_id, data)


# =============================================================================
# run_test_case
# =============================================================================

def run_test_case(sim_proc,
                  test_case:    Dict[str, Any],
                  logcat_path:  str   = _LOGCAT_PATH,
                  settle_sec:   float = 2.0) -> TestCaseResult:
    """
    Execute one YAML test case definition end-to-end, validating against
    the ADB logcat.txt captured by start_logcat_to_file() in run_auto.py.

    Parameters
    ----------
    sim_proc     : subprocess.Popen  running simulator (stdin=PIPE)
    test_case    : dict              loaded from YAML
    logcat_path  : str               path to logcat.txt
    settle_sec   : float             seconds to wait after inject before parsing

    Returns
    -------
    TestCaseResult  (pass/fail per check, ready for ConsoleReporter)
    """
    editor   = SimEditor(sim_proc)
    executor = TestExecutor(editor,
                            log_path=logcat_path,
                            log_parser=parse_logcat,
                            settle_sec=settle_sec)
    return executor.run(test_case)
