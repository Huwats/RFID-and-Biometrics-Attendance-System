import serial
import requests
import time
import threading
import json
import os
import queue
from datetime import datetime

# --- CONFIGURATION ---
WEB_APP_URL = "https://script.google.com/macros/s/AKfycbxfp-jLce-h-B7P70oIH8ANB3bQDBTy4pwAfdVTa0yKBHA8HJ4ebt5M9b_YE0SM4pN0/exec"

COM_PORT = 'COM4'
BAUD_RATE = 9600

# --- FILE PATHS ---
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(BASE_DIR, "offline_logs.json")
DB_CACHE = os.path.join(BASE_DIR, "local_db_cache.json")
CFG_CACHE = os.path.join(BASE_DIR, "local_cfg_cache.json")

# --- SYSTEM STATE ---
log_queue = queue.Queue()
hardware_delete_queue = queue.Queue()
local_scan_tracker = {}
current_tracker_date = ""

authorized_students = []
config = {"lateH": 8, "lateM": 0, "sweepH": 13, "sweepM": 0}
sweep_done_today = False
late_warning_done_today = False


# ==========================================
# OFFLINE VAULT & TXT GENERATOR FUNCTIONS
# ==========================================

def update_text_backup():
    try:
        safe_date = current_tracker_date if current_tracker_date else datetime.now().strftime("%Y-%m-%d")
        dynamic_txt_file = os.path.join(BASE_DIR, f"Offline_Backup_{safe_date}.txt")

        with open(dynamic_txt_file, 'w', encoding='utf-8') as f:
            f.write("=" * 118 + "\n")
            f.write(
                f"{'DATE':<12} | {'TIME-IN':<10} | {'TIME-OUT':<10} | {'STUDENT':<20} | {'ID NUMBER':<12} | {'IN STATUS':<10} | {'OUT STATUS':<12} | {'DURATION':<10}\n")
            f.write("=" * 118 + "\n")

            for sid, data in local_scan_tracker.items():
                date_str = data['date']
                t_in = data['time_in'].strftime("%I:%M %p") if data['time_in'] else "---"
                t_out = data['time_out'].strftime("%I:%M %p") if data['time_out'] else "---"
                f.write(
                    f"{date_str:<12} | {t_in:<10} | {t_out:<10} | {data['name']:<20} | {sid:<12} | {data['in_status']:<10} | {data['out_status']:<12} | {data['duration']:<10}\n")
            f.write("=" * 118 + "\n")
            f.write(f"STATUS: Offline backup for {safe_date}. Data will sync to the cloud when Wi-Fi is restored.\n")
    except Exception as e:
        print(f"[ERROR] Could not redraw TXT file: {e}")


def load_offline_logs():
    if os.path.exists(LOG_FILE):
        try:
            with open(LOG_FILE, 'r') as f:
                logs = json.load(f)
                for log in logs: log_queue.put(log)
                if logs: print(f"[SYSTEM] Loaded {len(logs)} pending logs from Offline Vault.")
        except Exception:
            pass


def save_offline_logs(logs_list):
    try:
        with open(LOG_FILE, 'w') as f:
            json.dump(logs_list, f)
    except:
        pass


def cloud_worker():
    while True:
        if not log_queue.empty():
            items = []
            while not log_queue.empty(): items.append(log_queue.get())
            failed_items = []
            for log in items:
                try:
                    values_str = f"{log['name']},{log['num']},{log['mode']},{log['timestamp']}"
                    resp = requests.post(WEB_APP_URL, json={"command": "insert_row", "values": values_str}, timeout=10)
                    if resp.status_code == 200:
                        print(f"[CLOUD SUCCESS] Synced {log['name']} historical scan to Cloud.")
                    else:
                        raise Exception(f"HTTP {resp.status_code}")
                except Exception as e:
                    failed_items.append(log)

            if failed_items:
                for item in failed_items: log_queue.put(item)
                save_offline_logs(list(log_queue.queue))
                time.sleep(10)
            else:
                save_offline_logs([])
        time.sleep(1)


