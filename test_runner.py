import os
import re
import time
import subprocess
import xml.etree.ElementTree as ET
from datetime import datetime

# Signal-level support (encode/decode/check by signal using the DBCs).
# Optional: if cantools or the DBCs are missing, byte-mode test cases still run;
# only signal-mode cases (send_signals / expect_signals) will report an error.
try:
    from signal_codec import SignalCodec
except Exception as _e:          # pragma: no cover
    SignalCodec = None
    _CODEC_IMPORT_ERROR = _e

_CODEC = None


def get_codec():
    """Lazily build a single SignalCodec (loads the 3 channel DBCs once)."""
    global _CODEC
    if _CODEC is None:
        if SignalCodec is None:
            raise RuntimeError(
                "signal_codec/cantools not available: %s" % _CODEC_IMPORT_ERROR)
        _CODEC = SignalCodec()
    return _CODEC

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
    So sánh mảng dữ liệu mong đợi và thực tế (byte-mode).
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


def validate_signals(channel, ident, actual_list, expected_signals):
    """
    So sánh theo TÍN HIỆU (signal-mode): chỉ kiểm tra các signal được liệt kê,
    các bit còn lại của frame được bỏ qua. `actual_list` là ['11','22',...] (hex).
    """
    if not actual_list:
        return False, "Không tìm thấy dữ liệu CAN ID này trong logcat."

    try:
        payload = bytes(int(b, 16) for b in actual_list)
    except ValueError:
        return False, f"Dữ liệu logcat không hợp lệ: {' '.join(actual_list)}"

    try:
        ok, bad = get_codec().check(channel, ident, payload, expected_signals)
    except Exception as e:
        return False, f"Lỗi giải mã signal: {e}"

    if ok:
        return True, "Passed"
    detail = " | ".join(f"{s}: got {g} want {w}" for s, g, w in bad)
    return False, f"Signal mismatch -> {detail}"


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
                <th>Mode</th>
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
                <td>{res.get('mode', 'byte')}</td>
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


# ---- UIAutomator validator -------------------------------------------------

def dump_ui_hierarchy(serial, retries=2):
    """
    Lấy cây giao diện hiện tại trên màn hình AAOS (XML) qua `uiautomator dump`.
    Trả về chuỗi XML, hoặc None nếu thất bại.
    """
    base = (f"adb -s {serial} " if serial else "adb ")
    last = ""
    for _ in range(retries + 1):
        try:
            out = subprocess.run(base + "shell uiautomator dump", shell=True,
                                 capture_output=True, text=True, timeout=30)
            blob = (out.stdout or "") + (out.stderr or "")
            m = re.search(r"dumped to:\s*(\S+)", blob)
            path = m.group(1).strip() if m else "/sdcard/window_dump.xml"
            xml = subprocess.run(base + f"shell cat {path}", shell=True,
                                 capture_output=True, text=True, timeout=30).stdout
            if "<hierarchy" in xml:
                return xml
            last = blob.strip() or "no <hierarchy> in dump"
        except Exception as e:
            last = str(e)
        time.sleep(1)
    print(f"  [UI] dump failed: {last}")
    return None


_UI_BOOL_ATTRS = {
    "checked": "checked", "enabled": "enabled", "selected": "selected",
    "focused": "focused", "clickable": "clickable",
}


