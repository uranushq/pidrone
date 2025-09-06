#define _GNU_SOURCE
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
#include <vector>
#include <cerrno>

extern char **environ;

// ====== 튜닝 상수 ======
const int PWM_GPIO = 18;
const int COOLDOWN_MS = 5000;
const uint32_t MIN_HIGH_US = 200;   // 잡음 방지
const uint32_t MAX_HIGH_US = 4000;  // 비정상 폭 방지
const uint32_t MATCH_TOL    = 25;

// ====== 상태 ======
static uint32_t last_rise_tick = 0;
static bool have_rise = false;
static bool armed = false;        // 첫 에지 이후 true
static bool drop_first = false;   // 무장 후 첫 사이클 드롭

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;
std::map<uint32_t, std::string> pwmPlaylist;
std::atomic<bool> daemonRunning{true};
std::atomic<bool> isPlaying{false};

// ====== 명령 큐 ======
struct PlayCommand {
    std::string binFilePath;
    std::chrono::steady_clock::time_point timestamp;
};
std::queue<PlayCommand> commandQueue;
std::mutex queueMutex;
std::condition_variable queueCv;

// ====== 유틸 ======
bool pin_to_cpu_set(pid_t pid, const cpu_set_t& set, const char* tag) {
    if (sched_setaffinity(pid, sizeof(set), &set) != 0) {
        std::cerr << "[WARN] " << tag << ": sched_setaffinity(" << pid << ") failed: "
                  << strerror(errno) << "\n";
        return false;
    }
    return true;
}
bool pin_to_cpu3(const char* tag) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(3, &set);
    return pin_to_cpu_set(0, set, tag);
}

void cleanup(int code) {
    daemonRunning = false;
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
    try {
        nlohmann::json j; file >> j;
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
    if (pwmPlaylist.empty()) return 0;
    for (const auto& [p, _] : pwmPlaylist) {
        uint32_t diff = (uint32_t)std::abs((int32_t)targetPWM - (int32_t)p);
        if (diff <= MATCH_TOL) return p;
    }
    return 0;
}

void signalHandler(int sig) {
    std::cerr << "[SIGNAL] Terminated by signal " << sig << "\n";
    cleanup(0);
}

// ====== rpi_play 스폰 (자식: CPU3 + SCHED_FIFO 92) ======
int spawnRpiPlay(const std::string& binFilePath) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    // 자식 RT 우선순위는 pigpio(99)보다 낮게: 92
    struct sched_param sp{ .sched_priority = 92 };
    posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
    posix_spawnattr_setschedparam(&attr, &sp);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDULER);

    const char* argv[] = {"./build/rpi_play", binFilePath.c_str(), nullptr};
    pid_t pid;
    int rc = posix_spawn(&pid, "./build/rpi_play", nullptr, &attr,
                         (char* const*)argv, environ);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        std::cerr << "[ERROR] posix_spawn failed: " << strerror(rc) << "\n";
        return -1;
    }

    // 자식 CPU affinity를 CPU3로 강제
    cpu_set_t childset; CPU_ZERO(&childset); CPU_SET(3, &childset);
    if (!pin_to_cpu_set(pid, childset, "child")) {
        std::cerr << "[WARN] Failed to pin child to CPU3 (pid=" << pid << ")\n";
    } else {
        std::cout << "[SCHED] Child pinned to CPU3 (pid=" << pid << ")\n";
    }

    // 우선순위 재확인/보강(일부 libc/커널 조합에서 spawn 이후 재적용이 안전)
    if (sched_setscheduler(pid, SCHED_FIFO, &sp) != 0) {
        std::cerr << "[WARN] child sched_setscheduler(" << pid << ") failed: "
                  << strerror(errno) << "\n";
    }

    std::cout << "[SPAWN] rpi_play started with pid=" << pid << "\n";
    return pid;
}

// ====== 데몬 워커 ======
void daemonWorker() {
    // 워커 스레드는 CPU3에 핀(원하면 유지, 필수는 아님. 자식이 3번을 먹으므로 영향 최소)
    pin_to_cpu3("daemonWorker");

    // 워커도 RT이지만 pigpio 콜백(메인)보다 낮은 98~99 미만 권장
    struct sched_param param{ .sched_priority = 95 };
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        std::cerr << "[WARNING] Failed to set SCHED_FIFO 95 for daemon worker\n";
    } else {
        std::cout << "[SCHED] Daemon worker set to SCHED_FIFO priority 95\n";
    }

    pid_t currentChild = -1;
    std::cout << "[DAEMON_WORKER] Started\n";

    while (daemonRunning) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, []{ return !commandQueue.empty() || !daemonRunning; });
        if (!daemonRunning) break;

        if (!commandQueue.empty()) {
            PlayCommand cmd = commandQueue.front();
            commandQueue.pop();
            lock.unlock();

            if (cmd.binFilePath == "STOP") {
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

            if (currentChild > 0) {
                std::cout << "[DAEMON] Stopping current playback (pid=" << currentChild << ")\n";
                kill(currentChild, SIGTERM);
                waitpid(currentChild, nullptr, 0);
                currentChild = -1;
            }

            // 파일 캐시 웜업(옵션)
            std::ifstream f(cmd.binFilePath, std::ios::binary);
            if (f) {
                std::vector<char> buf(1024*1024);
                while (f.read(buf.data(), buf.size())) {}
                std::cout << "[CACHE_WARM] Warmed " << cmd.binFilePath << "\n";
            }

            isPlaying = true;
            currentChild = spawnRpiPlay(cmd.binFilePath);

            if (currentChild > 0) {
                std::cout << "[DAEMON] Playing " << cmd.binFilePath
                          << " (pid=" << currentChild << ")\n";
                std::thread([&currentChild]{
                    int status;
                    waitpid(currentChild, &status, 0);
                    currentChild = -1;
                    isPlaying = false;
                    std::cout << "[DAEMON] Playback finished\n";
                }).detach();
            } else {
                isPlaying = false;
            }
        }
    }

    if (currentChild > 0) {
        kill(currentChild, SIGTERM);
        waitpid(currentChild, nullptr, 0);
    }
    std::cout << "[DAEMON_WORKER] Stopped\n";
}

