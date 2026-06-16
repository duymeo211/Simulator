"""
Validation logic: run checks defined in test_cases.yaml against
a parsed list of VsocFrame entries.

Check types
-----------
presence        : CAN ID must appear at least once
count           : CAN ID must appear >= min_count times
data_byte       : frame.data[offset] == value  (first matching frame)
data_mask       : (data[i] & mask[i]) == expected[i]  (first match)
data_all_bytes  : ALL frames must match full expected payload exactly
data_consistent : payload must not change across frames (all frames identical)
data_window     : like data_all_bytes but only for frames after after_sec
                  seconds from the first frame (use after runtime inject)
interval        : mean gap between consecutive frames within tolerance
no_unexpected   : CAN ID must NOT appear in the log
"""
from __future__ import annotations

import statistics
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Dict, List, Optional

from .vsoc_log_parser import VsocFrame


# =========================================================================
# Result model
# =========================================================================

@dataclass
class CheckResult:
    name:    str
    passed:  bool
    detail:  str = ""

    @property
    def status(self) -> str:
        return "PASS" if self.passed else "FAIL"


# =========================================================================
# Validator
# =========================================================================

class Validator:
    """
    Run a list of check specifications against a list of VsocFrame entries.

    Each check is a dict (loaded from YAML) with at minimum:
        type: <check_type>
        can_id: "0xXXX"
    """

    def __init__(self, frames: List[VsocFrame]):
        self.frames = frames

    def run_all(self, checks: List[Dict[str, Any]]) -> List[CheckResult]:
        results = []
        for check in checks:
            results.append(self._run_one(check))
        return results

    # ------------------------------------------------------------------

    def _run_one(self, check: Dict[str, Any]) -> CheckResult:
        check_type = check.get("type", "presence")
        raw_id     = check.get("can_id", "0x0")
        can_id     = int(raw_id, 16) if isinstance(raw_id, str) else int(raw_id)
        page       = check.get("page", None)
        name       = check.get("name", f"{check_type} 0x{can_id:03X}")

        subset = [
            f for f in self.frames
            if f.can_id == can_id and (page is None or f.page == page)
        ]

        # ---- dispatch ----
        if check_type == "presence":
            return self._check_presence(name, can_id, subset)

        if check_type == "count":
            return self._check_count(name, can_id, subset,
                                     check.get("min_count", 1))

        if check_type == "data_byte":
            return self._check_data_byte(name, subset,
                                         check["offset"],
                                         check["value"])

        if check_type == "data_mask":
            return self._check_data_mask(name, subset,
                                         check["mask"],
                                         check["expected"])

        if check_type == "interval":
            return self._check_interval(name, can_id, subset,
                                        check["expected_ms"],
                                        check.get("tolerance_ms", 100))

        if check_type == "no_unexpected":
            return self._check_absent(name, can_id, subset)

        if check_type == "data_all_bytes":
            return self._check_data_all_bytes(name, subset,
                                              check["expected"])

        if check_type == "data_consistent":
            return self._check_data_consistent(name, can_id, subset,
                                               check.get("offsets", None))

        if check_type == "data_window":
            return self._check_data_window(name, subset,
                                           check["expected"],
                                           check.get("after_sec",  0.0),
                                           check.get("before_sec", None),
                                           check.get("skip_count", 0))

        return CheckResult(name, False, f"Unknown check type: {check_type}")

    # ------------------------------------------------------------------
    # Individual checks
    # ------------------------------------------------------------------

    @staticmethod
    def _check_presence(name: str, can_id: int,
                        subset: List[VsocFrame]) -> CheckResult:
        if subset:
            return CheckResult(
                name, True,
                f"{len(subset)} frame(s) found (ID=0x{can_id:03X})"
            )
        return CheckResult(
            name, False,
            f"ID=0x{can_id:03X} not found in vSoC log"
        )

    @staticmethod
    def _check_count(name: str, can_id: int,
                     subset: List[VsocFrame],
                     min_count: int) -> CheckResult:
        n = len(subset)
        ok = n >= min_count
        return CheckResult(
            name, ok,
            f"found {n} frame(s), required >= {min_count}"
        )

    @staticmethod
    def _check_data_byte(name: str,
                         subset: List[VsocFrame],
                         offset: int,
                         raw_value: Any) -> CheckResult:
        if not subset:
            return CheckResult(name, False, "No frames found for this CAN ID")
        value = int(raw_value, 16) if isinstance(raw_value, str) else int(raw_value)
        frame = subset[0]   # use first frame
        actual = frame.byte_at(offset)
        if actual is None:
            return CheckResult(name, False,
                               f"DLC={frame.dlc} < offset {offset}")
        ok = (actual == value)
        return CheckResult(
            name, ok,
            f"data[{offset}]=0x{actual:02X} "
            f"{'==' if ok else '!='} 0x{value:02X}"
        )

    @staticmethod
    def _check_data_mask(name: str,
                         subset: List[VsocFrame],
                         raw_mask: List,
                         raw_expected: List) -> CheckResult:
        if not subset:
            return CheckResult(name, False, "No frames found for this CAN ID")
        mask     = bytes(int(v, 16) if isinstance(v, str) else int(v) for v in raw_mask)
        expected = bytes(int(v, 16) if isinstance(v, str) else int(v) for v in raw_expected)
        frame    = subset[0]
        ok = frame.matches_mask(mask, expected)
        if ok:
            return CheckResult(name, True,
                               f"data=[{frame.hex()[:24]}] matches mask")
        # Show mismatches
        mismatches = []
        for i, (m, e) in enumerate(zip(mask, expected)):
            if i < len(frame.data):
                actual = frame.data[i]
                if (actual & m) != (e & m):
                    mismatches.append(
                        f"[{i}]: got 0x{actual:02X} expected 0x{e:02X} (mask 0x{m:02X})")
        return CheckResult(name, False, "; ".join(mismatches[:4]))

    @staticmethod
    def _check_interval(name: str, can_id: int,
                        subset: List[VsocFrame],
                        expected_ms: float,
                        tolerance_ms: float) -> CheckResult:
        if len(subset) < 2:
            return CheckResult(name, False,
                               f"Need >= 2 frames for interval check, got {len(subset)}")
        gaps = []
        for a, b in zip(subset, subset[1:]):
            delta_ms = (b.timestamp - a.timestamp).total_seconds() * 1000
            if delta_ms > 0:
                gaps.append(delta_ms)
        if not gaps:
            return CheckResult(name, False, "No valid time gaps calculated")
        mean_ms = statistics.mean(gaps)
        ok = abs(mean_ms - expected_ms) <= tolerance_ms
        return CheckResult(
            name, ok,
            f"mean interval={mean_ms:.1f}ms, "
            f"expected={expected_ms}±{tolerance_ms}ms "
            f"({len(gaps)} gaps from {len(subset)} frames)"
        )

    @staticmethod
    def _check_absent(name: str, can_id: int,
                      subset: List[VsocFrame]) -> CheckResult:
        if not subset:
            return CheckResult(name, True,
                               f"ID=0x{can_id:03X} correctly absent")
        return CheckResult(name, False,
                           f"ID=0x{can_id:03X} found {len(subset)} unexpected time(s)")

    @staticmethod
    def _check_data_all_bytes(name: str,
                              subset: List[VsocFrame],
                              raw_expected: List) -> CheckResult:
        """Every received frame must carry exactly the specified payload."""
        if not subset:
            return CheckResult(name, False, "No frames found for this CAN ID")

        expected = bytes(
            int(v, 16) if isinstance(v, str) else int(v)
            for v in raw_expected
        )
        failures = []
        for idx, frame in enumerate(subset):
            actual = bytes(frame.data[:len(expected)])
            if actual != expected:
                mismatches = [
                    f"[{i}]: got 0x{a:02X} exp 0x{e:02X}"
                    for i, (a, e) in enumerate(zip(actual, expected))
                    if a != e
                ]
                failures.append(f"frame#{idx}: {'; '.join(mismatches[:4])}")

        if not failures:
            return CheckResult(
                name, True,
                f"all {len(subset)} frames carry expected payload"
            )
        return CheckResult(
            name, False,
            f"{len(failures)}/{len(subset)} frames mismatch — "
            + failures[0]
        )

    @staticmethod
    def _check_data_window(name: str,
                           subset: List[VsocFrame],
                           raw_expected: List,
                           after_sec:  float,
                           before_sec: "float | None",
                           skip_count: int) -> CheckResult:
        """
        Like data_all_bytes but restricted to a time window relative to the
        first frame for this CAN ID:
          after_sec  — only frames at or after this offset  (default 0)
          before_sec — only frames before this offset       (default: no limit)
          skip_count — skip the first N frames in the window (default 0)

        skip_count is used to tolerate in-flight frames that were already
        queued in the simulator when a runtime inject fired.  With 3 vSoC
        pages, set skip_count=3 to discard the last old-data cycle.
        """
        if not subset:
            return CheckResult(name, False, "No frames found for this CAN ID")

        t0 = subset[0].timestamp

        def _offset(f: VsocFrame) -> float:
            return (f.timestamp - t0).total_seconds()

        window = [
            f for f in subset
            if _offset(f) >= after_sec
            and (before_sec is None or _offset(f) < before_sec)
        ]

        window_desc = (
            f"t+{after_sec}s - t+{before_sec}s"
            if before_sec is not None
            else f"after t+{after_sec}s"
        )

        if not window:
            return CheckResult(
                name, False,
                f"No frames in window [{window_desc}] "
                f"(total frames={len(subset)})"
            )

        # Discard leading in-flight frames from the previous data cycle
        if skip_count > 0:
            window = window[skip_count:]
            if not window:
                return CheckResult(
                    name, False,
                    f"No frames left after skipping {skip_count} in-flight "
                    f"frames [{window_desc}]"
                )

        expected = bytes(
            int(v, 16) if isinstance(v, str) else int(v)
            for v in raw_expected
        )
        failures = []
        for idx, frame in enumerate(window):
            actual = bytes(frame.data[:len(expected)])
            if actual != expected:
                mismatches = [
                    f"[{i}]: got 0x{a:02X} exp 0x{e:02X}"
                    for i, (a, e) in enumerate(zip(actual, expected))
                    if a != e
                ]
                failures.append(f"frame#{idx}: {'; '.join(mismatches[:4])}")

        if not failures:
            return CheckResult(
                name, True,
                f"all {len(window)} frames [{window_desc}] carry expected payload"
            )
        return CheckResult(
            name, False,
            f"{len(failures)}/{len(window)} frames mismatch [{window_desc}] — "
            + failures[0]
        )

    @staticmethod
    def _check_data_consistent(name: str,
                               can_id: int,
                               subset: List[VsocFrame],
                               offsets: "List[int] | None") -> CheckResult:
        """
        Verify that the payload does not change between frames.
        If `offsets` is given, only those byte positions are compared.
        """
        if len(subset) < 2:
            return CheckResult(name, False,
                               f"Need >= 2 frames for consistency check, got {len(subset)}")

        ref = subset[0]
        ref_bytes = (
            bytes(ref.data[i] for i in offsets if i < len(ref.data))
            if offsets else bytes(ref.data)
        )

        changed_at = []
        for idx, frame in enumerate(subset[1:], start=1):
            cmp_bytes = (
                bytes(frame.data[i] for i in offsets if i < len(frame.data))
                if offsets else bytes(frame.data)
            )
            if cmp_bytes != ref_bytes:
                changed_at.append(idx)

        if not changed_at:
            scope = f"offsets {offsets}" if offsets else "all bytes"
            return CheckResult(
                name, True,
                f"payload stable across all {len(subset)} frames ({scope})"
            )
        return CheckResult(
            name, False,
            f"payload changed in {len(changed_at)}/{len(subset)} frame(s) "
            f"(first change at frame#{changed_at[0]})"
        )