# ==========================================
# BOOTUP, CACHE, AND BACKGROUND TASKS
# ==========================================

def _background_sync_worker():
    global config
    try:
        resp = requests.post(WEB_APP_URL, json={"command": "get_config"}, timeout=10)
        if resp.status_code == 200:
            new_cfg = resp.json()
            if new_cfg != config:
                config = new_cfg
                with open(CFG_CACHE, 'w') as f:
                    json.dump(config, f)
                try:
                    lH, lM = int(config.get('lateH', 8)), int(config.get('lateM', 0))
                    sH, sM = int(config.get('sweepH', 13)), int(config.get('sweepM', 0))
                    print(
                        f"\n[SYSTEM] Settings Updated: Late Threshold @ {lH:02d}:{lM:02d} | Absent Sweep @ {sH:02d}:{sM:02d}")
                except ValueError:
                    pass
    except Exception:
        pass

    try:
        resp = requests.get(WEB_APP_URL, params={"command": "get_pending_deletes"}, timeout=15)
        if resp.status_code == 200:
            try:
                pending = resp.json()
                if isinstance(pending, list) and len(pending) > 0:
                    print(f"\n[SYSTEM] Found {len(pending)} pending hardware deletions from Cloud Queue.")
                    for fid in pending: hardware_delete_queue.put(fid)
            except ValueError:
                pass
    except Exception:
        pass


def fetch_student_database(ser=None):
    global authorized_students
    if ser and ser.is_open:
        ser.write(b"INIT_SYSTEM\n")
        ser.flush()

    try:
        print("[SYSTEM] Fetching student database from Cloud...")
        response = requests.get(f"{WEB_APP_URL}?command=get_students", timeout=15)
        if response.status_code == 200:
            authorized_students = response.json()
            with open(DB_CACHE, 'w') as f:
                json.dump(authorized_students, f)
            print(f"[SYSTEM] Loaded {len(authorized_students)} students.")
        else:
            raise Exception("Bad HTTP Status")
    except Exception as e:
        print(f"[OFFLINE] No internet! Loading students from local cache...")
        try:
            if os.path.exists(DB_CACHE):
                with open(DB_CACHE, 'r') as f: authorized_students = json.load(f)
                print(f"[SYSTEM] Safely loaded {len(authorized_students)} students offline.")
        except Exception:
            authorized_students = []

    if ser and ser.is_open:
        time.sleep(0.5)
        ser.write(b"SYSTEM_READY\n")
        ser.flush()
    return authorized_students


def confirm_delete_task(fid):
    time.sleep(3)
    try:
        resp = requests.post(WEB_APP_URL, json={"command": "confirm_delete", "finger": str(fid)}, timeout=10)
        if resp.status_code == 200: print(f"[SYSTEM] Cloud Queue Cleared for Finger ID {fid}")
    except Exception:
        pass


def sync_enrollment_task(new_finger, new_uid):
    try:
        payload = {"command": "enroll", "finger": new_finger, "uid": new_uid}
        resp = requests.post(WEB_APP_URL, json=payload, timeout=10)
        if resp.status_code == 200: print("[ENROLLMENT] Successfully saved to Google Sheets.")
    except Exception as e:
        print(f"[ENROLLMENT] Cloud timeout! (Saved to hardware, but not sheets): {e}")


# ==========================================
# MAIN BRIDGE LOGIC
# ==========================================