// ====== PWM 콜백(HIGH 폭 기준) ======
void pwmCallback(int gpio, int level, uint32_t tick) {
    if (level == 1) { // rising: HIGH 시작
        last_rise_tick = tick;
        have_rise = true;

        if (!armed) {           // 무장: 첫 에지 이후부터 정상 측정
            armed = true;
            drop_first = true;  // 무장 직후 첫 완성 사이클 버림
        }
        return;
    }

    if (level == 0) { // falling: HIGH 종료 -> HIGH 폭 계산
        if (!have_rise) return;

        uint32_t high_pw = tick - last_rise_tick;
        have_rise = false;

        // sanity guard
        if (high_pw < MIN_HIGH_US || high_pw > MAX_HIGH_US) return;

        // 무장 직후 1사이클 드롭
        if (drop_first) { drop_first = false; return; }

        // 쿨다운
        auto now = std::chrono::steady_clock::now();
        int diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COOLDOWN_MS) return;

        // 재생 중이면 STOP(3000us 근처)만 허용
        if (isPlaying) {
            if (std::abs((int32_t)high_pw - 3000) <= (int32_t)MATCH_TOL) {
                PlayCommand stopCmd{ "STOP", now };
                { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(stopCmd); }
                queueCv.notify_one();
                lastCommandTime = now;
            }
            return;
        }

        // STOP 처리
        if (std::abs((int32_t)high_pw - 3000) <= (int32_t)MATCH_TOL) {
            PlayCommand stopCmd{ "STOP", now };
            { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(stopCmd); }
            queueCv.notify_one();
            lastCommandTime = now;
            return;
        }

        // 매칭
        uint32_t m = findMatchingPWM(high_pw);
        if (!m) return;

        std::string filename = pwmPlaylist[m];
        std::string path = "./src/bin_files/" + filename;

        PlayCommand playCmd{ path, now };
        { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(playCmd); }
        queueCv.notify_one();
        lastCommandTime = now;
    }
}

int main(int argc, char* argv[]) {
    // 메인(콜백) = CPU0-2에 고정
    cpu_set_t mainset; CPU_ZERO(&mainset);
    CPU_SET(0, &mainset); CPU_SET(1, &mainset); CPU_SET(2, &mainset);
    if (!pin_to_cpu_set(0, mainset, "main")) {
        std::cerr << "[WARN] Failed to set CPU 0-2 affinity for main\n";
    } else {
        std::cout << "[SCHED] Main pinned to CPU 0-2 for PWM processing\n";
    }

    // 메인 RT 최상위
    struct sched_param spMain{ .sched_priority = 99 };
    if (sched_setscheduler(0, SCHED_FIFO, &spMain) != 0) {
        std::cerr << "[WARNING] Failed to set SCHED_FIFO 99 for main\n";
    } else {
        std::cout << "[SCHED] Main set to SCHED_FIFO priority 99\n";
    }

    std::set_terminate([](){ cleanup(1); });
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <playlist_json_path>\n";
        return 1;
    }
    std::string playlistPath = argv[1];
    if (!loadPlaylist(playlistPath)) {
        std::cerr << "[ERROR] Failed to load playlist\n";
        return 1;
    }

    // (선택) 캐시 웜업
    std::cout << "[CACHE_WARM] Warming cache for playlist files...\n";
    for (const auto& [p, fn] : pwmPlaylist) {
        std::ifstream f("./src/bin_files/" + fn, std::ios::binary);
        if (!f) continue;
        std::vector<char> buf(1024*1024);
        while (f.read(buf.data(), buf.size())) {}
        std::cout << "[CACHE_WARM] Warmed " << fn << "\n";
    }
    { std::ifstream f("./build/rpi_play", std::ios::binary);
      if (f) { std::vector<char> buf(1024*1024); while (f.read(buf.data(), buf.size())) {} } }

    // pigpio 샘플링 파라미터(초기화 이전에 호출)
    gpioCfgClock(3, PI_CLOCK_PCM, 1); // 3us, PCM, 1MHz base 분주

    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] pigpio init failed.\n";
        return 1;
    }

    // GPIO 설정 순서: Mode -> Pull -> GlitchFilter -> AlertFunc(한 번만)
    std::cout << "[GPIO] Setting up PWM input on GPIO " << PWM_GPIO << "\n";
    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetPullUpDown(PWM_GPIO, PI_PUD_DOWN);  // 필요에 따라 PI_PUD_OFF/UP + 외부풀업
    gpioGlitchFilter(PWM_GPIO, 50);            // 50us 이하 무시
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);   // 중복 등록 제거

    // 데몬 워커 시작
    std::cout << "[DAEMON] Starting daemon thread...\n";
    std::thread workerThread(daemonWorker);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "[READY] PWM controller ready on GPIO " << PWM_GPIO << "\n";
    std::cout << "[INFO] Loaded " << pwmPlaylist.size() << " playlist entries\n";

    while (daemonRunning) {
        struct timespec ts{1,0};
        nanosleep(&ts, nullptr);
    }

    std::cout << "[SHUTDOWN] Shutting down daemon thread...\n";
    daemonRunning = false;
    queueCv.notify_all();
    if (workerThread.joinable()) workerThread.join();

    cleanup(0);
    return 0;
}
