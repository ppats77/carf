#pragma once
// Hardware-independent decisions shared by the firmware and the host tests in
// firmware/test. Keep this header free of Arduino and ESP-IDF types.
#include <cerrno>
#include <cstdint>
#include <cstring>

// An all-zero or broadcast Bluetooth address is never a real adapter.
inline bool usablePeerAddress(const uint8_t* address) {
    bool zero = true, broadcast = true;
    for (int i = 0; i < 6; ++i) {
        zero = zero && address[i] == 0x00;
        broadcast = broadcast && address[i] == 0xFF;
    }
    return !zero && !broadcast;
}

// The Bluetooth task records the peer of each successful SPP open. The
// capture task reads it after connect() returns. BluetoothSerial wakes
// connect() before it runs the custom callback, so the reader compares
// generations instead of trusting whatever address happens to be stored.
// The caller serialises record() and takeSince() with its own lock.
struct PeerLatch {
    uint8_t address[6] = {};
    uint32_t generation = 0;

    void record(const uint8_t* peer) {
        if (!usablePeerAddress(peer)) return;
        memcpy(address, peer, sizeof(address));
        ++generation;
    }

    bool takeSince(uint32_t seen, uint8_t* out) const {
        if (generation == seen) return false;
        memcpy(out, address, sizeof(address));
        return true;
    }
};

// USB power counts as lost only after it stays absent for holdMs. A crank dip
// or a single invalid AXP2101 reading (reported as 0 mV) does not pause capture.
// Power coming back is reported at once.
class UsbPowerDebounce {
public:
    enum Event { None, Lost, Restored };
    static constexpr int kPresentMv = 4000;
    static constexpr uint32_t kDefaultHoldMs = 15000;

    explicit UsbPowerDebounce(bool present = false, uint32_t holdMs = kDefaultHoldMs)
        : present_(present), holdMs_(holdMs) {}

    // vbusMv < 0 means the reading is unavailable and is ignored.
    Event update(uint32_t nowMs, int vbusMv) {
        if (vbusMv < 0) return None;
        if (vbusMv > kPresentMv) {
            absent_ = false;
            if (present_) return None;
            present_ = true;
            return Restored;
        }
        if (!present_) return None;
        if (!absent_) {
            absent_ = true;
            absentSince_ = nowMs;
            return None;
        }
        if (nowMs - absentSince_ < holdMs_) return None;
        present_ = false;
        absent_ = false;
        return Lost;
    }

    bool present() const { return present_; }

private:
    bool present_;
    uint32_t holdMs_;
    bool absent_ = false;
    uint32_t absentSince_ = 0;
};

// Exponential retry schedule on the millis() clock, safe across wraparound.
class RetryBackoff {
public:
    RetryBackoff(uint32_t firstMs, uint32_t maxMs)
        : first_(firstMs), max_(maxMs), delay_(firstMs) {}

    bool due(uint32_t nowMs) const {
        return !waiting_ || static_cast<int32_t>(nowMs - next_) >= 0;
    }
    void fail(uint32_t nowMs) {
        ++failures_;
        waiting_ = true;
        next_ = nowMs + delay_;
        delay_ = delay_ >= max_ / 2 ? max_ : delay_ * 2;
    }
    void succeed() {
        failures_ = 0;
        waiting_ = false;
        delay_ = first_;
    }
    uint32_t failures() const { return failures_; }

private:
    uint32_t first_, max_, delay_;
    uint32_t next_ = 0, failures_ = 0;
    bool waiting_ = false;
};

// stat() returns -1 both for a missing file and for an I/O error. Only call
// the file absent when errno says so and its folder was just verified;
// otherwise the state is unknown and nothing may be reset or claimed sealed.
enum class SpoolFile { Present, Absent, Unknown };

inline SpoolFile classifyStat(int rc, int err, bool folderVerified) {
    if (rc == 0) return SpoolFile::Present;
    if (err == ENOENT && folderVerified) return SpoolFile::Absent;
    return SpoolFile::Unknown;
}
