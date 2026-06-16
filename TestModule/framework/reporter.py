"""
Report generators: console (colored), plain text, HTML.
"""
from __future__ import annotations

import os
from datetime import datetime
from typing import Dict, List, Any

from .validator import CheckResult


# =========================================================================
# ANSI colors (Windows 10+ VT100)
# =========================================================================

def _enable_vt100() -> None:
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleMode(
            ctypes.windll.kernel32.GetStdHandle(-11), 7)
    except Exception:
        pass


_GREEN  = "\033[32m"
_RED    = "\033[31m"
_YELLOW = "\033[33m"
_BOLD   = "\033[1m"
_RESET  = "\033[0m"


def _c(text: str, color: str, use_color: bool) -> str:
    return f"{color}{text}{_RESET}" if use_color else text


# =========================================================================
# Data classes
# =========================================================================

class TestCaseResult:
    def __init__(self, name: str, description: str,
                 checks: List[CheckResult]):
        self.name        = name
        self.description = description
        self.checks      = checks

    @property
    def passed(self) -> bool:
        return all(c.passed for c in self.checks)

    @property
    def pass_count(self) -> int:
        return sum(1 for c in self.checks if c.passed)

    @property
    def fail_count(self) -> int:
        return sum(1 for c in self.checks if not c.passed)


# =========================================================================
# Console reporter
# =========================================================================

class ConsoleReporter:
    def __init__(self, color: bool = True):
        self.color = color
        if color:
            _enable_vt100()

    def suite_header(self, total: int) -> None:
        print()
        print(_c("=" * 60, _BOLD, self.color))
        print(_c(f"  CAN Integration Test Suite  ({total} test(s))", _BOLD, self.color))
        print(_c("=" * 60, _BOLD, self.color))
        print()

    def test_result(self, result: TestCaseResult,
                    idx: int, total: int) -> None:
        verdict = "PASS" if result.passed else "FAIL"
        color   = _GREEN if result.passed else _RED

        print(_c(f"[{idx}/{total}] {result.name}", _BOLD, self.color))
        if result.description:
            print(f"       {result.description}")

        for chk in result.checks:
            icon  = "✓" if chk.passed else "✗"
            clr   = _GREEN if chk.passed else _RED
            line  = f"  {icon} [{chk.status}] {chk.name}"
            if chk.detail:
                line += f"  →  {chk.detail}"
            print(_c(line, clr, self.color))

        print(_c(
            f"  ── {verdict}  "
            f"(PASS={result.pass_count}  FAIL={result.fail_count})",
            color, self.color
        ))
        print()

    def suite_summary(self, results: List[TestCaseResult]) -> None:
        passed = sum(1 for r in results if r.passed)
        failed = len(results) - passed
        clr    = _GREEN if failed == 0 else _RED
        print(_c("=" * 60, _BOLD, self.color))
        print(_c(
            f"  RESULT: {passed}/{len(results)} PASSED   {failed} FAILED",
            clr, self.color
        ))
        print(_c("=" * 60, _BOLD, self.color))
        print()


# =========================================================================
# Text report
# =========================================================================

def write_text_report(results: List[TestCaseResult],
                      out_dir: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join(out_dir, f"test_report_{ts}.txt")

    passed = sum(1 for r in results if r.passed)

    with open(path, "w", encoding="utf-8") as f:
        f.write("CAN Integration Test Report\n")
        f.write(f"Generated : {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"Tests     : {len(results)}\n")
        f.write(f"Passed    : {passed}\n")
        f.write(f"Failed    : {len(results) - passed}\n")
        f.write("=" * 60 + "\n\n")

        for i, r in enumerate(results, 1):
            verdict = "PASS" if r.passed else "FAIL"
            f.write(f"[{i}] {r.name}  ──  {verdict}\n")
            if r.description:
                f.write(f"    {r.description}\n")
            for chk in r.checks:
                icon = "[P]" if chk.passed else "[F]"
                line = f"    {icon} {chk.name}"
                if chk.detail:
                    line += f"  |  {chk.detail}"
                f.write(line + "\n")
            f.write(f"    PASS={r.pass_count}  FAIL={r.fail_count}\n")
            f.write("-" * 60 + "\n\n")

    return path


# =========================================================================
# HTML report
# =========================================================================

def write_html_report(results: List[TestCaseResult],
                      out_dir: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join(out_dir, f"test_report_{ts}.html")

    passed = sum(1 for r in results if r.passed)
    failed = len(results) - passed
    suite_color = "#2ecc71" if failed == 0 else "#e74c3c"

    rows = ""
    for i, r in enumerate(results, 1):
        verdict_clr = "#2ecc71" if r.passed else "#e74c3c"
        verdict     = "PASS" if r.passed else "FAIL"

        check_rows = ""
        for chk in r.checks:
            clr  = "#2ecc71" if chk.passed else "#e74c3c"
            icon = "✓" if chk.passed else "✗"
            check_rows += (
                f'<tr>'
                f'<td style="color:{clr};font-weight:bold;padding:3px 10px">{icon} {chk.status}</td>'
                f'<td style="padding:3px 10px">{chk.name}</td>'
                f'<td style="padding:3px 10px;color:#555;font-size:.85em">{chk.detail}</td>'
                f'</tr>'
            )

        rows += f"""
<tr style="background:#f9f9f9">
  <td style="padding:8px 10px;font-weight:bold">[{i}] {r.name}</td>
  <td style="padding:8px 10px;font-weight:bold;color:{verdict_clr}">{verdict}</td>
  <td style="padding:8px 10px;color:#666;font-size:.9em">{r.description}</td>
</tr>
<tr>
  <td colspan="3" style="padding:0 20px 12px">
    <table style="width:100%;border-collapse:collapse">{check_rows}</table>
  </td>
</tr>
"""

    html = f"""<!DOCTYPE html>
<html lang="en">
<head><meta charset="UTF-8">
<title>CAN Test Report</title>
<style>
  body  {{ font-family:'Segoe UI',Arial,sans-serif; margin:32px; background:#f4f6f9; }}
  h1    {{ color:#2c3e50; margin-bottom:4px; }}
  .box  {{ background:#fff; border-radius:8px; padding:20px;
           box-shadow:0 2px 8px rgba(0,0,0,.1); margin-bottom:24px; }}
  .badge{{ display:inline-block; padding:6px 16px; border-radius:4px;
           color:#fff; font-weight:bold; font-size:1.1em; }}
  table {{ border-collapse:collapse; width:100%; }}
  th    {{ background:#2c3e50; color:#fff; padding:10px; text-align:left; }}
</style>
</head>
<body>
<h1>CAN Integration Test Report</h1>
<p style="color:#888">{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>

<div class="box">
  <span class="badge" style="background:{suite_color}">
    {passed}/{len(results)} PASSED
  </span>
  &nbsp;
  <span style="color:#e74c3c;font-weight:bold">{failed} FAILED</span>
</div>

<div class="box">
<table>
  <thead><tr><th>Test</th><th>Result</th><th>Description</th></tr></thead>
  <tbody>{rows}</tbody>
</table>
</div>
</body></html>
"""

    with open(path, "w", encoding="utf-8") as f:
        f.write(html)

    return path
