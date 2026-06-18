import subprocess
import time
import sys

# --- CẤU HÌNH TEST ---
_TX_COMMANDS    = b"3\n1\n"              # TX on (3) → channel 1 (1)
_LOGCAT_FILTER  = "CustomVehicleHardware"  # keyword trong message để lọc log
_CAN_ID         = "0x382"                  # CAN ID cần theo dõi

def run_command_background(cmd):
    # Khởi chạy một tiến trình ngầm không làm treo script Python
    return subprocess.Popen(
        cmd,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
    )
def run_command_with_background(cmd):
    # Mở cửa sổ terminal mới và chạy lệnh trong đó (có thể thấy output)
    # shell=False để CREATE_NEW_CONSOLE áp dụng trực tiếp lên exe, không qua cmd.exe
    return subprocess.Popen(
        cmd,
        shell=False,
        creationflags=subprocess.CREATE_NEW_CONSOLE
    )

def is_android_booted():
    result = subprocess.run("adb shell getprop sys.boot_completed", shell=True, capture_output=True, text=True)
    value = result.stdout.strip()
    stderr = result.stderr.strip()

    if "offline" in stderr:
        print("  [ADB] Thiết bị offline, đang reconnect...")
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
        print("❌ Không tìm thấy emulator đang hoạt động trong ADB. Hủy test.")
        result = subprocess.run("adb devices", shell=True, capture_output=True, text=True)
        print(result.stdout)
        sys.exit(1)

    print(f"  [ADB] Thiết bị: {serial}")
    print("--- Đang thiết lập ADB Port Forwarding ---")

    port_mappings = [
        (5003, 9001),
        (5004, 9002),
        (5005, 9003)
    ]

    for local_port, device_port in port_mappings:
        command = f"adb -s {serial} forward tcp:{local_port} tcp:{device_port}"
        print(f"Đang thực thi: {command}")
        try:
            result = subprocess.run(command, shell=True, capture_output=True, text=True, check=True)
            print(f"✅ Thành công: PC:{local_port} -> Emulator:{device_port}")
        except subprocess.CalledProcessError as e:
            print(f"❌ Lỗi cổng {local_port}: {e.stderr.strip()}")
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
    print(f"  [LOGCAT] Đang ghi log vào log\\logcat.txt ...")

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
    print("Hệ thống đã dọn dẹp xong.")

def cleanup_previous_session():
    print("--- Dọn dẹp tiến trình cũ ---")
    for proc in ["emulator.exe", "qemu-system-x86_64.exe", "simulator.exe", "vecu_vm2.exe"]:
        subprocess.run(f"taskkill /F /IM {proc} /T", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)  # Chờ các tiến trình cũ tắt hoàn toàn
    subprocess.run("adb kill-server", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    subprocess.run("adb start-server", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    print("Dọn dẹp xong.\n")

def main():
    cleanup_previous_session()
    print("--- BƯỚC 1: SETUP HỆ THỐNG ---")
    
    # 1. Chạy CAN Simulator (stdin piped để điều khiển từ Python)
    print("Đang khởi chạy CAN Simulator...")
    sim_proc = subprocess.Popen(
        r"C:\Development\Simulator\TestModule\Simulator\simulator.exe",
        shell=False,
        stdin=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW
    )
    time.sleep(2) # Chờ simulator mở port TCP
    
    # 2. Bật Android Emulator
    # Thay 'vsoc_emu' bằng tên AVD thực tế của bạn
    print("Đang bật Android Emulator...")
    emulator_cmd = r"C:\Android\emulator\emulator.exe -avd vsoc_emu -no-audio -no-snapshot"
    emu_proc = run_command_background(emulator_cmd)
    
    # Chờ Android boot hoàn toàn (vòng lặp timeout 90 giây)
    timeout = 500
    start_time = time.time()
    while time.time() - start_time < timeout:
        if emu_proc.poll() is not None:
            print(f"Lỗi: Android Emulator đã crash (exit code {emu_proc.returncode}). Hủy test.")
            return
        if is_android_booted():
            print("Android Emulator đã sẵn sàng!")
            break
        print("Đang chờ Android boot...")
        time.sleep(5)
    else:
        print("Lỗi: Android boot quá lâu. Hủy test.")
        return
    
    # Chờ ADB ổn định sau khi boot xong trước khi forward port
    print("Đang chờ ADB ổn định...")
    time.sleep(3)
    result = subprocess.run("adb devices", shell=True, capture_output=True, text=True)
    print(result.stdout.strip())

    setup_adb_forwarding()

    # 3. Chạy vECU
    print("Đang khởi chạy vECU...")
    vecu_proc = subprocess.Popen(
        [r"C:\Development\Simulator\TestModule\vECU\vecu_vm2.exe", "config.ini"],
        shell=False,
        cwd=r"C:\Development\Simulator\TestModule\vECU",
        creationflags=subprocess.CREATE_NEW_CONSOLE
    )
    time.sleep(5) # Chờ các bên thiết lập kết nối TCP với nhau

    print("\n--- BƯỚC 2: THỰC THI KIỂM THỬ ---")
    serial = get_emulator_serial()
    try:
        print("Đang chạy các test case...")

        start_logcat_to_file(serial)

        # Kích hoạt simulator: TX on (3) → channel 1 (1)
        print("Gửi lệnh tới simulator...")
        sim_proc.stdin.write(_TX_COMMANDS)
        sim_proc.stdin.flush()
        print("Đang thu thập log CAN... kiểm tra log\\logcat.txt để xem kết quả.")
        time.sleep(50)

    except Exception as e:
        print(f"Lỗi trong quá trình test: {e}")

    finally:
        teardown()

if __name__ == "__main__":
    main()