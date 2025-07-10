#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include "./lib/PCA9635_RPI.h"

// Initialize PCA9635 boards with I2C addresses
PCA9635 pca1(0x40);
PCA9635 pca2(0x41);
PCA9635 pca3(0x42);

std::map<std::string, std::vector<std::vector<uint8_t>>> binDataMap;

struct ScheduleEntry {
    std::string filename;
    std::chrono::system_clock::time_point playTime;
};

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

bool fileValidation(std::ifstream& bin , int FRAME_SIZE){
    std::streampos currentPos = bin.tellg();
    // 
    const int HEADER_SIZE = 32;
    const int TRAILER_SIZE = 16;
    
    bin.seekg(0, std::ios::end);
    std::streampos fileSize = bin.tellg();
    std::cout << fileSize <<'\n';
    
    if (fileSize <HEADER_SIZE + TRAILER_SIZE) {
        std::cerr <<"헤더, 트레일러 없는 빈 파일\n ";
        return false;
    }
    else if ( fileSize == HEADER_SIZE + TRAILER_SIZE) {
        std::cout <<"헤더, 트레일러만 있는 빈 파일 \n";
        return false ;
    }
    
    bin.seekg(-16, std::ios::end);
    char trailer[16];
    bin.read(trailer, 16);

    uint32_t frameCnt;
    uint64_t saveTime;
    uint32_t endMarker;


    std::memcpy(&frameCnt,  trailer,      4);
    std::memcpy(&saveTime,  trailer + 4,  8);
    std::memcpy(&endMarker, trailer + 12, 4);
    
    
    
    if (endMarker != 0xdeadbeef) {
        std::cout << "Frame Count : " << frameCnt << std::endl;
        std::cout << "Save Time (ms) : " << saveTime << std::endl;
        std::cout << "End Marker : 0x" << std::hex << endMarker << std::dec << std::endl;
        std::cerr << "잘못된 엔트마커 ." <<endMarker <<"\n";
        return false;
    }
    
    using std::streamoff;

    
    // 프레임 갯수 검증 
    std::streampos frameDataSize = fileSize - static_cast<streamoff>(HEADER_SIZE + TRAILER_SIZE);
    
    uint32_t totalFrameCnt = frameDataSize / FRAME_SIZE;
    
    if ( totalFrameCnt != frameCnt ){
        std::cerr << "프레임 갯수 불일치 : 트레일러에 기록된 프레임 갯수 :" << frameCnt
                  << ", 실제 빈 파일 내 프레임 갯수 :" << totalFrameCnt << "\n";
        return false;
    }
    
    bin.seekg(currentPos); 
    
    return true;
    
}

// Set the color of a specific LED (by index)
void setLED(int ledIndex, uint8_t r, uint8_t g, uint8_t b) {
    RGBChannel ch = getLEDChannel(ledIndex);
    int r_fixed = r * 2/3 ;
    int b_fixed = b * 2/3 ;
    if (ch.r.pca && ch.r.ch >= 0) ch.r.pca->analogWrite(ch.r.ch, r_fixed);
    if (ch.g.pca && ch.g.ch >= 0) ch.g.pca->analogWrite(ch.g.ch, g);
    if (ch.b.pca && ch.b.ch >= 0) ch.b.pca->analogWrite(ch.b.ch, b_fixed);
}

// Print the current RGB frame visually to the terminal (as color blocks)
void printFrameVisual(const std::vector<uint8_t>& frame) {
    std::cout << "Current Frame (4x4 RGB):\n";
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int i = y * 4 + x;
            uint8_t r = frame[i * 3 + 0];
            uint8_t g = frame[i * 3 + 1];
            uint8_t b = frame[i * 3 + 2];
            printf("\033[48;2;%d;%d;%dm  \033[0m", r, g, b);  // RGB background color in terminal
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;
}

std::chrono::system_clock::time_point parseCompactTime(const std::string& compactTimeStr) {
    std::tm tm = {};
    if (compactTimeStr.size() != 14) {
        throw std::runtime_error("잘못된 시간형식입니다. 20250525180000 형식 유지");
    }

    tm.tm_year = std::stoi(compactTimeStr.substr(0, 4)) - 1900; 
    tm.tm_mon  = std::stoi(compactTimeStr.substr(4, 2)) - 1;    
    tm.tm_mday = std::stoi(compactTimeStr.substr(6, 2));
    tm.tm_hour = std::stoi(compactTimeStr.substr(8, 2));
    tm.tm_min  = std::stoi(compactTimeStr.substr(10, 2));
    tm.tm_sec  = std::stoi(compactTimeStr.substr(12, 2));

    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

bool loadBinFile(const std::string& path, const std::string& filename, int frameSize) {
    std::ifstream bin(path + filename, std::ios::binary);
    if (!bin) {
        std::cerr << "[ERROR] Cannot open file: " << filename << "\n";
        return false;
    }

    std::vector<std::vector<uint8_t>> frames;
    bin.seekg(32, std::ios::beg);
    while (true) {
        std::vector<uint8_t> frame(frameSize);
        bin.read(reinterpret_cast<char*>(frame.data()), frameSize);
        if (bin.gcount() != frameSize) break;
        frames.push_back(frame);
    }
    binDataMap[filename] = std::move(frames);
    return true;
}

std::vector<ScheduleEntry> loadSchedule(const std::string& jsonPath) {
    std::ifstream f(jsonPath);
    nlohmann::json j;
    f >> j;

    std::vector<ScheduleEntry> schedule;
    for (auto& item : j) {
        schedule.push_back({ item["filename"], parseCompactTime(item["time"]) });
    }
    return schedule;
}


// Handle SIGINT and SIGTERM: turn off all LEDs before exit
void handleExit(int signum) {
    std::cout << "\n[강제종료] " << std::endl;
    for (int i = 0 ; i < 16; ++i) {
        setLED(i, 0, 0, 0);
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    auto start = std::chrono::high_resolution_clock::now();

    // Check if enough arguments are provided
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <path_to_bin_file> <drone pixel size>\n";
        return 1;
    }

    std::string scheduleName = argv[1];
    const std::string binFilePath = "./src/bin_files/";
    int dronePixel = std::stoi(argv[2]); // Convert string to integer
    int frameSize = dronePixel * dronePixel * 3;

    // Register signal handlers for safe exit
    signal(SIGINT, handleExit);
    signal(SIGTERM, handleExit);

    // Initialize all PCA9635 boards
    if (!pca1.begin() || !pca2.begin() || !pca3.begin()) {
        std::cerr << "Failed to initialize PCA9635 boards.\n";
        return 1;
    }
    
    auto schedule = loadSchedule(scheduleName);
    for (const auto& entry : schedule) {
        if (binDataMap.find(entry.filename) == binDataMap.end()) {
            if (!loadBinFile(binFilePath, entry.filename, frameSize)) {
                return 1;
            }
        }
    }

    for (const auto& entry : schedule) {
        auto now = std::chrono::system_clock::now();
        if (entry.playTime > now) {
            std::this_thread::sleep_until(entry.playTime);
        }

        const auto& frames = binDataMap[entry.filename];
        for (const auto& frame : frames) {
            for (int i = 0; i < dronePixel * dronePixel; ++i) {
                setLED(i, frame[i * 3 + 0], frame[i * 3 + 1], frame[i * 3 + 2]);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    
    // Turn off all LEDs after playback
    for (int i = 0; i < 16; ++i) {
        setLED(i, 0, 0, 0);
    }

    return 0;
}