def _match_ui_condition(nodes, cond):
    """
    Một điều kiện là 1 dict. Cách tìm node (locator) + thuộc tính cần kiểm tra:
      resource_id / content_desc / class_name : locator
      text            : khớp text chính xác (vừa locator vừa assertion)
      text_contains   : text chứa chuỗi con
      checked/enabled/selected/focused/clickable : true/false
      present         : true (mặc định) phải tồn tại; false phải KHÔNG tồn tại
    """
    def a(n, k):
        return n.get(k, "") or ""

    cands = nodes
    if "resource_id" in cond:
        cands = [n for n in cands if a(n, "resource-id") == cond["resource_id"]]
    if "content_desc" in cond:
        cands = [n for n in cands if a(n, "content-desc") == cond["content_desc"]]
    if "class_name" in cond:
        cands = [n for n in cands if a(n, "class") == cond["class_name"]]
    if "text" in cond:
        cands = [n for n in cands if a(n, "text") == cond["text"]]
    if "text_contains" in cond:
        cands = [n for n in cands if cond["text_contains"] in a(n, "text")]
    for key, xmlattr in _UI_BOOL_ATTRS.items():
        if key in cond:
            want = bool(cond[key])
            cands = [n for n in cands if (a(n, xmlattr) == "true") == want]

    present = cond.get("present", True)
    found = len(cands) > 0
    if present and not found:
        return False, f"not found: {cond}"
    if not present and found:
        return False, f"should be absent: {cond}"
    return True, ""


def validate_ui(serial, conditions):
    """
    Kiểm tra giao diện theo danh sách điều kiện. Tất cả phải thoả -> Passed.
    """
    xml = dump_ui_hierarchy(serial)
    if not xml:
        return False, "UIAutomator: không lấy được cây giao diện"
    try:
        root = ET.fromstring(xml)
    except ET.ParseError as e:
        return False, f"UIAutomator: lỗi parse XML ({e})"

    nodes = list(root.iter("node"))
    fails = []
    for cond in conditions:
        ok, msg = _match_ui_condition(nodes, cond)
        if not ok:
            fails.append(msg)
    if fails:
        return False, "UI mismatch -> " + " | ".join(fails)
    return True, "Passed"


# ---- VHAL (CarProperty) validator ------------------------------------------
# Map friendly names -> VehicleProperty IDs (from `cmd car_service
# get-carpropertyconfig`). Add your own / vendor props as needed; you can also
# just use a raw id like 0x15200505 in the YAML.
VHAL_PROP_IDS = {
    "EV_CHARGE_SWITCH": 0x11200F42, "DISTANCE_DISPLAY_UNITS": 0x11400600,
    "EV_CHARGE_TIME_REMAINING": 0x11400F43, "RANGE_REMAINING": 0x11600308,
    "EV_BATTERY_LEVEL": 0x11600309, "ENV_OUTSIDE_TEMPERATURE": 0x11600703,
    "EV_CHARGE_PERCENT_LIMIT": 0x11600F40, "HVAC_DEFROSTER": 0x13200504,
    "HVAC_AC_ON": 0x15200505, "HVAC_RECIRC_ON": 0x15200508,
    "HVAC_DUAL_ON": 0x15200509, "HVAC_AUTO_ON": 0x1520050A,
    "HVAC_POWER_ON": 0x15200510, "HVAC_AUTO_RECIRC_ON": 0x15200512,
    "HVAC_FAN_SPEED": 0x15400500, "HVAC_FAN_DIRECTION": 0x15400501,
    "HVAC_TEMPERATURE_CURRENT": 0x15600502, "HVAC_TEMPERATURE_SET": 0x15600503,
}
VHAL_AREA_IDS = {
    "GLOBAL": 0x0, "FRONT_WINDSHIELD": 0x1, "REAR_WINDSHIELD": 0x2,
    "ROW_1_LEFT": 0x1, "ROW_1_RIGHT": 0x4, "ROW_2_LEFT": 0x10, "ROW_2_RIGHT": 0x40,
}
# Override if your build uses a different shell verb / argument order.
VHAL_GET_CMD = "cmd car_service get-property-value {prop} {area}"


def _resolve_vhal_prop(p):
    if isinstance(p, int):
        return p
    s = str(p).strip()
    if s in VHAL_PROP_IDS:
        return VHAL_PROP_IDS[s]
    return int(s, 0)                       # "0x15200505" / "354613509"


