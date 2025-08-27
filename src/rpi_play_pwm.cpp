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
#include <sys/socket.h>
#include <sys/un.h>
#include <atomic>
#include <spawn.h>
#include <sched.h>
#include <queue>
#include <mutex>
#include <condition_variable>

const int PWM_GPIO = 18;
const int TOL = 10;
const int PWM_GPIO = 18;
static const char* SOCK_PATH = "/tmp/rpi_playerd.sock";

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;

std::map<uint32_t, std::string> pwmPlaylist;
std::atomic<bool> daemonRunning{true};

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
    
    // Close daemon socket
    if (daemonSocketFd >= 0) {
        close(daemonSocketFd);
        unlink(SOCK_PATH);
    }
    
    gpioTerminate();
    std::cerr << "[EXIT] GPIO and daemon cleaned up.\n";
    exit(code);
}

// Send command to daemon via Unix socket
bool sendCommandToDaemon(const nlohmann::json& cmd) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "[ERROR] Failed to create socket\n";
        return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to connect to daemon\n";
        close(sockfd);
        return false;
    }
    
    std::string cmdStr = cmd.dump();
    if (write(sockfd, cmdStr.c_str(), cmdStr.size()) < 0) {
        std::cerr << "[ERROR] Failed to send command\n";
        close(sockfd);
        return false;
    }
    
    // Read response (optional)
    char response[256] = {0};
    read(sockfd, response, sizeof(response) - 1);
    std::cout << "[DAEMON_RESPONSE] " << response << std::endl;
    
    close(sockfd);
    return true;
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
            
            // Kill existing child if running
            if (currentChild > 0) {
                std::cout << "[DAEMON] Stopping current playback (pid=" << currentChild << ")\n";
                kill(currentChild, SIGTERM);
                waitpid(currentChild, nullptr, 0);
                currentChild = -1;
            }
            
            // Warm cache for the file
            warmFileCache(cmd.binFilePath);
            
            // Spawn new rpi_play
            currentChild = spawnRpiPlay(cmd.binFilePath);
            
            if (currentChild > 0) {
                std::cout << "[DAEMON] Playing " << cmd.binFilePath << " (pid=" << currentChild << ")\n";
                
                // Wait for completion in non-blocking way
                std::thread([&currentChild]() {
                    int status;
                    waitpid(currentChild, &status, 0);
                    currentChild = -1;
                    std::cout << "[DAEMON] Playback finished\n";
                }).detach();
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
void daemonSocketServer() {
    unlink(SOCK_PATH);
    
    daemonSocketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (daemonSocketFd < 0) {
        std::cerr << "[ERROR] Failed to create daemon socket\n";
        return;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);
    
    if (bind(daemonSocketFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind daemon socket\n";
        close(daemonSocketFd);
        return;
    }
    
    if (listen(daemonSocketFd, 5) < 0) {
        std::cerr << "[ERROR] Failed to listen on daemon socket\n";
        close(daemonSocketFd);
        return;
    }
    
    chmod(SOCK_PATH, 0666); // Allow access
    std::cout << "[DAEMON_SOCKET] Listening on " << SOCK_PATH << std::endl;
    
    while (daemonRunning) {
        int clientFd = accept(daemonSocketFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (daemonRunning) {
                std::cerr << "[ERROR] Failed to accept connection\n";
            }
            continue;
        }
        
        std::thread([clientFd]() {
            char buffer[4096] = {0};
            ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
            
            if (bytesRead > 0) {
                try {
                    nlohmann::json cmd = nlohmann::json::parse(buffer);
                    std::string action = cmd.value("cmd", "");
                    
                    if (action == "play") {
                        std::string filePath = cmd.value("file", "");
                        if (!filePath.empty()) {
                            PlayCommand playCmd = {filePath, std::chrono::steady_clock::now()};
                            
                            {
                                std::lock_guard<std::mutex> lock(queueMutex);
                                commandQueue.push(playCmd);
                            }
                            queueCv.notify_one();
                            
                            std::string response = R"({"ok":true,"detail":"queued"})";
                            write(clientFd, response.c_str(), response.size());
                        }
                    } else if (action == "stop") {
                        // Clear queue and stop current playback
                        {
                            std::lock_guard<std::mutex> lock(queueMutex);
                            std::queue<PlayCommand> empty;
                            commandQueue.swap(empty);
                        }
                        
                        std::string response = R"({"ok":true,"detail":"stopped"})";
                        write(clientFd, response.c_str(), response.size());
                    }
                } catch (const std::exception& e) {
                    std::string response = R"({"ok":false,"error":"parse_error"})";
                    write(clientFd, response.c_str(), response.size());
                }
            }
            
            close(clientFd);
        }).detach();
    }
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

        // Check for stop signal (3000us PWM)
        if (std::abs(static_cast<int32_t>(pw - 3000)) <= 25) {
            std::cout << "[STOP] PWM 3000us received - stopping playback\n";
            nlohmann::json stopCmd = {{"cmd", "stop"}};
            sendCommandToDaemon(stopCmd);
            lastCommandTime = now;
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
        
        nlohmann::json playCmd = {
            {"cmd", "play"},
            {"file", binFilePath}
        };
        
        std::cout << "[DAEMON_CMD] Sending play command: " << filename << std::endl;
        if (sendCommandToDaemon(playCmd)) {
            lastCommandTime = now;
            std::cout << "[SUCCESS] Command sent successfully" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to send command to daemon" << std::endl;
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
    std::cout << "[DAEMON] Starting daemon threads...\n";
    std::thread workerThread(daemonWorker);
    std::thread socketThread(daemonSocketServer);
    
    // Wait a bit for daemon to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);

    std::cout << "[READY] PWM controller ready on GPIO " << PWM_GPIO << std::endl;
    std::cout << "[INFO] Loaded " << pwmPlaylist.size() << " playlist entries" << std::endl;
    std::cout << "[INFO] Daemon socket: " << SOCK_PATH << std::endl;
    
    // Main loop - just keep alive and handle signals
    while (daemonRunning) {
        struct timespec sleepTime = {1, 0}; // 1초씩 sleep
        nanosleep(&sleepTime, nullptr);
    }

    // Cleanup
    std::cout << "[SHUTDOWN] Shutting down daemon threads...\n";
    
    daemonRunning = false;
    queueCv.notify_all();
    
    if (workerThread.joinable()) {
        workerThread.join();
    }
    if (socketThread.joinable()) {
        socketThread.join();
    }

    cleanup(0);
    return 0;
}
