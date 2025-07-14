#include <pigpio.h>
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>

const int PWM_GPIO = 18;
const uint32_t PW_PLAY = 1000;
const int TOL = 10;
const int COOLDOWN_MS = 5000;

uint32_t lastTick = 0;
std::chrono::steady_clock::time_point lastCommandTime;

std::string jsonFilePath;
int playIndex;

void cleanup(int code) {
    gpioTerminate();
    std::cerr << "[EXIT] GPIO cleaned up.\n";
    exit(code);
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
            std::cout << "[SKIP] Cooldown " << diff << "ms\n";
            return;
        }

        if (PW_PLAY - TOL <= pw && pw <= PW_PLAY + TOL) {
            lastCommandTime = now;
            std::cout << "[TRIGGER] Executing rpi_play...\n";

            std::string cmd = "sudo chrt -f 99 ./build/rpi_play " + jsonFilePath + " " + std::to_string(playIndex);
            int result = system(cmd.c_str());
            if (result != 0) std::cerr << "[ERROR] rpi_play execution failed.\n";
        }
    }
}

int main(int argc, char* argv[]) {
    std::set_terminate([](){ cleanup(1); });
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <json_path> <index>\n";
        return 1;
    }

    jsonFilePath = argv[1];
    playIndex = std::stoi(argv[2]);

    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] pigpio init failed.\n";
        return 1;
    }

    gpioSetMode(PWM_GPIO, PI_INPUT);
    gpioSetAlertFunc(PWM_GPIO, pwmCallback);

    std::cout << "[READY] Waiting for PWM trigger on GPIO " << PWM_GPIO << "...\n";
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cleanup(0);
    return 0;
}
