#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>

// ATE0, ATH0 and ATS1: each responding ECU gets one line of hex octets.
// Only accept a complete response to the PID we actually asked for.
inline bool decodePidLine(const char* line, size_t size, uint8_t pid,
                          size_t dataBytes, uint32_t& result) {
    uint8_t frame[6] = {};
    size_t digits = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = line[i];
        if (c == ' ' || c == '\t') continue;
        if (digits >= sizeof(frame) * 2 || !std::isxdigit(c)) return false;
        int nibble = std::isdigit(c) ? c - '0' : std::toupper(c) - 'A' + 10;
        size_t octet = digits++ / 2;
        frame[octet] = (frame[octet] << 4) | nibble;
    }
    if (dataBytes > 4 || digits != (2 + dataBytes) * 2 ||
        frame[0] != 0x41 || frame[1] != pid) return false;
    result = 0;
    for (size_t i = 0; i < dataBytes; ++i) result = (result << 8) | frame[2 + i];
    return true;
}
