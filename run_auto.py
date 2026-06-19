import subprocess
import time
import sys

# --- SETUP TEST ---
_TX_COMMANDS    = b"3\n1\n"                # TX on (3) → channel 1 (1)
_LOGCAT_FILTER  = "CustomVehicleHardware"  # filter log 
_CAN_ID         = "0x390"                  # filter CAN ID 

def run_command_background(cmd):
    return subprocess.Popen(
        cmd,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
    )

def is_android_booted():
    result = subprocess.run("adb shell getprop sys.boot_completed", shell=True, capture_output=True, text=True)
    value = result.stdout.strip()
    stderr = result.stderr.strip()

    if "offline" in stderr:
        print("  [ADB] Devices offline, reconnecting...")
        subprocess.run("adb reconnect", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return False

    if value:
        print(f"  [ADB] sys.boot_completed = '{value}'")
    elif stderr:
        print(f"  [ADB] {stderr}")

    return value == "1"

def get_emulator_serial():
    result = subprocess.run("adb devices", shell=True, capture_output=True, text=True)
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and "emulator" in parts[0] and parts[1] == "device":
            return parts[0]
    return None

def setup_adb_forwarding():
    serial = get_emulator_serial()
    if not serial:
        print("❌ Could not find Android emulator. Cancel test.")
        result = subprocess.run("adb devices", shell=True, capture_output=True, text=True)
        print(result.stdout)
        sys.exit(1)

    print(f"  [ADB] Device: {serial}")
    print("--- Setting ADB Port Forwarding ---")

    port_mappings = [
        (5003, 9001),
        (5004, 9002),
        (5005, 9003)
    ]

    for local_port, device_port in port_mappings:
        command = f"adb -s {serial} forward tcp:{local_port} tcp:{device_port}"
        print(f"Executing: {command}")
        try:
            result = subprocess.run(command, shell=True, capture_output=True, text=True, check=True)
            print(f"Successfull: PC:{local_port} -> Emulator:{device_port}")
        except subprocess.CalledProcessError as e:
            print(f"Port error {local_port}: {e.stderr.strip()}")
            sys.exit(1)

    print("\n--- Forward list ---")
    subprocess.run(f"adb -s {serial} forward --list", shell=True)


_logcat_file = None
_logcat_proc = None

def start_logcat_to_file(serial):
    global _logcat_file, _logcat_proc
    log_path = r"C:\Development\Simulator\log\logcat.txt"
    subprocess.run(f"adb -s {serial} logcat -c", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _logcat_file = open(log_path, "w", encoding="utf-8")
    _logcat_proc = subprocess.Popen(
        f'adb -s {serial} logcat -v time | findstr "{_LOGCAT_FILTER}" | findstr "{_CAN_ID}"',
        shell=True,
        stdout=_logcat_file,
        stderr=subprocess.STDOUT
    )
    print(f"  [LOGCAT] Writing log into log\\logcat.txt ...")

def stop_logcat():
    global _logcat_file, _logcat_proc
    if _logcat_proc:
        _logcat_proc.terminate()
        _logcat_proc = None
    if _logcat_file:
        _logcat_file.close()
        _logcat_file = None

def teardown():
    print("\n--- BƯỚC 3: TEARDOWN ---")
    stop_logcat()
    serial = get_emulator_serial()
    if serial:
        subprocess.run(f"adb -s {serial} forward --remove-all", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for proc in ["vecu_vm2.exe", "simulator.exe"]:
        subprocess.run(f"taskkill /F /IM {proc} /T", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run("adb emu kill", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Done cleaning up.")

def cleanup_previous_session():
    print("--- Clean old processes ---")
    for proc in ["emulator.exe", "qemu-system-x86_64.exe", "simulator.exe", "vecu_vm2.exe"]:
        subprocess.run(f"taskkill /F /IM {proc} /T", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    subprocess.run("adb kill-server", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    subprocess.run("adb start-server", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    print("Done clean.\n")

def main():
    cleanup_previous_session()
    print("--- SETUP SYSTEM ---")
    
    # 1. Run CAN Simulator
    print("Running CAN Simulator...")
    sim_proc = subprocess.Popen(
        r"C:\Development\Simulator\TestModule\Simulator\simulator.exe",
        shell=False,
        stdin=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW
    )
    time.sleep(2)
    
    # 2. Enable Android Emulator
    print("Enabling Android Emulator...")
    emulator_cmd = r"C:\Android\emulator\emulator.exe -avd vsoc_emu -no-audio -no-snapshot -no-window"
    emu_proc = run_command_background(emulator_cmd)
    
    # Waiting for Android boot 
    timeout = 120
    start_time = time.time()
    while time.time() - start_time < timeout:
        if emu_proc.poll() is not None:
            print(f"ERROR: Android Emulator crash (exit code {emu_proc.returncode}). Hủy test.")
            return
        if is_android_booted():
            print("Android Emulator is ready!")
            break
        print("Waiting for Android boot...")
        time.sleep(5)
    else:
        print("ERROR: Unable to boot Android. Cancel test.")
        return
    
    time.sleep(3)
    result = subprocess.run("adb devices", shell=True, capture_output=True, text=True)
    print(result.stdout.strip())

    setup_adb_forwarding()

    # 3. Run vMCU
    print("Running vMCU...")
    vecu_proc = subprocess.Popen(
        [r"C:\Development\Simulator\TestModule\vECU\vecu_vm2.exe", "config.ini"],
        shell=False,
        cwd=r"C:\Development\Simulator\TestModule\vECU",
        creationflags=subprocess.CREATE_NEW_CONSOLE
    )
    time.sleep(5) 

    print("\n--- TESTING EXECUTION ---")
    serial = get_emulator_serial()
    try:
        # Enable TX on simulator
        print("Sending command to simulator...")
        sim_proc.stdin.write(_TX_COMMANDS)
        sim_proc.stdin.flush()

        # Start ADB logcat capture (manual review)
        start_logcat_to_file(serial)

        # Load and run YAML test case
        import yaml
        from test_runner import run_test_case, ConsoleReporter, write_text_report

        test_case_path = r"C:\Development\Simulator\test_cases\runtime_inject.yaml"
        with open(test_case_path, encoding="utf-8") as f:
            test_case = yaml.safe_load(f)

        result = run_test_case(sim_proc, test_case)

        # Report to console and text file
        reporter = ConsoleReporter()
        reporter.test_result(result, 1, 1)
        report_path = write_text_report([result], r"C:\Development\Simulator\log")
        print(f"Report saved: {report_path}")

    except Exception as e:
        print(f"Error found: {e}")

    finally:
        teardown()

if __name__ == "__main__":
    main()