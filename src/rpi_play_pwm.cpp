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
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

extern char **environ;

/* ==========================
 * 로그 유틸 (스레드세이프)
 * ========================== */
static std::mutex g_log_mtx;

static inline std::string now_hhmmss_us() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    auto t  = system_clock::to_time_t(tp);
    auto us = duration_cast<microseconds>(tp.time_since_epoch()) % seconds(1);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << "."
        << std::setw(6) << std::setfill('0') << us.count();
    return oss.str();
}
static inline void log_line(const char* lvl, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cerr << "[" << now_hhmmss_us() << "][" << lvl << "][tid " << gettid() << "] " << msg << "\n";
}
#define LOGI(expr) do { std::ostringstream _o; _o << expr; log_line("INFO",  _o.str()); } while(0)
#define LOGW(expr) do { std::ostringstream _o; _o << expr; log_line("WARN",  _o.str()); } while(0)
#define LOGE(expr) do { std::ostringstream _o; _o << expr; log_line("ERROR", _o.str()); } while(0)
#define LOGF(expr) do { std::ostringstream _o; _o << expr; log_line("FILTER",_o.str()); } while(0)
#define LOGD(expr) do { std::ostringstream _o; _o << expr; log_line("DEBUG", _o.str()); } while(0)

/* ==========================
 * 튜닝 상수
 * ========================== */
const int PWM_GPIO        = 18;
const int COOLDOWN_MS     = 5000;
const uint32_t MIN_HIGH_US= 200;   // 잡음 방지 하한
const uint32_t MAX_HIGH_US= 4000;  // 비정상 폭 상한
const uint32_t MATCH_TOL  = 25;

/* ==========================
 * 상태
 * ========================== */
static uint32_t last_rise_tick = 0;
static bool have_rise          = false;
static bool armed              = false; // 첫 에지 이후 true
static bool drop_first         = false; // 무장 직후 첫 사이클 드롭

// 주기(상승→상승) 검증용
static uint32_t prev_rise_tick = 0;
static bool have_prev_rise     = false;

// 미디안(3) 필터
static uint32_t hi_buf[3] = {0,0,0};
static int hi_idx = 0;
static int hi_cnt = 0;

// 디바운스: 같은 버킷 2회 연속
static uint32_t last_bucket    = 0;
static int same_bucket_cnt     = 0;

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;
std::map<uint32_t, std::string> pwmPlaylist;
std::atomic<bool> daemonRunning{true};
std::atomic<bool> isPlaying{false};

/* ==========================
 * 명령 큐
 * ========================== */
struct PlayCommand {
    std::string binFilePath;
    std::chrono::steady_clock::time_point timestamp;
};
std::queue<PlayCommand> commandQueue;
std::mutex queueMutex;
std::condition_variable queueCv;

/* ==========================
 * 유틸
 * ========================== */
bool pin_to_cpu_set(pid_t pid, const cpu_set_t& set, const char* tag) {
    int rc = sched_setaffinity(pid, sizeof(set), &set);
    if (rc != 0) {
        LOGW(tag << ": sched_setaffinity(" << pid << ") failed: " << strerror(errno));
        return false;
    }
    LOGI(tag << ": sched_setaffinity(" << pid << ") OK");
    return true;
}
bool pin_to_cpu3(const char* tag) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(3, &set);
    return pin_to_cpu_set(0, set, tag);
}

void cleanup(int code) {
    daemonRunning = false;
    gpioTerminate();
    LOGI("[EXIT] GPIO cleaned up (code=" << code << ")");
    exit(code);
}

