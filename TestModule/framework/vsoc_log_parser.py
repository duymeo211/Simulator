"""
Parser for vSoC/rx_debug.log.

Log format written by vSoC_Test:

  Summary line (skipped by parser):
    [09:36:56.035] [RX][Page-2] parse result: 9 msg(s)

  Frame line:
    [09:36:56.035] [RX] Page-2 | ID=0x381 | DLC=32 | 00 07 00 3C 00 ...

  Startup lines (skipped):
    [09:36:41.476] [RX] Thread started for Page-0
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import List, Optional


# =========================================================================
# Data model
# =========================================================================

@dataclass
class VsocFrame:
    timestamp:  datetime
    page:       int           # 0 / 1 / 2
    can_id:     int
    dlc:        int
    data:       bytes

    def hex(self) -> str:
        return " ".join(f"{b:02X}" for b in self.data)

    def byte_at(self, offset: int) -> Optional[int]:
        return self.data[offset] if offset < len(self.data) else None

    def matches_mask(self,
                     mask:     bytes,
                     expected: bytes) -> bool:
        for i, (m, e) in enumerate(zip(mask, expected)):
            if i >= len(self.data):
                return False
            if (self.data[i] & m) != (e & m):
                return False
        return True

    def __repr__(self) -> str:
        ts = self.timestamp.strftime("%H:%M:%S.%f")[:-3]
        return (f"[{ts}] Page-{self.page} "
                f"ID=0x{self.can_id:03X} DLC={self.dlc} "
                f"[{self.hex()[:24]}{'...' if self.dlc > 8 else ''}]")


# =========================================================================
# Parser
# =========================================================================

# [23:44:07.073] Page-0 | 0x381 | 0x381 | DLC=32 | 00 07 00 3C ...
_RE_FRAME = re.compile(
    r"^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]\s+"   # [1] timestamp
    r"Page-(\d+)\s+\|\s+"                    # [2] page
    r"(0x[0-9A-Fa-f]+)\s+\|\s+"             # [3] CAN ID
    r"0x[0-9A-Fa-f]+\s+\|\s+"               # skip duplicate ID field
    r"DLC=(\d+)\s+\|\s+"                    # [4] DLC
    r"((?:[0-9A-Fa-f]{2}\s*)+)"             # [5] hex data
)


def _parse_hex(s: str) -> bytes:
    return bytes(int(t, 16) for t in s.split() if t)


def parse_log(log_path: str,
              can_id:   Optional[int] = None,
              page:     Optional[int] = None) -> List[VsocFrame]:
    """
    Parse entire vSoC rx_debug.log, return matching VsocFrame list.

    Parameters
    ----------
    can_id : filter by CAN ID (None = all)
    page   : filter by page number (None = all)
    """
    frames: List[VsocFrame] = []

    try:
        with open(log_path, "r", errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        return frames

    today    = datetime.now().date()
    last_ts: "datetime | None" = None

    for line in lines:
        m = _RE_FRAME.match(line.rstrip())
        if not m:
            continue

        ts = datetime.strptime(m.group(1), "%H:%M:%S.%f").replace(
             year=today.year, month=today.month, day=today.day)

        # Detect midnight rollover: timestamp went backward across 00:00
        if last_ts is not None and ts < last_ts:
            today += timedelta(days=1)
            ts = ts.replace(year=today.year, month=today.month, day=today.day)
        last_ts = ts
        pg  = int(m.group(2))
        cid = int(m.group(3), 16)
        dlc = int(m.group(4))
        dat = _parse_hex(m.group(5))

        if can_id is not None and cid != can_id:
            continue
        if page is not None and pg != page:
            continue

        frames.append(VsocFrame(
            timestamp = ts,
            page      = pg,
            can_id    = cid,
            dlc       = dlc,
            data      = dat,
        ))

    return frames


def summary(frames: List[VsocFrame]) -> dict:
    """
    Summarise a frame list: {can_id: {"count": N, "pages": set, "dlc": N}}.
    """
    result: dict = {}
    for f in frames:
        entry = result.setdefault(f.can_id, {"count": 0, "pages": set(), "dlc": f.dlc})
        entry["count"] += 1
        entry["pages"].add(f.page)
    return result
