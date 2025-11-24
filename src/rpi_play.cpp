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
#include <time.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <sched.h>


// Initialize PCA9635 boards with I2C addresses
PCA9635 pca1(0x4b);
PCA9635 pca2(0x4d);
PCA9635 pca3(0x4e);
PCA9635 pca4(0x4f);

std::map<std::string, std::vector<std::vector<uint8_t>>> binDataMap;

int getLastOctetFromIP() {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    
    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "[ERROR] getifaddrs failed, using default ID 4\n";
        return 4;
    }
    
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        family = ifa->ifa_addr->sa_family;
        
        // Skip loopback interface
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        
        if (family == AF_INET) {
            struct sockaddr_in* addr_in = (struct sockaddr_in*)ifa->ifa_addr;
            std::string ip = inet_ntoa(addr_in->sin_addr);
            
            // Find the last dot and extract the last octet
            size_t lastDot = ip.find_last_of('.');
            if (lastDot != std::string::npos) {
                int lastOctet = std::stoi(ip.substr(lastDot + 1));
                std::cout << "[INFO] Found IP: " << ip << ", last octet: " << lastOctet << std::endl;
                freeifaddrs(ifaddr);
                return lastOctet;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    std::cerr << "[ERROR] No valid IP found, using default ID 5\n";
    return 4;  // Default fallback
}

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

// Read image dimensions from bin file header
bool getImageDimensions(const std::string& filepath, int& total_row, int& total_col) {
    std::ifstream bin(filepath, std::ios::binary);
    if (!bin) {
        std::cerr << "[ERROR] Cannot open file for dimension reading: " << filepath << "\n";
        return false;
    }
    
    // Read the 16-byte header (4 uint32 values)
    bin.seekg(0, std::ios::beg);
    char header[16];
    bin.read(header, 16);
    
    if (bin.gcount() != 16) {
        std::cerr << "[ERROR] Could not read complete header\n";
        return false;
    }
    
    uint32_t total_frames, height, width, fps;
    std::memcpy(&total_frames, header + 0, 4);   // total_frames
    std::memcpy(&height, header + 4, 4);         // height  
    std::memcpy(&width, header + 8, 4);          // width
    std::memcpy(&fps, header + 12, 4);           // fps
    
    std::cout << "Header info:" << std::endl;
    std::cout << "Total frames: " << total_frames << std::endl;
    std::cout << "Height: " << height << std::endl;
    std::cout << "Width: " << width << std::endl;
    std::cout << "FPS: " << fps << std::endl;
    
    // Validate the values are reasonable
    if (width <= 0 || width > 1000 || height <= 0 || height > 1000) {
        std::cerr << "[ERROR] Invalid dimensions: " << width << "x" << height << std::endl;
        return false;
    }
    
    // Set output values
    total_col = width;   // width = number of columns
    total_row = height;  // height = number of rows
    
    std::cout << "Final dimensions: " << width << "x" << height 
              << " (cols x rows: " << total_col << "x" << total_row << ")" << std::endl;
    
    return true;
}

bool loadBinFile(const std::string& path, const std::string& filename, int frameSize) {
    std::ifstream bin(path + filename, std::ios::binary);
    if (!bin) {
        std::cerr << "[ERROR] Cannot open file: " << filename << "\n";
        return false;
    }

    // Get file size to calculate actual available frames
    bin.seekg(0, std::ios::end);
    std::streampos fileSize = bin.tellg();
    
    // Calculate actual frames based on file size
    // Header is 16 bytes, trailer might be 16 bytes (32 total overhead)
    std::streampos frameDataSize = fileSize - 32;  // subtract header + trailer
    uint32_t actualFrames = frameDataSize / frameSize;
    
    std::cout << "File size: " << fileSize << " bytes" << std::endl;
    std::cout << "Frame size: " << frameSize << " bytes" << std::endl;
    std::cout << "Calculated frames: " << actualFrames << std::endl;

    std::vector<std::vector<uint8_t>> frames;
    bin.seekg(16, std::ios::beg);  // Skip 16-byte header
    
    for (uint32_t i = 0; i < actualFrames; ++i) {
        std::vector<uint8_t> frame(frameSize);
        bin.read(reinterpret_cast<char*>(frame.data()), frameSize);
        if (bin.gcount() != frameSize) {
            std::cout << "End of frame data reached at frame " << i << std::endl;
            break;
        }
        frames.push_back(frame);
    }
    
    binDataMap[filename] = std::move(frames);
    std::cout << "Loaded " << binDataMap[filename].size() << " frames from " << filename << std::endl;
    return true;
}

std::vector<ScheduleEntry> loadSchedule(const std::string& jsonPath) {
    std::ifstream f(jsonPath);
    nlohmann::json j;
    f >> j;

    std::vector<ScheduleEntry> schedule;
    
    // Check if it's the new format (object with PWM keys) or old format (array)
    if (j.is_object() && !j.empty()) {
        // New format: {"900": {"filename": "...", "time": "..."}, ...}
        for (auto& [pwmKey, item] : j.items()) {
            schedule.push_back({ item["filename"], parseCompactTime(item["time"]) });
        }
    } else if (j.is_array()) {
        // Old format: [{"filename": "...", "time": "..."}, ...]
        for (auto& item : j) {
            schedule.push_back({ item["filename"], parseCompactTime(item["time"]) });
        }
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
    // Set high priority for LED control process
    struct sched_param param;
    param.sched_priority = 98;  // High priority for LED timing precision
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        std::cerr << "[WARNING] Failed to set high priority scheduling for LED control\n";
    } else {
        std::cout << "[SCHED] LED control process set to SCHED_FIFO priority 98\n";
    }
    
    auto start = std::chrono::high_resolution_clock::now();

    // Check if enough arguments are provided
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <bin_file_path>\n";
        return 1;
    }

    std::string binFilePath = argv[1];
    
    std::cout << "Loading bin file: " << binFilePath << std::endl;
    
    // Read image dimensions from the bin file
    int total_row, total_col;
    if (!getImageDimensions(binFilePath, total_row, total_col)) {
        std::cerr << "Failed to read image dimensions from: " << binFilePath << std::endl;
        return 1;
    }
    
    const int n_row = total_row / 4;
    const int n_col = total_col / 4;
    // const int raspberry_pi_id = getLastOctetFromIP();
    const int raspberry_pi_id = 3;

    std::cout << "[INFO] Raspberry Pi ID: " << raspberry_pi_id << std::endl;
    
    // Calculate this pi's position in the grid
    const int pi_row = raspberry_pi_id / n_col;
    const int pi_col = raspberry_pi_id % n_col;
    
    // Each raspberry pi handles 4x4 pixels
    const int local_pixel_size = 4;
    int frameSize = total_row * total_col * 3;  // Full frame size
    
    std::cout << "Image dimensions: " << total_row << "x" << total_col << std::endl;
    std::cout << "Grid size: " << n_row << "x" << n_col << " blocks" << std::endl;
    std::cout << "This Pi position: (" << pi_row << "," << pi_col << ")" << std::endl;

    // Register signal handlers for safe exit
    signal(SIGINT, handleExit);
    signal(SIGTERM, handleExit);

    // Initialize all PCA9635 boards
    bool pca_initialized = pca1.begin() && pca2.begin() && pca3.begin() && pca4.begin();
    if (!pca_initialized) {
        std::cerr << "Warning: Failed to initialize PCA9635 boards. Continuing for file validation...\n";
    } else {
        std::cout << "PCA9635 boards initialized successfully.\n";
    }
    
    // Load the single bin file
    std::cout << "Loading bin file: " << binFilePath << std::endl;
    
    // Extract just the filename from the full path
    std::string filename = binFilePath.substr(binFilePath.find_last_of("/\\") + 1);
    std::string path = binFilePath.substr(0, binFilePath.find_last_of("/\\") + 1);
    
    if (!loadBinFile(path, filename, frameSize)) {
        std::cerr << "Failed to load: " << binFilePath << std::endl;
        return 1;
    } else {
        std::cout << "Successfully loaded: " << filename << " with " 
                 << binDataMap[filename].size() << " frames" << std::endl;
    }
    
    if (!pca_initialized) {
        std::cout << "File validation completed. Exiting due to PCA9635 initialization failure." << std::endl;
        return 0;
    }

    // Play the loaded bin file
    const auto& frames = binDataMap[filename];
    
    const int interval_us = 067'000;
    const int repeat_count = 20;  // Number of times to repeat playback
    
    for (int repeat = 0; repeat < repeat_count; ++repeat) {
        std::cout << "[REPEAT] Starting playback iteration " << (repeat + 1) << "/" << repeat_count << std::endl;
        
        struct timespec nextFrameTime, actualTime;
        clock_gettime(CLOCK_MONOTONIC, &nextFrameTime);
        
        int frameIndex = 0;
        for (const auto& frame : frames) {
        // Log frame start time and LED sending start
        clock_gettime(CLOCK_MONOTONIC, &actualTime);
        std::cout << "[FRAME_START] Frame " << frameIndex << " start at " 
                  << actualTime.tv_sec << "." << std::setfill('0') << std::setw(9) << actualTime.tv_nsec 
                  << std::endl;
        
        // Measure LED sending time
        auto ledSendStart = std::chrono::high_resolution_clock::now();
        
        // Extract 4x4 region for this raspberry pi from the full image frame
        // Python code flattens as: [pixel for row in matrix for pixel in row]
        // This means row-major order: row 0 all pixels, then row 1 all pixels, etc.
        for (int local_row = 0; local_row < local_pixel_size; ++local_row) {
            for (int local_col = 0; local_col < local_pixel_size; ++local_col) {
                // Calculate global position in the full image
                // Flip the row order to fix upside-down issue
                int global_row = (n_row - 1 - pi_row) * local_pixel_size + local_row;
                int global_col = pi_col * local_pixel_size + local_col;
                
                // Calculate index in the flattened frame (row-major order)
                int global_index = global_row * total_col + global_col;
                
                // Calculate local LED index (0-15 for 4x4)
                int local_led_index = local_row * local_pixel_size + local_col;
                
                // Set LED with RGB values from the full frame (only if PCA initialized)
                if (pca_initialized) {
                    setLED(local_led_index, 
                           frame[global_index * 3 + 0], 
                           frame[global_index * 3 + 1], 
                           frame[global_index * 3 + 2]);
                }
            }
        }
        
        // Measure and log LED sending completion time
        auto ledSendEnd = std::chrono::high_resolution_clock::now();
        auto ledDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(ledSendEnd - ledSendStart).count();
        
        struct timespec ledSendTime;
        clock_gettime(CLOCK_MONOTONIC, &ledSendTime);
        std::cout << "[LED_SENT] Frame " << frameIndex << " LEDs sent at " 
                  << ledSendTime.tv_sec << "." << std::setfill('0') << std::setw(9) << ledSendTime.tv_nsec 
                  << ", LED_duration: " << ledDuration << "ns" << std::endl;

        nextFrameTime.tv_nsec += interval_us * 1000;
        if (nextFrameTime.tv_nsec >= 1000000000) {
            nextFrameTime.tv_sec += 1;
            nextFrameTime.tv_nsec -= 1000000000;
        }
        
        // Log expected vs actual sleep timing
        struct timespec beforeSleep, afterSleep;
        clock_gettime(CLOCK_MONOTONIC, &beforeSleep);
        
        std::cout << "[SLEEP_START] Frame " << frameIndex << " sleep start at " 
                  << beforeSleep.tv_sec << "." << std::setfill('0') << std::setw(9) << beforeSleep.tv_nsec 
                  << ", target: " << nextFrameTime.tv_sec << "." << std::setfill('0') << std::setw(9) << nextFrameTime.tv_nsec 
                  << std::endl;
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextFrameTime, nullptr);
        
        clock_gettime(CLOCK_MONOTONIC, &afterSleep);
        
        // Calculate timing accuracy
        long long expectedNs = nextFrameTime.tv_sec * 1000000000LL + nextFrameTime.tv_nsec;
        long long actualNs = afterSleep.tv_sec * 1000000000LL + afterSleep.tv_nsec;
        long long beforeNs = beforeSleep.tv_sec * 1000000000LL + beforeSleep.tv_nsec;
        long long diffNs = actualNs - expectedNs;
        long long sleepTimeNs = actualNs - beforeNs;
        long long targetSleepNs = expectedNs - beforeNs;
        
        std::cout << "[SLEEP_END] Frame " << frameIndex << " wakeup at " 
                  << afterSleep.tv_sec << "." << std::setfill('0') << std::setw(9) << afterSleep.tv_nsec 
                  << ", accuracy: " << diffNs << "ns, slept: " << sleepTimeNs 
                  << "ns, target: " << targetSleepNs << "ns" << std::endl;
        frameIndex++;
        }
        
        std::cout << "[REPEAT] Completed playback iteration " << (repeat + 1) << "/" << repeat_count << std::endl;
    }
    
    // Turn off all LEDs after playback (only if PCA initialized)
    if (pca_initialized) {
        for (int i = 0; i < 16; ++i) {
            setLED(i, 0, 0, 0);
        }
    }

    return 0;
}