bool loadPlaylist(const std::string& playlistPath) {
    std::ifstream file(playlistPath);
    if (!file.is_open()) {
        LOGE("[PLAYLIST] Cannot open: " << playlistPath);
        return false;
    }
    try {
        nlohmann::json j; file >> j;
        pwmPlaylist.clear();
        for (auto& [key, value] : j.items()) {
            uint32_t pwmValue = std::stoul(key);
            std::string filename = value["filename"];
            pwmPlaylist[pwmValue] = filename;
            LOGI("[PLAYLIST] " << pwmValue << "us -> " << filename);
        }
        LOGI("[PLAYLIST] Loaded entries: " << pwmPlaylist.size());
        return true;
    } catch (const std::exception& e) {
        LOGE("[PLAYLIST] JSON parse error: " << e.what());
        return false;
    }
}

uint32_t findMatchingPWM(uint32_t targetPWM) {
    if (pwmPlaylist.empty()) return 0;
    uint32_t best = 0; uint32_t best_err = 0xffffffffu;
    for (const auto& [p, _] : pwmPlaylist) {
        uint32_t err = (uint32_t)std::abs((int32_t)targetPWM - (int32_t)p);
        if (err < best_err) { best_err = err; best = p; }
        if (err <= MATCH_TOL) {
            LOGD("[MATCH] target=" << targetPWM << "us matched=" << p << "us (err=" << err << ")");
            return p;
        }
    }
    LOGD("[MATCH] target=" << targetPWM << "us no bucket within " << MATCH_TOL << "us (best=" << best << " err=" << best_err << ")");
    return 0;
}

void signalHandler(int sig) {
    LOGW("[SIGNAL] Caught signal " << sig);
    cleanup(0);
}

/* ==========================
 * rpi_play 스폰 (자식: CPU3 + SCHED_FIFO 92)
 * ========================== */
int spawnRpiPlay(const std::string& binFilePath) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    struct sched_param sp{ .sched_priority = 92 }; // pigpio 콜백(99)보다 낮게
    posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
    posix_spawnattr_setschedparam(&attr, &sp);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDULER);

    const char* argv[] = {"./build/rpi_play", binFilePath.c_str(), nullptr};
    pid_t pid;
    int rc = posix_spawn(&pid, "./build/rpi_play", nullptr, &attr,
                         (char* const*)argv, environ);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        LOGE("[SPAWN] posix_spawn failed: " << strerror(rc));
        return -1;
    }

    cpu_set_t childset; CPU_ZERO(&childset); CPU_SET(3, &childset);
    if (!pin_to_cpu_set(pid, childset, "child")) {
        LOGW("[SPAWN] Failed to pin child to CPU3 pid=" << pid);
    } else {
        LOGI("[SPAWN] Child pinned to CPU3 pid=" << pid);
    }

    if (sched_setscheduler(pid, SCHED_FIFO, &sp) != 0) {
        LOGW("[SPAWN] child sched_setscheduler(" << pid << ") failed: " << strerror(errno));
    } else {
        LOGI("[SPAWN] child SCHED_FIFO prio=" << sp.sched_priority << " pid=" << pid);
    }

    LOGI("[SPAWN] rpi_play started pid=" << pid << " file=" << binFilePath);
    return pid;
}

/* ==========================
 * 데몬 워커
 * ========================== */
