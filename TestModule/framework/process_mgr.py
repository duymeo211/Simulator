"""
Process manager: start the CAN simulator via CanSimulatorCs.dll (loaded
in-process through pythonnet/CLR), then start vSoC_Test.exe and
vecu_vm2.exe as separate console processes.

Why DLL instead of simulator.exe
---------------------------------
The C# SimulatorEngine is the authoritative CAN bus implementation.
Loading it via CLR gives direct API access (EditTxData, GetTxMessages)
without needing to parse stdin menu prompts.

Startup sequence:
  1. Load CanSimulatorCs.dll via CLR  (lazy, first call only)
  2. ConfigReader.Load(config.ini)
  3. SimulatorEngine(config, base_dir).Start()
  4. SetTxEnabled(ch, True) for all channels  (0-based channel index)
  5. Start vSoC_Test.exe in a new console window
  6. Start vecu_vm2.exe   in a new console window

Runtime edit:
  engine.EditTxData(channel_0based, msg_index_0based, System.Byte[])

Stop:
  engine.Stop()  +  taskkill /F /T /PID for vSoC and vECU
"""
from __future__ import annotations

import configparser
import os
import subprocess
import sys
import time
from typing import List, Optional, Tuple


_EXTRA_IMAGE_NAMES: List[str] = [
    "RX Log.exe",
    "TX Log.exe",
    "vSoC TX.exe",
    "vSoC RX.exe",
]


class ProcessManager:
    """
    Manages the CAN simulator (via DLL) and vSoC/vECU subprocesses.

    The simulator runs in-process; vSoC_Test.exe and vecu_vm2.exe are
    started as separate console processes tracked by PID.
    """

    def __init__(self,
                 base_dir:      str,
                 startup_delay: float = 5.0,
                 connect_delay: float = 5.0):
        self.base_dir      = os.path.abspath(base_dir)
        self.startup_delay = startup_delay
        self.connect_delay = connect_delay
        self._engine        = None
        self._channel_count = 0
        self._pids: List[int] = []

        # CLR types — populated once in _load_clr()
        self._Array          = None
        self._Byte           = None
        self._ConfigReader   = None
        self._SimulatorEngine = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def start_all(self) -> None:
        """Start stack in order: SimulatorEngine → vSoC → vECU."""
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

        Searches all channels for the CAN ID via engine.GetTxMessages(),
        then calls engine.EditTxData(channel, msg_index, byte_array).

        Returns True if the command was sent, False on any failure.
        """
        if self._engine is None:
            print("  [PM] send_edit: engine not started")
            return False

        loc: Optional[Tuple[int, int]] = self._find_message(can_id)
        if loc is None:
            print(f"  [PM] send_edit: CAN ID 0x{can_id:03X} not found in any channel")
            return False

        channel, msg_index = loc
        net_data = self._Array[self._Byte](data)

        try:
            self._engine.EditTxData(channel, msg_index, net_data)
        except Exception as exc:
            print(f"  [PM] send_edit: EditTxData failed — {exc}")
            return False

        hex_str = " ".join(f"{b:02X}" for b in data)
        print(f"  [PM] Edited 0x{can_id:03X} ch{channel} msg{msg_index}: "
              f"[{hex_str[:32]}{'...' if len(hex_str) > 32 else ''}]")
        return True

    def stop_all(self) -> None:
        """Stop the simulator engine and kill every tracked subprocess."""
        if self._engine is not None:
            try:
                self._engine.Stop()
                print("  [PM] SimulatorEngine stopped")
            except Exception as exc:
                print(f"  [PM] engine.Stop() error: {exc}")
            self._engine = None

        for pid in self._pids:
            self._kill_pid(pid)
        self._pids.clear()

        for name in ["vSoC_Test.exe", "vecu_vm2.exe"]:
            self._kill_image(name)

        for name in _EXTRA_IMAGE_NAMES:
            self._kill_image(name)

        time.sleep(1.0)

    # ------------------------------------------------------------------
    # Internal starters
    # ------------------------------------------------------------------

    def _load_clr(self, dll_dir: str) -> None:
        """Load CLR types from CanSimulatorCs.dll (idempotent)."""
        if self._SimulatorEngine is not None:
            return

        # The TestModule/CanSimulatorCs/ directory would shadow the CLR namespace
        # 'CanSimulatorCs' if its parent (TestModule) is in sys.path.
        # Temporarily remove it so Python resolves the namespace from the DLL.
        parent = os.path.dirname(dll_dir)          # .../CanSimulatorCs
        grandparent = os.path.dirname(parent)       # .../TestModule
        saved = [p for p in sys.path
                 if os.path.abspath(p) == os.path.abspath(grandparent)]
        sys.path = [p for p in sys.path
                    if os.path.abspath(p) != os.path.abspath(grandparent)]
        sys.path.insert(0, dll_dir)

        try:
            import clr  # type: ignore
            clr.AddReference("System")
            from System import Array, Byte  # type: ignore
            clr.AddReference("CanSimulatorCore")
            from CanSimulatorCs import ConfigReader, SimulatorEngine  # type: ignore
        finally:
            sys.path.extend(saved)

        self._Array           = Array
        self._Byte            = Byte
        self._ConfigReader    = ConfigReader
        self._SimulatorEngine = SimulatorEngine

    def _start_simulator(self) -> None:
        """Load CanSimulatorCs.dll, create and start the SimulatorEngine."""
        cs_dir      = os.path.join(self.base_dir, "CanSimulatorCs")
        dll_dir     = os.path.join(cs_dir, "bin")
        config_path = os.path.join(cs_dir, "config.ini")

        self._load_clr(dll_dir)

        # Read channel count from config so we know how many to enable
        cfg = configparser.ConfigParser()
        cfg.read(config_path)
        self._channel_count = int(cfg.get("general", "channel_count", fallback=1))

        print("  [PM] Loading CanSimulatorCs engine...")
        config        = self._ConfigReader.Load(config_path)
        self._engine  = self._SimulatorEngine(config, cs_dir)
        self._engine.Start()

        for ch in range(self._channel_count):
            self._engine.SetTxEnabled(ch, True)

        print(f"  [PM] SimulatorEngine started  "
              f"({self._channel_count} channel(s), TX enabled)")

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
        vecu_dir = os.path.join(self.base_dir, "vECU")
        exe      = os.path.join(vecu_dir, "vmcu_vm2.exe")
        ini      = os.path.join(vecu_dir, "config.ini")
        args     = [exe, ini] if os.path.exists(ini) else [exe]
        print("  [PM] Starting vecu_vm2.exe")
        proc = subprocess.Popen(
            args,
            cwd           = vecu_dir,
            creationflags = subprocess.CREATE_NEW_CONSOLE,
        )
        self._pids.append(proc.pid)
        print(f"  [PM] vecu_vm2.exe PID={proc.pid}")

    # ------------------------------------------------------------------
    # Message lookup
    # ------------------------------------------------------------------

    def _find_message(self, can_id: int) -> Optional[Tuple[int, int]]:
        """
        Search all channels for a CAN ID via engine.GetTxMessages().
        Returns (channel_0based, msg_index_0based) or None.
        """
        for ch in range(self._channel_count):
            tx_messages = self._engine.GetTxMessages(ch)
            for i in range(tx_messages.Count):
                msg = tx_messages[i]
                mid = int(msg.Id) if hasattr(msg, "Id") else int(msg.ID)
                if mid == can_id:
                    return (ch, i)
        return None

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
