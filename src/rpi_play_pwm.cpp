#include <pigpio.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <time.h>
#include <sstream>
#include <nlohmann/json.hpp>
#include "./lib/PCA9635_RPI.h"

PCA9635 pca1(0x40);
PCA9635 pca2(0x41);
PCA9635 pca3(0x42);

std::map<std::string, std::vector<std::vector<uint8_t>>> binDataMap;
std::chrono::steady_clock::time_point lastCommandTime = std::chrono::steady_clock::now();
const int COMMAND_COOLDOWN_MS = 1000; 

bool running = true;
bool isPlaying = false;
int fileIndex = 0;
int frameIndex = 0;
uint32_t lastTick = 0;
int dronePixel = 4; 
int frameSize = 48;
const int PWM_GPIO = 18;

const uint32_t PW_A = 900; 
const uint32_t PW_B = 1000; 
const uint32_t PW_C = 1100; 
const int TOL = 10;

std::vector<std::string> fileLists;
std::string currentFilename;
std::vector<std::vector<uint8_t>> currentFrames;

const std::string SAVE_DIR = "./src/bin_files/";

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
        case 4: return {{&pca1, 12}, {&pca1, 13}, {&pca1, 14}};
        case 5: return {{&pca1, 15}, {&pca2, 0}, {&pca2, 1}};
        case 6: return {{&pca2, 2}, {&pca2, 3}, {&pca2, 4}};
        case 7: return {{&pca2, 5}, {&pca2, 6}, {&pca2, 7}};
        case 8: return {{&pca2, 8}, {&pca2, 9}, {&pca2, 10}};
        case 9: return {{&pca2, 11}, {&pca2, 12}, {&pca2, 13}};
        case 10:return {{&pca2, 14}, {&pca2, 15}, {&pca3, 0}};
        case 11:return {{&pca3, 1}, {&pca3, 2}, {&pca3, 3}};
        case 12:return {{&pca3, 4}, {&pca3, 5}, {&pca3, 6}};
        case 13:return {{&pca3, 7}, {&pca3, 8}, {&pca3, 9}};
        case 14:return {{&pca3, 10}, {&pca3, 11}, {&pca3, 12}};
        case 15:return {{&pca3, 13}, {&pca3, 14}, {&pca3, 15}};
        default:return {{nullptr, -1}, {nullptr, -1}, {nullptr, -1}};
    }
}

void setLED(int ledIndex, uint8_t r, uint8_t g, uint8_t b) {
    constexpr int Rmax = 180; 
    constexpr int Gmax = 250; 
    constexpr int Bmax = 140;

    auto scale = [](uint8_t in, int max) -> uint8_t {
        int result = static_cast<int>((static_cast<double>(in) / 255.0) * max);
        return static_cast<uint8_t>(std::clamp(result, 0, 255));
    };

	RGBChannel ch = getLEDChannel(ledIndex);
    
    uint8_t r_fixed = scale(r, Rmax);
    uint8_t g_fixed = scale(g, Gmax);
    uint8_t b_fixed = scale(b, Bmax);
    
    if (ch.r.pca && ch.r.ch >= 0) ch.r.pca->analogWrite(ch.r.ch, r_fixed);
    if (ch.g.pca && ch.g.ch >= 0) ch.g.pca->analogWrite(ch.g.ch, g_fixed);
    if (ch.b.pca && ch.b.ch >= 0) ch.b.pca->analogWrite(ch.b.ch, b_fixed);
}

void handleSignal(int s) {
    running = false;
}

void loadCurrentFile() {
    if (fileIndex >= fileLists.size()) return;
    currentFilename = fileLists[fileIndex];
    if (binDataMap.find(currentFilename) == binDataMap.end()) {
        std::ifstream bin(SAVE_DIR + currentFilename, std::ios::binary);
        if (!bin) return;
        bin.seekg(32, std::ios::beg);
        std::vector<std::vector<uint8_t>> frames;
        while (true) {
            std::vector<uint8_t> frame(frameSize);
            bin.read(reinterpret_cast<char*>(frame.data()), frameSize);
            if (bin.gcount() != frameSize) break;
            frames.push_back(frame);
        }
        binDataMap[currentFilename] = std::move(frames);
    }
    currentFrames = binDataMap[currentFilename];
}

void pwmCallback(int gpio, int level, uint32_t tick) {
    if (level == 1) {
        lastTick = tick;
    } else if (level == 0) {
        uint32_t pw = tick - lastTick;
        
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COMMAND_COOLDOWN_MS) return;
        lastCommandTime = now;

        if (PW_A - TOL <= pw && pw <= PW_A + TOL) {
            fileIndex = (fileIndex + 1) % fileLists.size();
            frameIndex = 0;
            isPlaying = false;
            loadCurrentFile();
            std::cout << "[NEXT] fileIndex=" << fileIndex << std::endl;
        } else if (PW_B - TOL <= pw && pw <= PW_B + TOL) {
            isPlaying = true;
            std::cout << "[PLAY]" << std::endl;
        } else if (PW_C - TOL <= pw && pw <= PW_C + TOL) {
            isPlaying = false;
            std::cout << "[PAUSE]" << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: ./rpi_play_pwm <playlist.json> <pixel_size> \n";
        return 1;
    }

    std::string scheduleName = argv[1];
    dronePixel = std::stoi(argv[2]);
    frameSize = dronePixel * dronePixel * 3;

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] pigpio init failed" << std::endl;
        return 1;
    }
    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);

    if (!pca1.begin() || !pca2.begin() || !pca3.begin()) {
        std::cerr << "[ERROR] PCA9635 init failed" << std::endl;
        return 1;
    }

    // Load file list
    std::ifstream f(scheduleName);
    nlohmann::json j;
    f >> j;
    for (auto& item : j) fileLists.push_back(item["filename"]);
    
    loadCurrentFile();
    
    const int interval_us = 30'000;
    struct timespec nextFrameTime;
    clock_gettime(CLOCK_MONOTONIC, &nextFrameTime);
    
    while (running) {
        if (isPlaying && !currentFrames.empty()) {
            for (; frameIndex < currentFrames.size(); ++frameIndex) {
                if (!isPlaying) break;
                
                auto& frame = currentFrames[frameIndex];
                for (int i = 0; i < dronePixel * dronePixel; ++i) {
                    setLED(i, frame[i*3+0], frame[i*3+1], frame[i*3+2]);
                }

                nextFrameTime.tv_nsec += interval_us * 1000;
                if (nextFrameTime.tv_nsec >= 1000000000) {
                    nextFrameTime.tv_sec += 1;
                    nextFrameTime.tv_nsec -= 1000000000;
                }

                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextFrameTime, nullptr);
            }

            if (frameIndex >= currentFrames.size()) {
                frameIndex = 0;
                isPlaying = false;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    for (int i = 0; i < 16; ++i) setLED(i, 0, 0, 0);
    gpioTerminate();
    return 0;
}