void daemonWorker() {
    pin_to_cpu3("daemonWorker");
    struct sched_param param{ .sched_priority = 95 };
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        LOGW("[WORKER] set SCHED_FIFO 95 failed: " << strerror(errno));
    } else {
        LOGI("[WORKER] SCHED_FIFO prio=95");
    }

    pid_t currentChild = -1;
    LOGI("[WORKER] Started");

    while (daemonRunning) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, []{ return !commandQueue.empty() || !daemonRunning; });
        if (!daemonRunning) break;

        if (!commandQueue.empty()) {
            PlayCommand cmd = commandQueue.front();
            commandQueue.pop();
            lock.unlock();

            if (cmd.binFilePath == "STOP") {
                LOGI("[WORKER] STOP command received");
                if (currentChild > 0) {
                    LOGI("[WORKER] Terminating pid=" << currentChild);
                    kill(currentChild, SIGTERM);
                    waitpid(currentChild, nullptr, 0);
                    currentChild = -1;
                }
                isPlaying = false;
                LOGI("[WORKER] Playback stopped");
                continue;
            }

            if (currentChild > 0) {
                LOGI("[WORKER] Replacing current pid=" << currentChild);
                kill(currentChild, SIGTERM);
                waitpid(currentChild, nullptr, 0);
                currentChild = -1;
            }

            // 캐시 웜업(옵션)
            {
                std::ifstream f(cmd.binFilePath, std::ios::binary);
                if (f) {
                    std::vector<char> buf(1024*1024);
                    size_t total = 0;
                    while (f.read(buf.data(), buf.size())) total += buf.size();
                    total += f.gcount();
                    LOGI("[CACHE] Warmed " << cmd.binFilePath << " bytes~" << total);
                } else {
                    LOGW("[CACHE] Cannot open " << cmd.binFilePath);
                }
            }

            isPlaying = true;
            currentChild = spawnRpiPlay(cmd.binFilePath);

            if (currentChild > 0) {
                LOGI("[WORKER] Playing " << cmd.binFilePath << " pid=" << currentChild);
                std::thread([&currentChild]{
                    int status;
                    pid_t w = waitpid(currentChild, &status, 0);
                    if (w > 0) {
                        LOGI("[WORKER] Child finished pid=" << w << " status=" << status);
                    } else {
                        LOGW("[WORKER] waitpid failed: " << strerror(errno));
                    }
                    currentChild = -1;
                    isPlaying = false;
                }).detach();
            } else {
                LOGE("[WORKER] spawn failed; isPlaying=false");
                isPlaying = false;
            }
        }
    }

    if (currentChild > 0) {
        LOGI("[WORKER] Cleaning child pid=" << currentChild);
        kill(currentChild, SIGTERM);
        waitpid(currentChild, nullptr, 0);
    }
    LOGI("[WORKER] Stopped");
}

/* ==========================
 * 유틸: 미디안(3)
 * ========================== */
static inline uint32_t median3(uint32_t a, uint32_t b, uint32_t c){
    if (a>b) std::swap(a,b);
    if (b>c) std::swap(b,c);
    if (a>b) std::swap(a,b);
    return b;
}

/* ==========================
 * PWM 콜백(HIGH 폭 기준)
 * ========================== */
