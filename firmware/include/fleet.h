#pragma once
#include <cstdint>

enum class Vehicle : uint8_t { Generic, Sienna, X3, Convertible, Prius, Count };

inline const char* vehicleId(Vehicle v) {
    static const char* const ids[] = {"generic", "sienna_2009", "x3_2017_n55", "335i_2007_n54", "prius_2005"};
    return ids[static_cast<unsigned>(v)];
}
inline const char* vehicleTitle(Vehicle v) {
    static const char* const titles[] = {"Generic OBD", "2009 Sienna", "2017 X3 N55", "2007 335i N54", "2005 Prius"};
    return titles[static_cast<unsigned>(v)];
}

struct Probe {
    const char* request;
    const char* tx;        // Null for a standard functional OBD request.
    const char* rx;
    const char* extension;
    unsigned pid;          // Zero for manufacturer-specific requests.
};

inline const Probe* profileProbe(Vehicle v, unsigned index) {
    static const Probe sienna[] = {
        {"0106", nullptr, nullptr, nullptr, 6}, {"0107", nullptr, nullptr, nullptr, 7},
        {"0108", nullptr, nullptr, nullptr, 8}, {"0109", nullptr, nullptr, nullptr, 9},
        {"21D9", "7E0", "7E8", nullptr, 0}
    };
    static const Probe x3[] = {
        {"2257C3", "6F1", "612", "12", 0}, {"22DA12", "6F1", "618", "18", 0}
    };
    static const Probe rail{"0123", nullptr, nullptr, nullptr, 0x23};
    static const Probe battery{"21CE", "7E3", "7EB", nullptr, 0};
    switch (v) {
    case Vehicle::Sienna: return &sienna[index % 5];
    case Vehicle::X3: return &x3[index % 2];
    case Vehicle::Convertible: return &rail;
    case Vehicle::Prius: return &battery;
    default: return nullptr;
    }
}
