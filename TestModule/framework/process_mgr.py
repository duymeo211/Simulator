"""
Process manager: start simulator → vSoC → vECU in the correct order,
enable TX by piping menu commands through stdin, then stop all.

How TX is enabled
-----------------
The simulator already supports automated stdin input (menu.c line 388):

    if (!_isatty(_fileno(stdin)))   // stdin is a pipe → use fgets
    {
        fgets(line, sizeof(line), stdin);
        *out_choice = atoi(line);
        return true;
    }

stdin is kept open as a persistent PIPE so commands can be written at
any time during the run — enabling both startup config and runtime edits.

Startup command sequence written immediately after Popen:
    "3\n"  →  Start TX  (menu choice)
    "0\n"  →  All channels

Runtime edit command sequence (send_edit):
    "2\n"           →  Edit TX
    "{ch}\n"        →  channel number (1-based)
    "{msg}\n"       →  message index  (1-based, order in .can file)
    "{hex data}\n"  →  new payload as "AA BB CC ..."

Start order:   simulator  →  vSoC  →  vECU
Stop order:    taskkill /F /T /PID  (kills process tree + console window)
"""
from __future__ import annotations

import os
import subprocess
import time
from typing import List, Optional, Tuple

from .can_lookup import lookup as _can_lookup


_EXTRA_IMAGE_NAMES: List[str] = [
    "RX Log.exe",
    "TX Log.exe",
    "vSoC TX.exe",
    "vSoC RX.exe",
]


class ProcessManager:
    """
    Manages simulator.exe, vSoC_Test.exe, vecu_vm2.exe for one test run.

    All processes are tracked by PID; stop_all() kills each tree with /T
    which also closes the console (conhost.exe) window.

    The simulator's stdin is a persistent PIPE — use send_edit() to change
    a message's payload while the simulator is running.
    """

    _TX_COMMANDS = b"3\n0\n"   # Start TX → All channels

    def __init__(self,
                 base_dir:      str,
                 startup_delay: float = 5.0,
                 connect_delay: float = 5.0):
        self.base_dir      = os.path.abspath(base_dir)
        self.Sim           = os.path.join(self.base_dir, "Simulator")
        self.startup_delay = startup_delay
        self.connect_delay = connect_delay
        self._sim_proc: "subprocess.Popen | None" = None
        self._pids:     List[int] = []

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def start_all(self) -> None:
        """Start stack in order: simulator → vSoC → vECU."""
        self._start_simulator()
        print(f"  [PM] Waiting {self.startup_delay}s for simulator startup...")
        time.sleep(self.startup_delay)

        self._start_vsoc()
        print("  [PM] Waiting 3s for vSoC...")
        time.sleep(3.0)

        self._start_vecu()
        print(f"  [PM] Waiting {self.connect_delay}s for vECU ↔ simulator connection...")
        time.sleep(self.connect_delay)

        print("  [PM] Stack is running.  TX enabled.")

    def send_edit(self, can_id: int, data: List[int]) -> bool:
        """
        Change a message's payload while the simulator is running.

        Looks up which channel and message index owns `can_id` from the
        .can files, then writes the Edit TX command sequence to stdin:
            2  →  Edit TX
            {channel}
            {msg_index}
            {hex bytes space-separated}

        Returns True if the command was sent, False if lookup failed or
        the simulator is not running.
        """
        if self._sim_proc is None or self._sim_proc.stdin is None:
            print("  [PM] send_edit: simulator not running")
            return False

        loc: Optional[Tuple[int, int]] = _can_lookup(can_id, self.Sim)
        if loc is None:
            print(f"  [PM] send_edit: CAN ID 0x{can_id:03X} not found in any .can file")
            return False

        channel, msg_index = loc
        hex_str = " ".join(f"{b:02X}" for b in data)

        cmd = f"2\n{channel}\n{msg_index}\n{hex_str}\n".encode()
        try:
            self._sim_proc.stdin.write(cmd)
            self._sim_proc.stdin.flush()
        except OSError as exc:
            print(f"  [PM] send_edit: pipe write failed — {exc}")
            return False

        print(f"  [PM] Edited 0x{can_id:03X} ch{channel} msg{msg_index}: "
              f"[{hex_str[:32]}{'...' if len(hex_str) > 32 else ''}]")
        return True

    def stop_all(self) -> None:
        """Kill every tracked process tree then clean up stragglers."""
        for pid in self._pids:
            self._kill_pid(pid)
        self._pids.clear()
        self._sim_proc = None

        for name in ["simulator.exe", "vSoC_Test.exe", "vecu_vm2.exe"]:
            self._kill_image(name)

        for name in _EXTRA_IMAGE_NAMES:
            self._kill_image(name)

        time.sleep(1.0)

    # ------------------------------------------------------------------
    # Internal starters
    # ------------------------------------------------------------------

    def _start_simulator(self) -> None:
        """
        Start simulator.exe with stdin=PIPE (persistent, non-tty).
        Write the initial TX enable commands immediately, then keep the
        pipe open for runtime edits via send_edit().
        """
        sim_dir = os.path.join(self.base_dir, "Simulator")
        exe = os.path.join(sim_dir, "simulator.exe")
        ini = os.path.join(sim_dir, "config.ini")
        log = open(os.path.join(sim_dir, "log", "sim_stdout.log"), "w")

        print("  [PM] Starting simulator (stdin=PIPE)")
        self._sim_proc = subprocess.Popen(
            [exe, ini],
            cwd    = sim_dir,
            stdin  = subprocess.PIPE,
            stdout = log,
            stderr = subprocess.STDOUT,
        )
        self._pids.append(self._sim_proc.pid)

        # Enable TX immediately — simulator reads these as soon as it starts
        self._sim_proc.stdin.write(self._TX_COMMANDS)
        self._sim_proc.stdin.flush()
        print(f"  [PM] simulator.exe PID={self._sim_proc.pid}  TX enable sent")

    def _start_vsoc(self) -> None:
        vsoc_dir = os.path.join(self.base_dir, "vSoC")
        exe      = os.path.join(vsoc_dir, "vSoC_Test.exe")
        print("  [PM] Starting vSoC_Test.exe")
        proc = subprocess.Popen(
            [exe],
            cwd           = vsoc_dir,
            creationflags = subprocess.CREATE_NEW_CONSOLE,
        )
        self._pids.append(proc.pid)
        print(f"  [PM] vSoC_Test.exe PID={proc.pid}")

    def _start_vecu(self) -> None:
        release_dir = os.path.join(self.base_dir, "vECU")
        exe         = os.path.join(release_dir, "vecu_vm2.exe")
        ini         = os.path.join(release_dir, "config.ini")
        args        = [exe, ini] if os.path.exists(ini) else [exe]
        print("  [PM] Starting vecu_vm2.exe")
        proc = subprocess.Popen(
            args,
            cwd           = release_dir,
            creationflags = subprocess.CREATE_NEW_CONSOLE,
        )
        self._pids.append(proc.pid)
        print(f"  [PM] vecu_vm2.exe PID={proc.pid}")

    # ------------------------------------------------------------------
    # Kill helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _kill_pid(pid: int) -> None:
        result = subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(pid)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if result.returncode == 0:
            print(f"  [PM] Killed PID {pid} (+ tree)")
        else:
            print(f"  [PM] PID {pid} already gone (skipped)")

    @staticmethod
    def _kill_image(exe_name: str) -> None:
        result = subprocess.run(
            ["taskkill", "/F", "/IM", exe_name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if result.returncode == 0:
            print(f"  [PM] Killed {exe_name}")