void pwmCallback(int gpio, int level, uint32_t tick) {
    if (level == 1) { // rising
        last_rise_tick = tick;
        have_rise = true;
        LOGD("[EDGE] RISE tick=" << tick);

        if (!armed) {
            armed = true;
            drop_first = true;   // 무장 직후 첫 사이클 드랍
            have_prev_rise = false;
            LOGD("[STATE] Armed=true (drop_first=1)");
        }
        return;
    }

    if (level == 0) { // falling -> HIGH 폭 계산
        LOGD("[EDGE] FALL tick=" << tick);

        if (!have_rise) {
            LOGF("no prior RISE -> ignore FALL");
            return;
        }

        uint32_t high_pw = tick - last_rise_tick;
        have_rise = false;
        LOGD("[MEAS] HIGH width raw=" << high_pw << "us (tick=" << tick << ", last_rise=" << last_rise_tick << ")");

        // glitchFilter에 걸린 에지는 콜백이 아예 안 옴(로깅 불가).
        // 여기서는 high_pw 가드만 로깅.
        if (high_pw < MIN_HIGH_US || high_pw > MAX_HIGH_US) {
            LOGF("width_guard drop high_pw=" << high_pw << "us (min=" << MIN_HIGH_US << ", max=" << MAX_HIGH_US << ")");
            return;
        }

        // 무장 직후 1사이클 드랍
        if (drop_first) {
            drop_first = false;
            prev_rise_tick = last_rise_tick;
            have_prev_rise = true;
            LOGF("drop_first cycle ignored (prime prev_rise_tick=" << prev_rise_tick << ")");
            return;
        }

        // 주기(상승→상승) 15~25ms 검사
        if (!have_prev_rise) {
            prev_rise_tick = last_rise_tick;
            have_prev_rise = true;
            LOGD("[PERIOD] initialize prev_rise_tick=" << prev_rise_tick);
            return;
        } else {
            uint32_t period = last_rise_tick - prev_rise_tick;
            prev_rise_tick = last_rise_tick;
            LOGD("[PERIOD] measure period=" << period << "us");
            if (period < 15000 || period > 25000) {
                LOGF("period_guard drop period=" << period << "us (expected 15000~25000)");
                return;
            }
        }

        // 미디안(3) 필터
        hi_buf[hi_idx] = high_pw;
        hi_idx = (hi_idx + 1) % 3;
        if (hi_cnt < 3) hi_cnt++;
        uint32_t filt = (hi_cnt == 3) ? median3(hi_buf[0], hi_buf[1], hi_buf[2]) : high_pw;
        LOGD("[FILTER] median3 in={" << hi_buf[0] << "," << hi_buf[1] << "," << hi_buf[2] << "} cnt=" << hi_cnt << " -> " << filt << "us");

        // 쿨다운
        auto now = std::chrono::steady_clock::now();
        int diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCommandTime).count();
        if (diff < COOLDOWN_MS) {
            LOGF("cooldown drop " << diff << "ms < " << COOLDOWN_MS << "ms");
            return;
        }

        // 재생 중: STOP(3000us 근처)만 허용
        if (isPlaying) {
            if (std::abs((int32_t)filt - 3000) <= (int32_t)MATCH_TOL) {
                LOGI("[STOP] while playing: filt=" << filt << "us (tol " << MATCH_TOL << ")");
                PlayCommand stopCmd{ "STOP", now };
                { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(stopCmd); }
                queueCv.notify_one();
                lastCommandTime = now;
            } else {
                LOGF("playing_ignore filt=" << filt << "us (only STOP allowed)");
            }
            return;
        }

        // STOP 개별 처리
        if (std::abs((int32_t)filt - 3000) <= (int32_t)MATCH_TOL) {
            LOGI("[STOP] request filt=" << filt << "us");
            PlayCommand stopCmd{ "STOP", now };
            { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(stopCmd); }
            queueCv.notify_one();
            lastCommandTime = now;
            return;
        }

        // 매칭(버킷)
        uint32_t m = findMatchingPWM(filt);
        if (!m) {
            LOGF("no_match filt=" << filt << "us");
            same_bucket_cnt = 0;
            last_bucket = 0;
            return;
        }

        // 디바운스(같은 버킷 2회 연속)
        if (last_bucket == m) same_bucket_cnt++;
        else { last_bucket = m; same_bucket_cnt = 1; }
        LOGD("[DEBOUNCE] bucket=" << m << " cnt=" << same_bucket_cnt);

        if (same_bucket_cnt < 2) {
            LOGF("debounce_hold bucket=" << m << " need 2x");
            return;
        }

        // 발사
        std::string filename = pwmPlaylist[m];
        std::string path = "./src/bin_files/" + filename;

        LOGI("[CMD] filt=" << filt << "us -> " << filename);
        PlayCommand playCmd{ path, now };
        { std::lock_guard<std::mutex> lk(queueMutex); commandQueue.push(playCmd); }
        queueCv.notify_one();

        lastCommandTime = now;
        same_bucket_cnt = 0;
    }
}

/* ==========================
 * main
 * ========================== */