def start_bridge():
    global config, authorized_students, current_tracker_date, local_scan_tracker, sweep_done_today, late_warning_done_today

    load_offline_logs()
    threading.Thread(target=cloud_worker, daemon=True).start()
    _background_sync_worker()

    try:
        print(f"[BT/USB] Connecting to {COM_PORT}...")
        ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=2)
        time.sleep(7)
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        while True:
            ser.write(b"INIT_SYSTEM\n")
            ser.flush()
            time.sleep(1.5)
            if ser.in_waiting > 0:
                raw_response = ser.readline().decode('utf-8', errors='ignore').strip()
                if "READY" in raw_response:
                    print("[SUCCESS] Arduino Connected!")
                    break

        fetch_student_database(ser)
        last_sync_check = time.time()

        while True:
            now = datetime.now()
            today_date = now.strftime("%Y-%m-%d")

            if time.time() - last_sync_check > 60:
                threading.Thread(target=_background_sync_worker, daemon=True).start()
                last_sync_check = time.time()

            if not hardware_delete_queue.empty():
                fid = hardware_delete_queue.get()
                clean_fid = str(fid).strip()
                print(f"[HARDWARE] Sending DELETE command for Finger ID: {clean_fid}")

                ser.write(f"REMOTE_DELETE,{clean_fid}\n".encode())
                ser.flush()

                authorized_students = [s for s in authorized_students if str(s.get('finger')) != str(clean_fid)]
                with open(DB_CACHE, 'w') as f: json.dump(authorized_students, f)
                threading.Thread(target=confirm_delete_task, args=(clean_fid,), daemon=True).start()

            if current_tracker_date != today_date:
                local_scan_tracker.clear()
                current_tracker_date = today_date
                sweep_done_today = False
                late_warning_done_today = False
                update_text_backup()

            try:
                lH, lM = int(config.get('lateH', 8)), int(config.get('lateM', 0))
                sH, sM = int(config.get('sweepH', 13)), int(config.get('sweepM', 0))

                # 1. Late Warning
                if now.hour == lH and now.minute == lM:
                    if not late_warning_done_today:
                        print(f"\n[SYSTEM] It is {lH:02d}:{lM:02d}. Triggering Late Warning SMS/Emails via Cloud.")
                        threading.Thread(
                            target=lambda: requests.post(WEB_APP_URL, json={"command": "trigger_late_warning"},
                                                         timeout=20), daemon=True).start()
                        late_warning_done_today = True

                # 2. Absent Sweep - Threaded to absolutely prevent freezing Arduino!
                if now.hour == sH and now.minute == sM:
                    if not sweep_done_today:
                        print(f"\n[SYSTEM] It is {sH:02d}:{sM:02d}. Executing Auto-Sweep UI Protocol on Hardware.")

                        ser.write(b"SHOW_SWEEP\n")
                        ser.flush()

                        def do_cloud_sweep_and_reset():
                            try:
                                print("[SYSTEM] Forcing Cloud Database to insert Absent students...")
                                resp = requests.post(WEB_APP_URL, json={"command": "auto_timeout_sweep"}, timeout=20)
                                if resp.status_code == 200:
                                    print("[SYSTEM] Cloud Sweep Completed Successfully!")
                                else:
                                    print(f"[WARNING] Cloud Sweep returned HTTP {resp.status_code}")
                            except Exception as e:
                                print(f"[ERROR] Cloud Sweep connection failed: {e}")

                            # After cloud is done, wait 2 seconds for visual LCD effect, then reset
                            time.sleep(2)
                            ser.write(b"SYSTEM_READY\n")
                            ser.flush()

                        # Put it completely in the background
                        threading.Thread(target=do_cloud_sweep_and_reset, daemon=True).start()
                        sweep_done_today = True
            except ValueError:
                pass

            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if not line: continue

                if line == "PING":
                    ser.write(b"PONG\n")
                    ser.flush()

                elif line == "CHECK_TIME":
                    try:
                        sH, sM = int(config.get('sweepH', 13)), int(config.get('sweepM', 0))
                        # --- FIXED: Use Absent Sweep Time (Class Ended) to lock the doors, NOT Late Threshold! ---
                        if (now.hour > sH) or (now.hour == sH and now.minute >= sM):
                            ser.write(b"TOO_LATE\n")
                        else:
                            ser.write(b"PROCEED\n")
                    except ValueError:
                        ser.write(b"PROCEED\n")
                    # --- FIXED: Guarantee immediate send ---
                    ser.flush()

                elif line == "GET_NEW_ID":
                    used_ids = set()
                    for student in authorized_students:
                        try:
                            used_ids.add(int(student.get('finger', 0)))
                        except ValueError:
                            pass

                    next_id = 1
                    while next_id in used_ids: next_id += 1

                    print(f"[ENROLLMENT] Providing Arduino with new Finger ID: {next_id}")
                    ser.write(f"{next_id}\n".encode())
                    ser.flush()

                elif line.startswith("NEW_ENROLL"):
                    parts = line.split(',')
                    if len(parts) >= 3:
                        new_finger = parts[1]
                        new_uid = parts[2]
                        print(f"[ENROLLMENT] Hardware success! Finger: {new_finger}, UID: {new_uid}")

                        new_student = {"name": "New Student", "id": "TBD", "uid": new_uid, "finger": new_finger,
                                       "email": ""}
                        authorized_students.append(new_student)
                        with open(DB_CACHE, 'w') as f: json.dump(authorized_students, f)

                        threading.Thread(target=sync_enrollment_task, args=(new_finger, new_uid), daemon=True).start()

                elif line.startswith("CHECK_UID") or line.startswith("GET_NAME_BY_FINGER"):
                    parts = line.split(',')
                    cmd_type = parts[0]
                    search_val = parts[1].upper()
                    match = next((s for s in authorized_students if str(s.get('uid', '')).upper() == search_val
                                  or str(s.get('finger', '')) == search_val), None)
                    if match:
                        ser.write(
                            f"FOUND,{match.get('name', 'Unknown')},{match.get('finger', '0')},{match.get('id', 'N/A')}\n".encode())
                    else:
                        ser.write(b"NOT_FOUND\n")

                        if cmd_type == "GET_NAME_BY_FINGER":
                            print(
                                f"[SYSTEM] Orphaned Finger ID {search_val} detected on hardware! Queuing auto-cleanup.")
                            hardware_delete_queue.put(search_val)
                    # --- FIXED: Guarantee immediate send ---
                    ser.flush()

                elif line.startswith("CLOUD_LOG"):
                    parts = line.split(',')
                    name = parts[1]
                    student_id = parts[2]
                    mode = parts[3]

                    if student_id not in local_scan_tracker:
                        is_late = (now.hour > int(config['lateH']) or (
                                now.hour == int(config['lateH']) and now.minute >= int(config['lateM'])))
                        in_status = "NO ID" if mode == "NO ID" else ("LATE" if is_late else "PRESENT")

                        local_scan_tracker[student_id] = {
                            "name": name, "date": today_date, "time_in": now,
                            "time_out": None, "in_status": in_status,
                            "out_status": "---", "duration": "---"
                        }
                        print(f"[INFO] Time-In recorded locally for {name}")

                    elif local_scan_tracker[student_id]["time_out"] is None:
                        t_in = local_scan_tracker[student_id]["time_in"]
                        diff_sec = (now - t_in).total_seconds()
                        hrs, mins = int(diff_sec // 3600), int((diff_sec % 3600) // 60)

                        in_stat = local_scan_tracker[student_id]["in_status"]
                        out_stat = "ID TAKEN" if mode == "NO ID" else (
                            "ID RETRIEVED" if in_stat == "NO ID" else "RETURNING")

                        local_scan_tracker[student_id]["time_out"] = now
                        local_scan_tracker[student_id]["duration"] = f"{hrs}h {mins}m"
                        local_scan_tracker[student_id]["out_status"] = out_stat
                        print(f"[INFO] Time-Out recorded locally for {name}")

                    else:
                        print(f"[INFO] DUPLICATE Scan detected: {name}. Rejected.")
                        ser.write(b"ALREADY_DONE\n")
                        ser.flush()
                        continue

                    ser.write(b"LOG_SUCCESS\n")
                    ser.flush()

                    log_queue.put(
                        {"name": name, "num": student_id, "mode": mode, "timestamp": int(now.timestamp() * 1000)})
                    save_offline_logs(list(log_queue.queue))
                    update_text_backup()

            time.sleep(0.01)

    except Exception as e:
        print(f"\n[CRITICAL] Error: {e}")
        time.sleep(5)


if __name__ == "__main__":
    start_bridge()