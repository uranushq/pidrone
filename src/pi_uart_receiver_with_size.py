import serial
import os
import json
import subprocess
import threading
import struct 

from datetime import datetime

# === Configuration ===
SERIAL_PORT = '/dev/ttyS0'  # UART port (adjust as needed) # 컨테이너 실행 시 --device=/dev/ttyS0 필요
BAUD_RATE = 115200

# Dockerfile 또는 docker-compose에서 이 경로들을 컨테이너 내부에 바인딩
# SAVE_DIR = "/home/pi/PCA9635/bin_files"
# LOG_FILE = "/home/pi/PCA9635/transfer_log.txt"
# WORK_DIR = "/home/pi/PCA9635/"
# JSON_DIR = "/home/pi/PCA9635/jsonFile/"

# 컨테이너 내부 기준 경로
SAVE_DIR = "./bin_files"
LOG_FILE = "./transfer_log.txt"
WORK_DIR = "./"  # rpi_play, rgb_test 등의 실행 파일 위치
JSON_DIR = "./jsonFile"

# Ensure save directory exists
os.makedirs(SAVE_DIR, exist_ok=True)
os.makedirs(JSON_DIR, exist_ok=True)

# Open UART serial connection
ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
ser.reset_input_buffer()  # Clear any leftover junk from ESP32
print("[Pi] UART handler started. Waiting for commands...")

# Write a line to the log file with timestamp
def write_log(entry: str):
    with open(LOG_FILE, "a") as logf:
        logf.write(entry + "\n")
        
        
def run_playlist_from_json(list_name: str, playlist_json :str ):
    try:
        # 파일 저장 
        json_path = os.path.join(JSON_DIR, f"{list_name}_play.json")
        with open(json_path, "w") as f:
            f.write(playlist_json + "\n")

        print(f"[Pi] Playlist saved to {json_path}")

        # 실행 명령어 호출 
        cmd = ["./rpi_play", f"{JSON_DIR}{list_name}_play.json", "4"]
        print("[Pi] Executing:", " ".join(cmd))
        subprocess.run(cmd, cwd=WORK_DIR)

    except Exception as e:
        print("[Pi] Failed to run playlist:", e)

# Receive and store file sent from ESP32 over UART
def receive_file_with_size(filename, file_size):
    filepath = os.path.join(SAVE_DIR, filename)

    total_bytes = 0
    with open(filepath, "wb") as f:
        while total_bytes < file_size:
            chunk = ser.read(min(1024, file_size - total_bytes))
            if not chunk:
                break
            f.write(chunk)
            total_bytes += len(chunk)

    if total_bytes == file_size:
        print(f"[Pi] File received: {filename} ({total_bytes} bytes)")
        write_log(f"[{datetime.now()}] Received: {filename}, size: {total_bytes} bytes")
    else:
        print(f"[Pi] Incomplete file: {filename}")
        write_log(f"[{datetime.now()}] Incomplete: {filename}, only {total_bytes}/{file_size} bytes")

    ack_message = f"ACK:{filename}"
    ser.write(f"{ack_message}\n".encode())
    print(f"[Pi] Sent ACK for: {filename}")
    write_log(f"[{datetime.now()}] Sent ACK: {ack_message}")

# Delete a specified file and log the action
def delete_file(filename):
    filepath = os.path.join(SAVE_DIR, filename)
    if os.path.exists(filepath):
        os.remove(filepath)
        print(f"[Pi] Deleted file: {filename}")
        write_log(f"[{datetime.now()}] Deleted: {filename}")
    else:
        print(f"[Pi] File not found: {filename}")
        write_log(f"[{datetime.now()}] Delete failed (not found): {filename}")
        
# === ADD THIS FUNCTION HERE ===
def run_led_async(filename):
    def worker():
        try:
            subprocess.run(["./test_files/rpi_test", filename, "4"], cwd=WORK_DIR)
        except Exception as e:
            print("[Pi] LED error:", e)
    threading.Thread(target=worker, daemon=True).start()

# Send list of stored .bin files to ESP32
def list_files():
    try:
        files = [f for f in os.listdir(SAVE_DIR) if f.endswith(".bin")]
        response = json.dumps(files)
        ser.write(f"{response}\n".encode())
        print("[Pi] Sent file list to ESP32.")
    except Exception as e:
        error_msg = f"ERROR: {str(e)}"
        ser.write(f"{error_msg}\n".encode())
        print("[Pi] Failed to send file list:", error_msg)

# === Main UART listening loop ===
while True:
    try:
        line = ser.readline()
        decoded = line.decode(errors='ignore').strip()

        if decoded == "":
            continue

        elif decoded == "PING":
            print("[Pi] Received PING")
            ser.write(b"I_AM_ALIVE\n")

        elif decoded.startswith("UPLOAD:"):
            parts = decoded.split(":")
            if len(parts) >= 3:
                filename = parts[1].strip()
                file_size = int(parts[2].strip())
                print(f"[Pi] Receiving file: {filename} (expected size: {file_size})")
                receive_file_with_size(filename, file_size)
            else :
                print("[Pi] Invalid UPLOAD header format ")

        elif decoded.startswith("DELETE:"):
            filename = decoded.split("DELETE:")[1].strip()
            print(f"[Pi] Delete command received: {filename}")
            delete_file(filename)

        elif decoded == "LIST_FILES":
            print("[Pi] File list requested from ESP32.")
            list_files()
            
        elif decoded.startswith("RUN_LED:"):
            filename = decoded.split("RUN_LED:")[1].strip()
            print(f"[Pi] RUN_LED received: {filename}")
            run_led_async(filename)  # Run LED without blocking PING
            
        elif decoded.startswith("RUN_RGB:"):
            try:
                g, r, b = map(int, decoded.split("RUN_RGB:")[1].strip().split(","))
                print(f"[Pi] RUN_RGB received: G={r}, R={g}, B={b}")
                
                subprocess.run(["./rgb_test", str(r), str(g), str(b)], cwd=WORK_DIR)
            except Exception as e:
                print("[Pi] Failed to run RGB test:", e)
        
        elif decoded.startswith("RUN_PLAYLIST:"):
            list_name = decoded[len("RUN_PLAYLIST:"):].strip()
            print(f"[Pi] RUN_PLAYLIST received for: {list_name}")

            if ser.in_waiting:
                playlist_line = ser.readline().decode(errors='ignore').strip()
                run_playlist_from_json(list_name, playlist_line)
            else:
                print("[Pi] No playlist JSON received after RUN_PLAYLIST.")
        
        elif decoded.startswith("PASS:"):
            pass

        else:
            print("[Pi] Ignored:", decoded)

    except Exception as e:
        print("[Pi] Error in main loop:", e)