int main(int argc, char* argv[]) {
    LOGI("===== RPI PWM Receiver starting =====");

    // 메인(콜백) = CPU0-2에 고정
    cpu_set_t mainset; CPU_ZERO(&mainset);
    CPU_SET(0, &mainset); CPU_SET(1, &mainset); CPU_SET(2, &mainset);
    if (!pin_to_cpu_set(0, mainset, "main")) {
        LOGW("Failed to set CPU 0-2 affinity for main");
    } else {
        LOGI("Main pinned to CPU 0-2 for PWM processing");
    }

    // 메인 RT 최상위
    struct sched_param spMain{ .sched_priority = 99 };
    if (sched_setscheduler(0, SCHED_FIFO, &spMain) != 0) {
        LOGW("Failed to set SCHED_FIFO 99 for main: " << strerror(errno));
    } else {
        LOGI("Main set to SCHED_FIFO priority 99");
    }

    std::set_terminate([](){ cleanup(1); });
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    if (argc != 2) {
        LOGE("Usage: program <playlist_json_path>");
        return 1;
    }
    std::string playlistPath = argv[1];
    LOGI("Playlist path: " << playlistPath);
    if (!loadPlaylist(playlistPath)) {
        LOGE("Failed to load playlist");
        return 1;
    }

    // (선택) 캐시 웜업
    LOGI("[CACHE] Warming playlist files...");
    for (const auto& [p, fn] : pwmPlaylist) {
        std::ifstream f("./src/bin_files/" + fn, std::ios::binary);
        if (!f) { LOGW("[CACHE] cannot open " << fn); continue; }
        std::vector<char> buf(1024*1024);
        size_t total = 0;
        while (f.read(buf.data(), buf.size())) total += buf.size();
        total += f.gcount();
        LOGI("[CACHE] warmed " << fn << " bytes~" << total);
    }
    { std::ifstream f("./build/rpi_play", std::ios::binary);
      if (f) { std::vector<char> buf(1024*1024);
               size_t total=0; while (f.read(buf.data(), buf.size())) total += buf.size();
               total += f.gcount();
               LOGI("[CACHE] warmed rpi_play bytes~" << total);
      } else { LOGW("[CACHE] cannot open ./build/rpi_play"); } }

    // pigpio 샘플링 파라미터(초기화 이전에 호출)
    // 주의: glitchFilter로 걸러진 에지는 콜백이 호출되지 않아 로깅 불가.
    gpioCfgClock(3, PI_CLOCK_PCM, 1); // 3us, PCM, 1MHz base 분주
    LOGI("[GPIO] gpioCfgClock: clkMicros=3, clkPeripheral=PCM, cfgMicros=1");

    if (gpioInitialise() < 0) {
        LOGE("[GPIO] pigpio init failed");
        return 1;
    }
    LOGI("[GPIO] pigpio initialised");

    // GPIO 설정 순서: Mode -> Pull -> GlitchFilter -> AlertFunc(한 번만)
    LOGI("[GPIO] Setting up PWM input on GPIO " << PWM_GPIO);
    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetPullUpDown(PWM_GPIO, PI_PUD_DOWN);
    LOGI("[GPIO] PullDown set (note: if external pull exists, adjust)");
    gpioGlitchFilter(PWM_GPIO, 10); // 50us 이하 에지는 콜백 미호출 (로그 불가)
    LOGI("[GPIO] GlitchFilter set to 50us (edges shorter than this will not be logged)");
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);
    LOGI("[GPIO] AlertFunc registered");

    // 데몬 워커 시작
    LOGI("[DAEMON] Starting worker thread...");
    std::thread workerThread(daemonWorker);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    LOGI("[READY] PWM controller ready on GPIO " << PWM_GPIO);
    LOGI("[INFO ] Loaded playlist entries: " << pwmPlaylist.size());

    while (daemonRunning) {
        struct timespec ts{1,0};
        nanosleep(&ts, nullptr);
    }

    LOGI("[SHUTDOWN] Shutting down worker...");
    daemonRunning = false;
    queueCv.notify_all();
    if (workerThread.joinable()) workerThread.join();

    cleanup(0);
    return 0;
}
