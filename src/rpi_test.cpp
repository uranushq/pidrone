#include <iostream>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <chrono>
#include "./lib/PCA9635_RPI.h"

// Initialize PCA9635 boards with I2C addresses
PCA9635 pca1(0x40);
PCA9635 pca2(0x41);
PCA9635 pca3(0x42);
PCA9635 pca4(0x44);

// Define LED control structures
struct Channel {
    PCA9635* pca;
    int ch;
};

struct RGBChannel {
    Channel r, g, b;
};

// Map LED index (0~15) to the corresponding RGB PCA9635 channels
RGBChannel getLEDChannel(int ledIndex) {
    switch (ledIndex) {
        case 0: return {{&pca1, 0}, {&pca1, 1}, {&pca1, 2}};
        case 1: return {{&pca1, 3}, {&pca1, 4}, {&pca1, 5}};
        case 2: return {{&pca1, 6}, {&pca1, 7}, {&pca1, 8}};
        case 3: return {{&pca1, 9}, {&pca1, 10}, {&pca1, 11}};
        case 4: return {{&pca2, 0}, {&pca2, 1}, {&pca2, 2}};
        case 5: return {{&pca2, 3}, {&pca2, 4}, {&pca2, 5}};
        case 6: return {{&pca2, 6}, {&pca2, 7}, {&pca2, 8}};
        case 7: return {{&pca2, 9}, {&pca2, 10}, {&pca2, 11}};

        case 8: return {{&pca3, 0}, {&pca3, 1}, {&pca3, 2}};
        case 9: return {{&pca3, 3}, {&pca3, 4}, {&pca3, 5}};
        case 10: return {{&pca3, 6}, {&pca3, 7}, {&pca3, 8}};
        case 11: return {{&pca3, 9}, {&pca3, 10}, {&pca3, 11}};
        case 12: return {{&pca4, 0}, {&pca4, 1}, {&pca4, 2}};
        case 13: return {{&pca4, 3}, {&pca4, 4}, {&pca4, 5}};
        case 14: return {{&pca4, 6}, {&pca4, 7}, {&pca4, 8}};
        case 15: return {{&pca4, 9}, {&pca4, 10}, {&pca4, 11}};

        default: return {{nullptr, -1}, {nullptr, -1}, {nullptr, -1}};
    }
}

// Set the color of a specific LED (by index)
void setLED(int ledIndex, uint8_t r, uint8_t g, uint8_t b) {
    RGBChannel ch = getLEDChannel(ledIndex);
    int r_fixed = r * 2/3;
    int b_fixed = b * 2/3;
    if (ch.r.pca && ch.r.ch >= 0) ch.r.pca->analogWrite(ch.r.ch, r_fixed);
    if (ch.g.pca && ch.g.ch >= 0) ch.g.pca->analogWrite(ch.g.ch, g);
    if (ch.b.pca && ch.b.ch >= 0) ch.b.pca->analogWrite(ch.b.ch, b_fixed);
}

// Set all LEDs to the same color
void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < 16; ++i) {
        setLED(i, r, g, b);
    }
}

// Handle SIGINT and SIGTERM: turn off all LEDs before exit
void handleExit(int signum) {
    std::cout << "\n[종료] 모든 LED를 끕니다..." << std::endl;
    setAllLEDs(0, 0, 0);
    exit(0);
}

int main(int argc, char* argv[]) {
    // Check if enough arguments are provided
    if (argc != 4) {
        std::cerr << "사용법: " << argv[0] << " <r_value> <g_value> <b_value>" << std::endl;
        std::cerr << "예시: " << argv[0] << " 255 0 0 (빨간색)" << std::endl;
        std::cerr << "RGB 값은 0-255 범위입니다." << std::endl;
        return 1;
    }

    // Parse RGB values
    int r_val = std::atoi(argv[1]);
    int g_val = std::atoi(argv[2]);
    int b_val = std::atoi(argv[3]);

    // Validate RGB values
    if (r_val < 0 || r_val > 255 || g_val < 0 || g_val > 255 || b_val < 0 || b_val > 255) {
        std::cerr << "오류: RGB 값은 0-255 범위여야 합니다." << std::endl;
        return 1;
    }

    uint8_t r = static_cast<uint8_t>(r_val);
    uint8_t g = static_cast<uint8_t>(g_val);
    uint8_t b = static_cast<uint8_t>(b_val);

    std::cout << "RGB 값 설정: R=" << static_cast<int>(r) 
              << ", G=" << static_cast<int>(g) 
              << ", B=" << static_cast<int>(b) << std::endl;

    // Register signal handlers for safe exit
    signal(SIGINT, handleExit);
    signal(SIGTERM, handleExit);

    // Initialize all PCA9635 boards
    bool pca_initialized = pca1.begin() && pca2.begin() && pca3.begin() && pca4.begin();
    if (!pca_initialized) {
        std::cerr << "오류: PCA9635 보드 초기화에 실패했습니다." << std::endl;
        return 1;
    }

    std::cout << "PCA9635 보드가 성공적으로 초기화되었습니다." << std::endl;
    std::cout << "모든 LED에 색상을 설정합니다... (Ctrl+C로 종료)" << std::endl;

    // Set all LEDs to the specified color and keep them on
    setAllLEDs(r, g, b);

    // Keep the program running until interrupted
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}