import os
import re
import subprocess
import xml.etree.ElementTree as ET
from datetime import datetime

# ==========================================
# 1. PARSER & VALIDATOR
# ==========================================

def get_latest_can_data_from_logcat(log_path, can_id):
    """
    Quét file logcat.txt để lấy chuỗi data MỚI NHẤT của CAN ID cụ thể.
    Ví dụ log: ... CustomVehicleHardware: RX CAN ID=0x381 DLC=32 data=11 22 33 ...
    """
    if not os.path.exists(log_path):
        return None

    # Thêm re.IGNORECASE để phòng trường hợp hex in hoa/in thường
    pattern = re.compile(rf"RX CAN ID={can_id} DLC=\d+ data=(.*)", re.IGNORECASE)
    latest_data = None

    with open(log_path, 'r', encoding='utf-8') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                # Lấy chuỗi "11 22 33 44 00..." và cắt thành list ['11', '22', '33', ...]
                latest_data = match.group(1).strip().split()
                
    return latest_data

def validate_payload(expected_list, actual_list):
    """
    So sánh mảng dữ liệu mong đợi và thực tế.
    """
    if not actual_list:
        return False, "Không tìm thấy dữ liệu CAN ID này trong logcat."

    # Chuẩn hóa mảng Expected: Bỏ "0x", in hoa, thêm số 0 ở đầu nếu cần (VD: 0x1 -> 01)
    exp_norm = [x.replace("0x", "").strip().upper().zfill(2) for x in expected_list]
    
    # Thực tế logcat đã in hoa/in thường tùy máy, ta cứ in hoa hết để so sánh
    act_norm = [x.upper() for x in actual_list]

    # Kiểm tra xem thực tế có chứa đúng đoạn dữ liệu mong đợi không
    check_len = len(exp_norm)
    if act_norm[:check_len] == exp_norm:
        return True, "Passed"
    else:
        return False, f"Expected: {' '.join(exp_norm)} | Actual: {' '.join(act_norm[:check_len])}"

# ==========================================
# 2. REPORT GENERATORS
# ==========================================

def generate_junit_xml(results, output_path="log/junit_report.xml"):
    testsuites = ET.Element("testsuites")
    testsuite = ET.SubElement(testsuites, "testsuite", name="vECU_Automated_Tests", 
                              tests=str(len(results)), 
                              failures=str(sum(1 for r in results if not r['passed'])))

    for res in results:
        tc = ET.SubElement(testsuite, "testcase", classname="RuntimeInject", name=res['name'])
        if not res['passed']:
            failure = ET.SubElement(tc, "failure", message="Validation Mismatch")
            failure.text = res['detail']

    tree = ET.ElementTree(testsuites)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    tree.write(output_path, encoding="utf-8", xml_declaration=True)
    print(f"✅ Đã xuất báo cáo JUnit XML: {output_path}")


def generate_html_report(results, output_path="log/report.html"):
    html_content = f"""
    <!DOCTYPE html>
    <html>
    <head>
        <title>vECU Automation Test Report</title>
        <style>
            body {{ font-family: Arial, sans-serif; margin: 20px; }}
            h2 {{ color: #333; }}
            table {{ width: 100%; border-collapse: collapse; margin-top: 20px; }}
            th, td {{ border: 1px solid #ddd; padding: 10px; text-align: left; }}
            th {{ background-color: #f4f4f4; }}
            .pass {{ color: green; font-weight: bold; }}
            .fail {{ color: red; font-weight: bold; }}
        </style>
    </head>
    <body>
        <h2>🚀 vECU Automation Test Report</h2>
        <p><strong>Date:</strong> {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}</p>
        <table>
            <tr>
                <th>Test Case</th>
                <th>CAN ID</th>
                <th>Status</th>
                <th>Details (Expected vs Actual)</th>
            </tr>
    """
    
    for res in results:
        status_html = "<span class='pass'>PASS</span>" if res['passed'] else "<span class='fail'>FAIL</span>"
        html_content += f"""
            <tr>
                <td>{res['name']}</td>
                <td>{res['can_id']}</td>
                <td>{status_html}</td>
                <td>{res['detail']}</td>
            </tr>
        """
        
    html_content += """
        </table>
    </body>
    </html>
    """
    
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(html_content)
    print(f"✅ Đã xuất báo cáo HTML: {output_path}")

# ==========================================
# 3. TEST EXECUTOR
# ==========================================

def dump_logcat(serial, log_path):
    """
    Ép Android xả toàn bộ logcat hiện tại ra file ngay lập tức.
    """
    cmd = f"adb -s {serial} logcat -d -v time" if serial else "adb logcat -d -v time"
    try:
        output = subprocess.check_output(cmd, shell=True, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='ignore')
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(output)
    except Exception as e:
        print(f"  [Error] Failed to dump logcat: {e}")

def run_test_cases(sim_proc, test_cases_yaml, logcat_path, serial=None):
    """
    Hàm chính để chạy danh sách test cases truyền vào.
    """
    import time
    
    results = []

    for tc in test_cases_yaml:
        tc_name = tc.get("name", "Unknown")
        can_id = tc.get("can_id", "")
        # Nếu YAML chưa khai báo can_id, lấy từ Tên (VD: "Runtime Inject: 0x381 single change")
        if not can_id:
            can_id = next((w for w in tc_name.split() if w.startswith("0x")), "0x000")
            
        channel = tc.get("channel", 1)
        timeout_ms = tc.get("timeout", 1000)
        expected = tc.get("expect", tc.get("expected", [])) # Hỗ trợ cả chữ expect và expected
        
        # 1. Bơm dữ liệu
        send_array = tc.get("send", [])
        if send_array:
            hex_data_str = " ".join([b.replace("0x", "") for b in send_array])
            
            cmd = f"INJECT {channel} {can_id} {hex_data_str}\n"
            sim_proc.stdin.write(cmd)
            sim_proc.stdin.flush()
            print(f"  -> Injected {can_id}")

        # 2. Đợi vECU xử lý và xuất log (Chỉ cần 2s, không cần tới 20s)
        time.sleep(10)

        # Đẩy toàn bộ dữ liệu logcat xuống ổ cứng một cách đồng bộ
        dump_logcat(serial, logcat_path)

        # 3. Validate ngay trên file vừa ghi
        actual_data = get_latest_can_data_from_logcat(logcat_path, can_id)
        is_passed, detail = validate_payload(expected, actual_data)
        
        status_str = "✅ PASS" if is_passed else "❌ FAIL"
        print(f"  -> Result: {status_str} | {detail}")

        # 4. Lưu Result
        results.append({
            "name": tc_name,
            "can_id": can_id,
            "passed": is_passed,
            "detail": detail
        })

    return results