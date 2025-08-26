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
#include <time.h>

const int PWM_GPIO = 18;
const int TOL = 10;
const int COOLDOWN_MS = 5000;
const int CONFIRMATION_TIMEOUT_MS = 500;  // 0.5초 내에 같은 신호가 다시 와야 함

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;

pid_t childPid = -1;
std::map<uint32_t, std::string> pwmPlaylist;

// PWM confirmation tracking
struct PWMConfirmation {
    uint32_t targetPWM;  // 플레이리스트에서 찾은 목표 PWM 값
    std::chrono::steady_clock::time_point firstDetectionTime;
    int confirmationCount;
};

std::map<uint32_t, PWMConfirmation> pendingConfirmations;

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

void cleanupExpiredConfirmations() {
    auto now = std::chrono::steady_clock::now();
    auto it = pendingConfirmations.begin();
    
    while (it != pendingConfirmations.end()) {
        auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.firstDetectionTime).count();
        if (timeDiff > CONFIRMATION_TIMEOUT_MS) {
            std::cout << "[TIMEOUT] PWM " << it->first << "us confirmation expired after " << timeDiff << "ms" << std::endl;
            it = pendingConfirmations.erase(it);
        } else {
            ++it;
        }
    }
}

bool checkPWMConfirmation(uint32_t rawPWM, uint32_t targetPWM) {
    auto now = std::chrono::steady_clock::now();
    
    cleanupExpiredConfirmations();
    
    auto it = pendingConfirmations.find(targetPWM);
    
    if (it == pendingConfirmations.end()) {
        // First detection - start waiting for confirmation
        pendingConfirmations[targetPWM] = {targetPWM, now, 1};
        std::cout << "[FIRST] Raw PWM " << rawPWM << "us -> Target " << targetPWM 
                  << "us detected, waiting exactly 500ms for confirmation..." << std::endl;
        
        // Wait exactly 500ms using nanosleep
        struct timespec waitTime = {0, 500000000}; // 0.5초
        struct timespec startTime, endTime;
        clock_gettime(CLOCK_MONOTONIC, &startTime);
        
        nanosleep(&waitTime, nullptr);
        
        clock_gettime(CLOCK_MONOTONIC, &endTime);
        long long actualWaitNs = (endTime.tv_sec - startTime.tv_sec) * 1000000000LL + 
                                (endTime.tv_nsec - startTime.tv_nsec);
        
        std::cout << "[WAIT_COMPLETE] Target PWM " << targetPWM 
                  << "us waited " << actualWaitNs << "ns (target: 500000000ns)" << std::endl;
        
        // Check if confirmation was received during wait
        auto confirmIt = pendingConfirmations.find(targetPWM);
        if (confirmIt != pendingConfirmations.end() && confirmIt->second.confirmationCount >= 2) {
            std::cout << "[CONFIRMED_AFTER_WAIT] Target PWM " << targetPWM 
                      << "us confirmed with count: " << confirmIt->second.confirmationCount << std::endl;
            pendingConfirmations.erase(confirmIt);
            return true;
        } else {
            std::cout << "[TIMEOUT_NO_CONFIRMATION] Target PWM " << targetPWM 
                      << "us no confirmation received" << std::endl;
            pendingConfirmations.erase(targetPWM);
            return false;
        }
    } else {
        // Subsequent detection within timeout
        auto timeDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.firstDetectionTime).count();
        
        if (timeDiff <= CONFIRMATION_TIMEOUT_MS) {
            it->second.confirmationCount++;
            std::cout << "[CONFIRMATION] Raw PWM " << rawPWM << "us -> Target " << targetPWM 
                      << "us count now: " << it->second.confirmationCount 
                      << ", time: " << timeDiff << "ms" << std::endl;
            return false; // Don't execute yet, let the wait complete
        } else {
            // Reset as new detection
            std::cout << "[RESET] Target PWM " << targetPWM << "us timeout, starting new wait" << std::endl;
            pendingConfirmations[targetPWM] = {targetPWM, now, 1};
            return false;
        }
    }
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
        
        // Log all incoming PWM values
        std::cout << "[PWM_IN] Raw PWM: " << pw << "us" << std::endl;
        
        auto now = std::chrono::steady_clock::now();
        int diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COOLDOWN_MS) {
            std::cout << "[COOLDOWN] Ignoring PWM " << pw << "us (cooldown: " << diff << "ms)" << std::endl;
            return;
        }

        // Find matching PWM value in playlist
        uint32_t matchingPWM = findMatchingPWM(pw);
        if (matchingPWM == 0) {
            std::cout << "[NO_MATCH] PWM " << pw << "us not found in playlist" << std::endl;
            return;
        }
        
        std::cout << "[MATCH_FOUND] Raw PWM " << pw << "us -> Playlist PWM " << matchingPWM << "us" << std::endl;
        
        // Clear any different pending PWM confirmations
        auto it = pendingConfirmations.begin();
        while (it != pendingConfirmations.end()) {
            if (it->first != matchingPWM) {
                std::cout << "[CLEAR] Clearing different pending PWM " << it->first 
                          << "us due to new target " << matchingPWM << "us" << std::endl;
                it = pendingConfirmations.erase(it);
            } else {
                ++it;
            }
        }
        
        // Check confirmation (this will handle the 0.5s wait internally)
        if (!checkPWMConfirmation(pw, matchingPWM)) {
            return; // Either waiting or failed confirmation
        }
        
        // Confirmed and ready to execute
        std::string filename = pwmPlaylist[matchingPWM];
        std::cout << "[EXECUTE] Confirmed PWM " << matchingPWM << "us -> " << filename << std::endl;
        
        std::string binFilePath = "./src/bin_files/" + filename;
        
        lastCommandTime = now;

        if (childPid > 0) {
            std::cerr << "[INFO] Killing existing rpi_play (pid=" << childPid << ")" << std::endl;
            kill(childPid, SIGTERM);
            waitpid(childPid, nullptr, 0);
        }

        std::cout << "[TRIGGER] Executing rpi_play with " << binFilePath << "..." << std::endl;

        childPid = fork();
        if (childPid == 0) {
            // 자식 프로세스: rpi_play 실행
            execlp("sudo", "sudo", "chrt", "-f", "98",
                   "./build/rpi_play",
                   binFilePath.c_str(),
                   (char*)nullptr);
            std::cerr << "[ERROR] Failed to exec rpi_play" << std::endl;
            exit(1);
        } else if (childPid < 0) {
            std::cerr << "[ERROR] fork failed" << std::endl;
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

    std::cout << "[READY] Waiting for PWM trigger on GPIO " << PWM_GPIO << "..." << std::endl;
    std::cout << "[INFO] Loaded " << pwmPlaylist.size() << " playlist entries" << std::endl;
    std::cout << "[INFO] PWM confirmation timeout: " << CONFIRMATION_TIMEOUT_MS << "ms" << std::endl;
    
    while (true) {
        struct timespec sleepTime = {1, 0}; // 1초씩 sleep
        nanosleep(&sleepTime, nullptr);
        
        // 주기적으로 expired 정리 (이제 덜 중요함)
        cleanupExpiredConfirmations();
    }

    cleanup(0);
    return 0;
}