def _resolve_vhal_area(a):
    if a is None:
        return 0
    if isinstance(a, int):
        return a
    s = str(a).strip()
    if s in VHAL_AREA_IDS:
        return VHAL_AREA_IDS[s]
    return int(s, 0)


def _parse_vhal_value(raw):
    """Pull the value out of get-property-value output (format varies by build)."""
    if not raw:
        return None
    for pat in (r"mValue\s*=\s*\[?\s*([^\],}\s]+)",     # CarPropertyValue{... mValue=1}
                r"\bvalues?\s*[:=]\s*\[?\s*([^\],}\s]+)"):  # "value: 1" / "value=1"
        m = re.search(pat, raw, re.IGNORECASE)
        if m:
            return m.group(1).strip()
    nums = re.findall(r"[-+]?\d+\.?\d*", raw)   # last-resort: last number printed
    return nums[-1] if nums else None


def _vhal_equal(got, want, tol):
    if got is None:
        return False
    g = str(got).strip().lower()
    if g in ("true", "false"):
        g = "1" if g == "true" else "0"
    if isinstance(want, bool):
        want = 1 if want else 0
    elif isinstance(want, str) and want.lower() in ("true", "false"):
        want = 1 if want.lower() == "true" else 0
    try:
        return abs(float(g) - float(want)) <= tol
    except ValueError:
        return str(got).strip() == str(want)


def get_vhal_property(serial, prop_id, area_id=0):
    base = f"adb -s {serial} shell " if serial else "adb shell "
    cmd = base + VHAL_GET_CMD.format(prop=hex(prop_id), area=hex(area_id))
    try:
        out = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=20)
        return (out.stdout or "") + (out.stderr or "")
    except Exception as e:
        print(f"  [VHAL] read failed: {e}")
        return None


def validate_vhal(serial, conditions):
    """
    Đọc giá trị property từ CarService và so sánh. Mỗi điều kiện:
      property : tên (HVAC_AC_ON) hoặc id (0x15200505)
      area     : tên (ROW_1_LEFT) / id / bỏ trống = GLOBAL(0)
      value    : giá trị mong đợi (bool/int so khớp tuyệt đối, float theo tol)
      tol      : sai số cho float (mặc định 0.5)
    """
    fails = []
    for c in conditions:
        try:
            prop = _resolve_vhal_prop(c["property"])
            area = _resolve_vhal_area(c.get("area", 0))
        except (KeyError, ValueError) as e:
            fails.append(f"bad vhal condition {c}: {e}")
            continue
        raw = get_vhal_property(serial, prop, area)
        got = _parse_vhal_value(raw)
        if "value" in c and not _vhal_equal(got, c["value"], c.get("tol", 0.5)):
            fails.append(f"{c['property']}@{c.get('area', 'GLOBAL')}: "
                         f"got {got} want {c['value']}")
    if fails:
        return False, "VHAL mismatch -> " + " | ".join(fails)
    return True, "Passed"


def resolve_can_id(tc, channel):
    """
    Xác định CAN ID dạng '0xNNN' để vừa INJECT vừa tìm trong logcat.
    Ưu tiên: can_id -> message (tra DBC) -> tách từ tên test.
    """
    can_id = tc.get("can_id", "")
    if can_id:
        return can_id
    if tc.get("message"):
        try:
            return "0x%X" % get_codec().frame_id(channel, tc["message"])
        except Exception:
            pass
    name = tc.get("name", "")
    return next((w for w in name.split() if w.lower().startswith("0x")), "0x000")


def build_send_array(tc, channel, ident):
    """
    Trả về list ['0xNN', ...] để bơm. Nếu có 'send_signals' thì mã hoá qua DBC,
    ngược lại dùng 'send' (byte-mode) như cũ.
    """
    if "send_signals" in tc:
        return get_codec().encode_hex(channel, ident, tc["send_signals"])
    return tc.get("send", [])


