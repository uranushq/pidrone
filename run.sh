#!/bin/bash
set -e

echo "[*] run.sh 시작됨"

echo "[*] ./build.sh 실행..."
./build.sh

# pigpiod가 실행 중이면 종료
if pgrep pigpiod > /dev/null; then
    echo "[*] 기존 pigpiod 프로세스 종료 중..."
    sudo killall pigpiod || true
    sudo rm -f /var/run/pigpio.pid
    sleep 1
fi

# 종료 시 cleanup 함수 실행
cleanup() {
    echo "[!] SIGTERM 또는 종료 감지됨. 정리 중..."
    sudo killall pigpiod || true
    sudo rm -f /var/run/pigpio.pid
    echo "[*] 정리 완료. 종료합니다."
    exit 0
}
trap cleanup SIGINT SIGTERM SIGQUIT

# UART 수신기 백그라운드 실행
echo "[+] UART 수신기 실행 중..."
python3 ./src/pi_uart_receiver_with_size.py &  # 경로 수정 필요시 조정

# 예제 실행
echo "[+] rpi_play_pwm 실행 중..."
sudo chrt -f 99 ./build/rpi_play_pwm ./src/jsonFile/playlist.json 4

# 백그라운드 프로세스 대기
wait
