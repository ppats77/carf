#include "obd_frame.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    struct Trace { const char* line; uint8_t pid; size_t bytes; uint32_t raw; };
    const Trace valid[] = {
        {"41 00 BE 3E B8 13", 0x00, 4, 0xBE3EB813},
        {"410C1AF8", 0x0C, 2, 6904},  // 1726 rpm
        {"41 0d 00", 0x0D, 1, 0},     // stopped is a valid reading
        {"41 05 00", 0x05, 1, 0},     // -40 Celsius
    };
    for (const auto& t : valid) {
        uint32_t value = 0;
        assert(decodePidLine(t.line, strlen(t.line), t.pid, t.bytes, value));
        assert(value == t.raw);
    }
    const char* rejected[] = {"", "010C", "41 0D 10", "NO DATA", "7F 01 12",
                              "41 0C FF", "41 0C FF FF 00", "41 0C GG FF",
                              "41 0C FF F", "SEARCHING...", "7E8 04 41 0C 04 00"};
    for (auto line : rejected) {
        uint32_t value = 0;
        assert(!decodePidLine(line, strlen(line), 0x0C, 2, value));
    }
    puts("15 OBD frame cases passed");
}