def run_test_cases(sim_proc, test_cases_yaml, logcat_path, serial=None):
    """
    Hàm chính để chạy danh sách test cases truyền vào.
    Hỗ trợ 2 chế độ:
      - byte-mode : send / expect là mảng byte (như cũ)
      - signal-mode: send_signals / expect_signals là dict {tên_signal: giá_trị}
    """
    import time

    results = []

    for tc in test_cases_yaml:
        tc_name = tc.get("name", "Unknown")
        channel = tc.get("channel", 1)
        timeout_ms = tc.get("timeout", 1000)

        # Định danh message: ưu tiên 'message', fallback 'can_id'
        ident = tc.get("message") or tc.get("can_id")
        can_id = resolve_can_id(tc, channel)

        signal_mode = ("send_signals" in tc) or ("expect_signals" in tc)
        ui_mode = ("expect_ui" in tc)
        vhal_mode = ("expect_vhal" in tc)
        parts = []
        if signal_mode:
            parts.append("signal")
        if vhal_mode:
            parts.append("vhal")
        if ui_mode:
            parts.append("ui")
        mode_str = "+".join(parts) if parts else "byte"

        # 1. Bơm dữ liệu
        try:
            send_array = build_send_array(tc, channel, ident)
        except Exception as e:
            # Không mã hoá được signal -> ghi nhận FAIL và bỏ qua phần còn lại
            print(f"  -> ❌ Encode error for {tc_name}: {e}")
            results.append({"name": tc_name, "can_id": can_id, "channel": channel,
                            "mode": mode_str, "passed": False,
                            "detail": f"Encode error: {e}"})
            continue

        if send_array:
            hex_data_str = " ".join([str(b).replace("0x", "").replace("0X", "")
                                     for b in send_array])
            cmd = f"INJECT {channel} {can_id} {hex_data_str}\n"
            sim_proc.stdin.write(cmd)
            sim_proc.stdin.flush()
            print(f"  -> Injected {can_id} ({mode_str}-mode)")

        # 2. Đợi vECU xử lý và xuất log
        time.sleep(10)

        # 3. Validate: gộp kết quả của các loại kiểm tra có khai báo.
        checks = []

        # 3a. Kiểm tra CAN qua logcat (signal-mode hoặc byte-mode)
        if "expect_signals" in tc:
            dump_logcat(serial, logcat_path)
            actual_data = get_latest_can_data_from_logcat(logcat_path, can_id)
            checks.append(validate_signals(channel, ident, actual_data,
                                           tc["expect_signals"]))
        elif ("expect" in tc) or ("expected" in tc):
            dump_logcat(serial, logcat_path)
            actual_data = get_latest_can_data_from_logcat(logcat_path, can_id)
            expected = tc.get("expect", tc.get("expected", []))
            checks.append(validate_payload(expected, actual_data))

        # 3b. Kiểm tra VHAL property qua CarService
        if vhal_mode:
            checks.append(validate_vhal(serial, tc["expect_vhal"]))

        # 3c. Kiểm tra giao diện qua UIAutomator
        if ui_mode:
            checks.append(validate_ui(serial, tc["expect_ui"]))

        # 3d. Không khai báo gì -> coi như pass (giữ hành vi cũ)
        if not checks:
            dump_logcat(serial, logcat_path)
            actual_data = get_latest_can_data_from_logcat(logcat_path, can_id)
            checks.append(validate_payload(tc.get("expect", []), actual_data))

        is_passed = all(ok for ok, _ in checks)
        detail = " ; ".join(d for _, d in checks)

        status_str = "✅ PASS" if is_passed else "❌ FAIL"
        print(f"  -> Result: {status_str} | {detail}")

        # 4. Lưu Result
        results.append({
            "name": tc_name,
            "can_id": can_id,
            "channel": channel,
            "mode": mode_str,
            "passed": is_passed,
            "detail": detail
        })

    return results
