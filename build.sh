#!/bin/bash

set -e  # 실패 시 종료

echo "[*] 빌드 시작..."

# 빌드 디렉토리 준비
mkdir -p build

# 실행 파일 빌드
echo "[*] rpi_play 빌드..."
g++ src/rpi_play.cpp src/lib/PCA9635_RPI.cpp -o build/rpi_play

echo "[*] rpi_play_pwm 빌드..."
g++ -o build/rpi_play_pwm \
    src/rpi_play_pwm.cpp src/lib/PCA9635_RPI.cpp \
    -lpigpio -lrt -lpthread

echo "[*] rpi_test 빌드..."
g++ src/rpi_test.cpp src/lib/PCA9635_RPI.cpp -o build/rpi_test

chmod +x build/rpi_play
chmod +x build/rpi_play_pwm
chmod +x build/rpi_test