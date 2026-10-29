#ifndef HELPER_H
#define HELPER_H
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
using namespace std;

struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch {
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};
inline uint32_t computeCRC32(const char* data, size_t length, uint32_t currentCRC = 0xFFFFFFFF) {
    uint32_t crc = currentCRC;
    for (size_t i = 0; i < length; i++) {
        crc = crc ^ (unsigned char)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

inline uint32_t finalizeCRC32(uint32_t crc) {
    return crc ^ 0xFFFFFFFF;
}

struct TrailerChecker {
    vector<char> buffer;
    uint32_t runningCRC = 0xFFFFFFFF;

    void feedByte(char b) {
        buffer.push_back(b);
        if (buffer.size() > 4) {
            char oldest = buffer[0];
            buffer.erase(buffer.begin());
            runningCRC = computeCRC32(&oldest, 1, runningCRC);
        }
    }

    bool verify() {
        if (buffer.size() < 4) {
            return false;
        }
        uint32_t receivedCRC = 0;
        memcpy(&receivedCRC, buffer.data(), 4);
        return finalizeCRC32(runningCRC) == receivedCRC;
    }
};

#endif