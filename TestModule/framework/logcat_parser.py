"""
logcat_parser.py
----------------
Parses ADB logcat output captured by start_logcat_to_file() in run_auto.py.

Logcat line format (-v time):
  MM-DD HH:MM:SS.mmm priority/tag(pid): message

CAN RX line example:
  06-19 00:50:55.168 I/android.hardware.automotive.vehicle@V3-custom-service(  539):
      CustomVehicleHardware: RX CAN ID=0x204 DLC=32 data=00 F0 0F 00 ...

Returns List[VsocFrame] so Validator works with no changes.
page is always 0 (logcat has no page concept).
"""
from __future__ import annotations

import re
from datetime import datetime
from typing import List, Optional

from .vsoc_log_parser import VsocFrame


_RE_CAN_RX = re.compile(
    r"^(\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})"    # [1] MM-DD HH:MM:SS.mmm
    r".+?"                                          # priority/tag(pid):
    r"RX\s+CAN\s+ID=(0x[0-9A-Fa-f]+)"               # [2] CAN ID
    r"\s+DLC=(\d+)"                                 # [3] DLC
    r"\s+data=([\s0-9A-Fa-f]+)"                     # [4] hex bytes
)


def parse_logcat(log_path: str,
                 can_id: Optional[int] = None) -> List[VsocFrame]:
    """
    Parse a logcat.txt file, return VsocFrame list for all RX CAN lines.

    Parameters
    ----------
    log_path : path to the logcat file written by start_logcat_to_file()
    can_id   : optional filter; None = return all CAN IDs
    """
    frames: List[VsocFrame] = []
    year = datetime.now().year

    try:
        with open(log_path, "r", errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        return frames

    for line in lines:
        m = _RE_CAN_RX.match(line.rstrip())
        if not m:
            continue

        # "06-19 00:50:55.168" → datetime
        ts = datetime.strptime(
            f"{year}-{m.group(1).strip()}", "%Y-%m-%d %H:%M:%S.%f"
        )
        cid = int(m.group(2), 16)
        dlc = int(m.group(3))
        dat = bytes(int(b, 16) for b in m.group(4).split() if b)

        if can_id is not None and cid != can_id:
            continue

        frames.append(VsocFrame(
            timestamp=ts,
            page=0,
            can_id=cid,
            dlc=dlc,
            data=dat,
        ))

    return frames
