#include <pigpio.h>
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <map>
#include <cmath>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <atomic>
#include <spawn.h>
#include <sched.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <pthread.h>

bool pin_to_cpu3(const char* tag) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(3, &set); // CPU #3

    // 프로세스(현재 스레드)에 affinity 설정
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::cerr << "[WARN] " << tag << ": sched_setaffinity failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

const int PWM_GPIO = 18;
const int TOL = 10;
const int COOLDOWN_MS = 5000;  // 기존 쿨다운으로 복원
static const char* SOCK_PATH = "/tmp/rpi_playerd.sock";

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;

std::map<uint32_t, std::string> pwmPlaylist;
std::atomic<bool> daemonRunning{true};
std::atomic<bool> isPlaying{false};  // 재생 중인지 추적

// Command queue for async processing
struct PlayCommand {
    std::string binFilePath;
    std::chrono::steady_clock::time_point timestamp;
};

std::queue<PlayCommand> commandQueue;
std::mutex queueMutex;
std::condition_variable queueCv;

extern char **environ;

void cleanup(int code) {
    daemonRunning = false;
    
    gpioTerminate();
    std::cerr << "[EXIT] GPIO cleaned up.\n";
    exit(code);
}

// Spawn rpi_play with optimized parameters
int spawnRpiPlay(const std::string& binFilePath) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    
    // Set real-time priority
    sched_param sp = {.sched_priority = 98};
    posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
    posix_spawnattr_setschedparam(&attr, &sp);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDULER);
    
    const char* argv[] = {"./build/rpi_play", binFilePath.c_str(), nullptr};
    pid_t pid;
    int rc = posix_spawn(&pid, "./build/rpi_play", nullptr, &attr, (char* const*)argv, environ);
    posix_spawnattr_destroy(&attr);
    
    if (rc == 0) {
        std::cout << "[SPAWN] rpi_play started with pid=" << pid << std::endl;
        return pid;
    } else {
        std::cerr << "[ERROR] posix_spawn failed: " << strerror(rc) << std::endl;
        return -1;
    }
}

// File cache warming
void warmFileCache(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return;
    
    std::vector<char> buffer(1024 * 1024); // 1MB buffer
    while (file.read(buffer.data(), buffer.size())) {
        // Just read to warm the cache
    }
    std::cout << "[CACHE_WARM] Warmed cache for " << filePath << std::endl;
}

// Daemon worker thread - handles play commands
void daemonWorker() {
    pin_to_cpu3("daemonWorker");
    // Set highest priority for real-time scheduling
    struct sched_param param;
    param.sched_priority = 99;  // Highest priority
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        std::cerr << "[WARNING] Failed to set high priority scheduling for daemon worker\n";
    } else {
        std::cout << "[SCHED] Daemon worker set to SCHED_FIFO priority 99\n";
    }
    
    pid_t currentChild = -1;
    std::cout << "[DAEMON_WORKER] Started\n";
    
    while (daemonRunning) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [] { return !commandQueue.empty() || !daemonRunning; });
        
        if (!daemonRunning) break;
        
        if (!commandQueue.empty()) {
            PlayCommand cmd = commandQueue.front();
            commandQueue.pop();
            lock.unlock();
            
            if (cmd.binFilePath == "STOP") {
                // Stop command received
                if (currentChild > 0) {
                    std::cout << "[DAEMON] Stopping current playbook (pid=" << currentChild << ")\n";
                    kill(currentChild, SIGTERM);
                    waitpid(currentChild, nullptr, 0);
                    currentChild = -1;
                }
                isPlaying = false;
                std::cout << "[DAEMON] Playback stopped\n";
                continue;
            }
            
            // Kill existing child if running
            if (currentChild > 0) {
                std::cout << "[DAEMON] Stopping current playback (pid=" << currentChild << ")\n";
                kill(currentChild, SIGTERM);
                waitpid(currentChild, nullptr, 0);
                currentChild = -1;
            }
            
            // Warm cache for the file
            warmFileCache(cmd.binFilePath);
            
            // Set playing state
            isPlaying = true;
            
            // Spawn new rpi_play
            currentChild = spawnRpiPlay(cmd.binFilePath);
            
            if (currentChild > 0) {
                std::cout << "[DAEMON] Playing " << cmd.binFilePath << " (pid=" << currentChild << ")\n";
                
                // Wait for completion in non-blocking way
                std::thread([&currentChild]() {
                    int status;
                    waitpid(currentChild, &status, 0);
                    currentChild = -1;
                    isPlaying = false;  // 재생 완료시 상태 업데이트
                    std::cout << "[DAEMON] Playback finished\n";
                }).detach();
            } else {
                isPlaying = false;  // 실행 실패시 상태 리셋
            }
        }
    }
    
    // Cleanup on exit
    if (currentChild > 0) {
        kill(currentChild, SIGTERM);
        waitpid(currentChild, nullptr, 0);
    }
    
    std::cout << "[DAEMON_WORKER] Stopped\n";
}

