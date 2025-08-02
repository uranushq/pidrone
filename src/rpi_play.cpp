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


// Initialize PCA9635 boards with I2C addresses
PCA9635 pca1(0x40);
PCA9635 pca2(0x41);
PCA9635 pca3(0x42);

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
                return lastOctet - 1;  // Subtract 1 as requested
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

// Read image dimensions from bin file header
bool getImageDimensions(const std::string& filepath, int& total_row, int& total_col) {
    std::ifstream bin(filepath, std::ios::binary);
    if (!bin) {
        std::cerr << "[ERROR] Cannot open file for dimension reading: " << filepath << "\n";
        return false;
    }
    
    // Read the entire 32-byte header for debugging
    bin.seekg(0, std::ios::beg);
    char header[32];
    bin.read(header, 32);
    
    std::cout << "Header debug (all 32 bytes as integers):" << std::endl;
    for (int i = 0; i < 32; i += 4) {
        int val;
        std::memcpy(&val, header + i, 4);
        std::cout << "Offset " << std::setw(2) << i << ": " << std::setw(8) << val 
                  << " (0x" << std::hex << val << std::dec << ")" << std::endl;
    }
    
    // Based on analysis, both offset 4 and 8 contain 12
    // This suggests the image is 12x12
    int width, height;
    
    std::memcpy(&width, header + 4, 4);   // Use offset 4 for width
    std::memcpy(&height, header + 8, 4);  // Use offset 8 for height
    
    std::cout << "Reading from offsets 4,8: width=" << width << ", height=" << height << std::endl;
    
    // Validate the values are reasonable
    if (width <= 0 || width > 100 || height <= 0 || height > 100) {
        std::cout << "Values seem unreasonable, defaulting to 12x12" << std::endl;
        width = 12;
        height = 12;
    }
    
    // Convert image dimensions to matrix dimensions
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
    const int raspberry_pi_id = getLastOctetFromIP();

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
    bool pca_initialized = pca1.begin() && pca2.begin() && pca3.begin();
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
    
    const int interval_us = 200'000;
    struct timespec nextFrameTime;
    clock_gettime(CLOCK_MONOTONIC, &nextFrameTime);
    
    for (const auto& frame : frames) {
        // Extract 4x4 region for this raspberry pi from the full image frame
        // Python code flattens as: [pixel for row in matrix for pixel in row]
        // This means row-major order: row 0 all pixels, then row 1 all pixels, etc.
        for (int local_row = 0; local_row < local_pixel_size; ++local_row) {
            for (int local_col = 0; local_col < local_pixel_size; ++local_col) {
                // Calculate global position in the full image
                int global_row = pi_row * local_pixel_size + local_row;
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

        nextFrameTime.tv_nsec += interval_us * 1000;
        if (nextFrameTime.tv_nsec >= 1000000000) {
            nextFrameTime.tv_sec += 1;
            nextFrameTime.tv_nsec -= 1000000000;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextFrameTime, nullptr);
    }
    
    // Turn off all LEDs after playback (only if PCA initialized)
    if (pca_initialized) {
        for (int i = 0; i < 16; ++i) {
            setLED(i, 0, 0, 0);
        }
    }

    return 0;
}
