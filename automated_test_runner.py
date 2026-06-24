import subprocess
import time
import sys
import signal
import os
import glob 
import yaml 
from test_runner import run_test_cases, generate_junit_xml, generate_html_report

# --- SETUP TEST ---
_LOGCAT_FILTER  = "CustomVehicleHardware"  # filter log 

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


def clear_logcat_buffer(serial):
    """Xóa trắng bộ nhớ logcat cũ trước khi chạy test."""
    cmd = f"adb -s {serial} logcat -c" if serial else "adb logcat -c"
    subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("  [LOGCAT] Cleared old logcat buffer.")


def teardown(sim_proc=None):
    print("\n--- BƯỚC 3: TEARDOWN ---")
    
    serial = get_emulator_serial()
    if serial:
        subprocess.run(f"adb -s {serial} forward --remove-all", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # Tắt an toàn C Simulator để nó kịp lưu log
    if sim_proc:
        print("Gracefully shutting down Simulator...")
        try:
            sim_proc.send_signal(signal.CTRL_C_EVENT)
            sim_proc.wait(timeout=5)
        except Exception as e:
            print(f"Force killing simulator due to timeout: {e}")
            subprocess.run("taskkill /F /IM CanSimulatorCs.exe /T", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
    # Tắt các tiến trình còn lại
    for proc in ["vecu_vm2.exe"]:
        subprocess.run(f"taskkill /F /IM {proc} /T", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run("adb emu kill", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Done cleaning up.")

def cleanup_previous_session():
    print("--- Clean old processes ---")
    for proc in ["emulator.exe", "qemu-system-x86_64.exe", "CanSimulatorCs.exe", "vecu_vm2.exe"]:
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
    
    # 1. Run CAN Simulator in AUTO MODE
    print("Running CAN Simulator in Auto Mode...")
    sim_proc = subprocess.Popen(
        [r"C:\Development\CanSimulatorCsv2.2\CanSimulatorCs\CanSimulatorCs.exe", "--auto"],
        shell=False,
        stdin=subprocess.PIPE,
        text=True, 
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    )
    time.sleep(2)
    
    # 2. Enable Android Emulator
    print("Enabling Android Emulator...")
    emulator_cmd = r"C:\Android\emulator\emulator.exe -avd vsoc_emu -no-audio -no-snapshot -no-window"
    emu_proc = run_command_background(emulator_cmd)
    
    timeout = 120
    start_time = time.time()
    while time.time() - start_time < timeout:
        if emu_proc.poll() is not None:
            print(f"ERROR: Android Emulator crash (exit code {emu_proc.returncode}). Cancel test.")
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
        print("Simulator is running in --auto mode. TX and RX are already active.")

        # Xóa sạch logcat cũ của các lần test trước
        clear_logcat_buffer(serial)

        test_cases_dir = r"C:\Development\Simulator\TestModule\Testcases"
        yaml_files = []

        if len(sys.argv) > 1:
            target_file = sys.argv[1]
            if os.path.exists(target_file) and target_file.endswith(".yaml"):
                yaml_files = [target_file]
                print(f"🎯 Single Mode: Chạy 1 file test duy nhất -> {target_file}")
            else:
                print(f"❌ Lỗi: File không tồn tại hoặc sai định dạng -> {target_file}")
                return
        else:
            yaml_files = glob.glob(os.path.join(test_cases_dir, "*.yaml"))
            print(f"📂 Batch Mode: Tìm thấy {len(yaml_files)} file YAML trong thư mục")

        if not yaml_files:
            print(f"❌ Không tìm thấy file YAML nào!")
        
        for yaml_file in yaml_files:
            print(f"\n==================================================")
            print(f"🚀 Executing Test Suite: {os.path.basename(yaml_file)}")
            print(f"==================================================")
            
            with open(yaml_file, encoding="utf-8") as f:
                test_cases = yaml.safe_load(f)
            
            if not test_cases:
                continue

            logcat_path = r"C:\Development\Simulator\log\logcat.txt"
            
            # Truyền thêm `serial` để hàm run_test_cases tự động dump log
            results = run_test_cases(sim_proc, test_cases, logcat_path, serial)

            generate_junit_xml(results, r"C:\Development\Simulator\log\junit_report.xml")
            generate_html_report(results, r"C:\Development\Simulator\log\report.html")

        print("\n✅ All Test Suites Completed!")

    except Exception as e:
        print(f"Error found: {e}")

    finally:
        teardown(sim_proc)

if __name__ == "__main__":
    main()