// Daemon socket server
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
        
        // Ignore PWM values 50us or below (noise filtering)
        if (pw <= 50) {
            return;
        }
        
        // Log all incoming PWM values
        std::cout << "[PWM_IN] Raw PWM: " << pw << "us" << std::endl;
        
        auto now = std::chrono::steady_clock::now();
        int diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COOLDOWN_MS) {
            std::cout << "[COOLDOWN] Ignoring PWM " << pw << "us (cooldown: " << diff << "ms)" << std::endl;
            return;
        }

        // Check for stop signal (3000us PWM) - 항상 처리
        if (std::abs(static_cast<int32_t>(pw - 3000)) <= 25) {
            std::cout << "[STOP] PWM 3000us received - stopping playback\n";
            
            // Send stop command to daemon
            PlayCommand stopCmd;
            stopCmd.binFilePath = "STOP";
            stopCmd.timestamp = now;
            
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                commandQueue.push(stopCmd);
            }
            queueCv.notify_one();
            
            lastCommandTime = now;
            return;
        }
        
        // 재생 중이면 다른 모든 신호 무시
        if (isPlaying) {
            std::cout << "[PLAYING] Ignoring PWM " << pw << "us (playback in progress, only stop signal accepted)" << std::endl;
            return;
        }

        // Find matching PWM value in playlist
        uint32_t matchingPWM = findMatchingPWM(pw);
        if (matchingPWM == 0) {
            std::cout << "[NO_MATCH] PWM " << pw << "us not found in playlist" << std::endl;
            return;
        }
        std::cout << "[MATCH_FOUND] Raw PWM " << pw << "us -> Playlist PWM " << matchingPWM << "us" << std::endl;
        
        // Send play command to daemon
        std::string filename = pwmPlaylist[matchingPWM];
        std::string binFilePath = "./src/bin_files/" + filename;
        
        PlayCommand playCmd;
        playCmd.binFilePath = binFilePath;
        playCmd.timestamp = now;
        
        std::cout << "[DAEMON_CMD] Sending play command: " << filename << std::endl;
        
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(playCmd);
        }
        queueCv.notify_one();
        
        lastCommandTime = now;
        std::cout << "[SUCCESS] Command sent successfully" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Set highest priority for main process (PWM signal processing)
    struct sched_param param;
    param.sched_priority = 99;  // Highest priority for PWM processing

    pin_to_cpu3("main");
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        std::cerr << "[WARNING] Failed to set SCHED_FIFO 99 for main\n";
    } else {
        std::cout << "[SCHED] Daemon worker set to SCHED_FIFO priority 99\n";
    }
    
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

    // Warm cache for all playlist files
    std::cout << "[CACHE_WARM] Warming cache for playlist files...\n";
    for (const auto& [pwm, filename] : pwmPlaylist) {
        std::string binFilePath = "./src/bin_files/" + filename;
        warmFileCache(binFilePath);
    }
    
    // Warm the rpi_play binary itself
    warmFileCache("./build/rpi_play");

    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] pigpio init failed.\n";
        return 1;
    }

    // Start daemon threads
    std::cout << "[DAEMON] Starting daemon thread...\n";
    std::thread workerThread(daemonWorker);
    
    // Wait a bit for daemon to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);

    std::cout << "[READY] PWM controller ready on GPIO " << PWM_GPIO << std::endl;
    std::cout << "[INFO] Loaded " << pwmPlaylist.size() << " playlist entries" << std::endl;
    
    // Main loop - just keep alive and handle signals
    while (daemonRunning) {
        struct timespec sleepTime = {1, 0}; // 1초씩 sleep
        nanosleep(&sleepTime, nullptr);
    }

    // Cleanup
    std::cout << "[SHUTDOWN] Shutting down daemon thread...\n";
    
    daemonRunning = false;
    queueCv.notify_all();
    
    if (workerThread.joinable()) {
        workerThread.join();
    }

    cleanup(0);
    return 0;
}
