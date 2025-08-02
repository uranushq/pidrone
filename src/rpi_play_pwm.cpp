#include <pigpio.h>
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <map>
#include <cmath>

const int PWM_GPIO = 18;
const int TOL = 10;
const int COOLDOWN_MS = 5000;

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;

pid_t childPid = -1;
std::map<uint32_t, std::string> pwmPlaylist;

void cleanup(int code) {
    if (childPid > 0) {
        std::cerr << "[CLEANUP] Killing child process (rpi_play), pid=" << childPid << "\n";
        kill(childPid, SIGTERM);
        waitpid(childPid, nullptr, 0);  // 자식 프로세스 종료 대기
    }
    gpioTerminate();
    std::cerr << "[EXIT] GPIO cleaned up.\n";
    exit(code);
}

bool loadPlaylist(const std::string& playlistPath) {
    std::ifstream file(playlistPath);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Cannot open playlist file: " << playlistPath << "\n";
        return false;
    }
    
    nlohmann::json j;
    try {
        file >> j;
        pwmPlaylist.clear();
        
        for (auto& [key, value] : j.items()) {
            uint32_t pwmValue = std::stoul(key);
            std::string filename = value["filename"];
            pwmPlaylist[pwmValue] = filename;
            std::cout << "[PLAYLIST] PWM " << pwmValue << " -> " << filename << "\n";
        }
        
        std::cout << "[PLAYLIST] Loaded " << pwmPlaylist.size() << " entries\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse playlist JSON: " << e.what() << "\n";
        return false;
    }
}

uint32_t findMatchingPWM(uint32_t targetPWM) {
    if (pwmPlaylist.empty()) {
        return 0;
    }
    
    const uint32_t TOLERANCE = 25;  // ±25 tolerance
    
    for (const auto& [pwmValue, filename] : pwmPlaylist) {
        uint32_t diff = std::abs(static_cast<int32_t>(targetPWM - pwmValue));
        if (diff <= TOLERANCE) {
            return pwmValue;  // Found a match within tolerance
        }
    }
    
    return 0;  // No match found within tolerance
}

void signalHandler(int sig) {
    std::cerr << "[SIGNAL] Terminated by signal " << sig << "\n";
    cleanup(0);
}

void pwmCallback(int gpio, int level, uint32_t tick) {
    if (level == 1) {
        lastTick = tick;
    } else if (level == 0) {
        uint32_t pw = tick - lastTick;
        auto now = std::chrono::steady_clock::now();
        int diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COOLDOWN_MS) {
            return;
        }

        // Round PWM value to nearest 50 (0-2000 range, 50 step intervals)
        uint32_t roundedPW = ((pw + 25) / 50) * 50;
        if (roundedPW > 2000) roundedPW = 2000;
        
        // std::cout << "[PWM] Measured: " << pw << "us, Rounded: " << roundedPW << "us\n";
        
        // Find matching PWM value in playlist (within ±25 tolerance)
        uint32_t matchingPWM = findMatchingPWM(pw);  // Use original pw, not rounded
        if (matchingPWM == 0) {
            // std::cout << "[IGNORE] PWM " << pw << "us not found in playlist (no entry within ±25 tolerance)\n";
            return;
        }
        
        std::string targetFilename = pwmPlaylist[matchingPWM];
        std::cout << "[MATCH] PWM " << pw << "us -> " << matchingPWM << " -> " << targetFilename << "\n";
        
        std::string binFilePath = "./src/bin_files/" + targetFilename;
        
        lastCommandTime = now;

        if (childPid > 0) {
            std::cerr << "[INFO] Killing existing rpi_play (pid=" << childPid << ")\n";
            kill(childPid, SIGTERM);
            waitpid(childPid, nullptr, 0);
        }

        std::cout << "[TRIGGER] Executing rpi_play with " << binFilePath << "...\n";

        childPid = fork();
        if (childPid == 0) {
            // 자식 프로세스: rpi_play 실행
            execlp("sudo", "sudo", "chrt", "-f", "98",
                   "./build/rpi_play",
                   binFilePath.c_str(),
                   (char*)nullptr);
            std::cerr << "[ERROR] Failed to exec rpi_play\n";
            exit(1);
        } else if (childPid < 0) {
            std::cerr << "[ERROR] fork failed\n";
        }
    }
}

int main(int argc, char* argv[]) {
    std::set_terminate([]() { cleanup(1); });
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <playlist_json_path>\n";
        return 1;
    }

    std::string playlistPath = argv[1];

    // Load playlist from JSON file
    if (!loadPlaylist(playlistPath)) {
        std::cerr << "[ERROR] Failed to load playlist\n";
        return 1;
    }

    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] pigpio init failed.\n";
        return 1;
    }

    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);

    std::cout << "[READY] Waiting for PWM trigger on GPIO " << PWM_GPIO << "...\n";
    std::cout << "[INFO] Loaded " << pwmPlaylist.size() << " playlist entries\n";
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cleanup(0);
    return 0;
}
