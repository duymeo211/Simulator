"""
Lookup (channel_number, message_index) for a given CAN ID by scanning
the .can files referenced in config.ini.

Returns 1-based values matching the simulator's Edit TX menu prompts:
  channel: 1, 2, or 3
  message: 1 .. N  (order in the .can file)
"""
from __future__ import annotations

import configparser
import os
import re
from typing import Dict, List, Optional, Tuple


_ID_RE    = re.compile(r"^\s*id\s*:\s*0[xX]([0-9A-Fa-f]+)", re.IGNORECASE)
_LABEL_RE = re.compile(r"^\s*-\s+label\s*:", re.IGNORECASE)


def _read_can_ids(can_file: str) -> List[int]:
    """Return ordered list of CAN IDs as they appear in a .can file."""
    ids: List[int] = []
    if not os.path.exists(can_file):
        return ids
    with open(can_file, "r", encoding="utf-8") as f:
        for line in f:
            m = _ID_RE.match(line)
            if m:
                ids.append(int(m.group(1), 16))
    return ids


def build_index(base_dir: str,
                config_ini: str = "config.ini") -> Dict[int, Tuple[int, int]]:
    """
    Read config.ini and all referenced .can files.
    Returns {can_id: (channel_1based, msg_index_1based)}.
    """
    cfg_path = os.path.join(base_dir, config_ini)
    cfg = configparser.ConfigParser()
    cfg.read(cfg_path)

    index: Dict[int, Tuple[int, int]] = {}

    ch_num = 1
    while True:
        section = f"channel{ch_num}"
        if not cfg.has_section(section):
            break
        can_file_rel = cfg.get(section, "can_file", fallback=None)
        if can_file_rel:
            can_file = os.path.join(base_dir, can_file_rel)
            for msg_idx, can_id in enumerate(_read_can_ids(can_file), start=1):
                if can_id not in index:          # first occurrence wins
                    index[can_id] = (ch_num, msg_idx)
        ch_num += 1

    return index


def lookup(can_id: int, base_dir: str) -> Optional[Tuple[int, int]]:
    """Return (channel, msg_index) for a CAN ID, or None if not found."""
    return build_index(base_dir).get(can_id)
