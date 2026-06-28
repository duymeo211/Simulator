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

        name_w   = max((len(chk.name)   for chk in result.checks), default=0)
        status_w = max((len(chk.status) for chk in result.checks), default=4)

        for chk in result.checks:
            icon   = "✓" if chk.passed else "✗"
            clr    = _GREEN if chk.passed else _RED
            status = f"[{chk.status}]".ljust(status_w + 2)
            name   = chk.name.ljust(name_w)
            line   = f"  {icon} {status}  {name}"
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
    suite_color = "#27ae60" if failed == 0 else "#e74c3c"

    def badge(label: str, color: str) -> str:
        return (
            f'<span style="display:inline-block;padding:2px 10px;border-radius:999px;'
            f'background:{color};color:#fff;font-size:.78em;font-weight:700;'
            f'letter-spacing:.04em">{label}</span>'
        )

    rows = ""
    for i, r in enumerate(results, 1):
        row_bg     = "#fff" if i % 2 == 0 else "#f8fafc"
        chk_bg     = "#f0fdf4" if i % 2 == 0 else "#eff6ff"
        verdict_bg = "#27ae60" if r.passed else "#e74c3c"
        verdict    = "PASS" if r.passed else "FAIL"

        rows += f"""
<tr style="background:{row_bg};border-top:2px solid #dce1e7">
  <td style="padding:12px 14px;font-weight:700;font-size:.95em;color:#1a202c">
    <span style="color:#94a3b8;font-weight:400;margin-right:6px">[{i}]</span>{r.name}
  </td>
  <td style="padding:12px 14px;white-space:nowrap">{badge(verdict, verdict_bg)}</td>
  <td style="padding:12px 14px;color:#64748b;font-size:.88em">{r.description}</td>
</tr>
"""
        for chk in r.checks:
            clr  = "#16a34a" if chk.passed else "#dc2626"
            icon = "✓" if chk.passed else "✗"
            icon_style = (
                f'display:inline-block;width:18px;height:18px;line-height:18px;'
                f'text-align:center;border-radius:50%;background:{clr};'
                f'color:#fff;font-size:.75em;font-weight:900;margin-right:8px;flex-shrink:0'
            )
            rows += (
                f'<tr style="background:{chk_bg};border-bottom:1px solid #e2e8f0">'
                f'<td style="padding:6px 14px 6px 32px;font-size:.86em;color:#374151;white-space:nowrap">'
                f'<span style="{icon_style}">{icon}</span>{chk.name}</td>'
                f'<td style="padding:6px 14px;text-align:center"></td>'
                f'<td style="padding:6px 14px;font-size:.83em;color:#4b5563;'
                f'font-family:monospace;word-break:break-word">{chk.detail}</td>'
                f'</tr>\n'
            )

    html = f"""<!DOCTYPE html>
<html lang="en">
<head><meta charset="UTF-8">
<title>CAN Test Report</title>
<style>
  *, *::before, *::after {{ box-sizing: border-box; }}
  body  {{ font-family:'Segoe UI',Arial,sans-serif; margin:40px; background:#f1f5f9; color:#1e293b; }}
  h1    {{ color:#1e293b; margin-bottom:2px; font-size:1.6em; }}
  .sub  {{ color:#94a3b8; font-size:.9em; margin-bottom:24px; }}
  .box  {{ background:#fff; border-radius:10px; padding:24px;
           box-shadow:0 1px 4px rgba(0,0,0,.08); margin-bottom:28px; }}
  .summary {{ display:flex; align-items:center; gap:16px; }}
  .pill {{ display:inline-block; padding:8px 22px; border-radius:999px;
           color:#fff; font-weight:700; font-size:1.05em; }}
  .stat {{ font-size:.95em; color:#64748b; }}
  .stat b {{ color:#1e293b; }}
  table {{ border-collapse:collapse; width:100%; }}
  th    {{ background:#1e293b; color:#f8fafc; padding:11px 14px;
           text-align:left; font-size:.85em; letter-spacing:.05em; text-transform:uppercase; }}
  td    {{ vertical-align:middle; }}
  tr:last-child td {{ border-bottom:none; }}
</style>
</head>
<body>
<h1>CAN Integration Test Report</h1>
<p class="sub">{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>

<div class="box">
  <div class="summary">
    <span class="pill" style="background:{suite_color}">{passed}/{len(results)} PASSED</span>
    <span class="stat"><b>{failed}</b> failed &nbsp;·&nbsp; <b>{passed}</b> passed &nbsp;·&nbsp; <b>{len(results)}</b> total</span>
  </div>
</div>

<div class="box">
<table>
  <colgroup><col style="width:32%"><col style="width:90px"><col></colgroup>
  <thead><tr><th>Test</th><th>Result</th><th>Description / Detail</th></tr></thead>
  <tbody>{rows}</tbody>
</table>
</div>
</body></html>
"""

    with open(path, "w", encoding="utf-8") as f:
        f.write(html)

    return path
