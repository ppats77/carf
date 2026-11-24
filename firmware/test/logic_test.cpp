// Host regression tests for carf_logic.h. Build from the repository root:
//   c++ -std=c++11 -Wall -Wextra -Ifirmware/include firmware/test/logic_test.cpp -o /tmp/carf-logic-test
#include "carf_logic.h"
#include <cassert>
#include <cstdio>

static int cases = 0;
#define CHECK(x) do { assert(x); ++cases; } while (0)

static void peerAddress() {
    const uint8_t zero[6] = {};
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t mx[6] = {0x00, 0x04, 0x3E, 0x12, 0x34, 0x56};
    const uint8_t lowByte[6] = {0, 0, 0, 0, 0, 1};
    CHECK(!usablePeerAddress(zero));
    CHECK(!usablePeerAddress(broadcast));
    CHECK(usablePeerAddress(mx));
    CHECK(usablePeerAddress(lowByte));
}

static void peerLatch() {
    const uint8_t zero[6] = {};
    const uint8_t first[6] = {0x00, 0x04, 0x3E, 0x12, 0x34, 0x56};
    const uint8_t second[6] = {0x00, 0x04, 0x3E, 0xAB, 0xCD, 0xEF};
    PeerLatch latch;
    uint8_t out[6] = {};

    // connect() returned before the callback ran: nothing to take yet.
    uint32_t seen = latch.generation;
    CHECK(!latch.takeSince(seen, out));

    // An all-zero open event is ignored and does not advance the generation.
    latch.record(zero);
    CHECK(latch.generation == seen);
    CHECK(!latch.takeSince(seen, out));

    // The callback finally runs: the reader sees the new address.
    latch.record(first);
    CHECK(latch.takeSince(seen, out));
    CHECK(memcmp(out, first, 6) == 0);

    // A stale address from an earlier connection is not re-used for a new one.
    seen = latch.generation;
    CHECK(!latch.takeSince(seen, out));
    latch.record(second);
    CHECK(latch.takeSince(seen, out));
    CHECK(memcmp(out, second, 6) == 0);
}

static void usbDebounce() {
    typedef UsbPowerDebounce U;
    U usb(true, 15000);

    // Engine crank: 3 s dip, power returns, capture never pauses.
    CHECK(usb.update(1000, 5000) == U::None);
    CHECK(usb.update(1500, 3100) == U::None);
    CHECK(usb.update(4000, 2900) == U::None);
    CHECK(usb.update(4500, 5000) == U::None);
    CHECK(usb.present());

    // One invalid AXP2101 reading (0 mV) and an unavailable reading (-1).
    CHECK(usb.update(5000, 0) == U::None);
    CHECK(usb.update(5500, -1) == U::None);
    CHECK(usb.update(6000, 5000) == U::None);

    // The hold restarts after each return of power.
    CHECK(usb.update(10000, 0) == U::None);
    CHECK(usb.update(24999, 0) == U::None);
    CHECK(usb.present());
    CHECK(usb.update(25000, 0) == U::Lost);
    CHECK(!usb.present());
    CHECK(usb.update(40000, 0) == U::None);  // Reported once.

    // Power back: resume at once, reported once.
    CHECK(usb.update(40500, 5100) == U::Restored);
    CHECK(usb.update(41000, 5100) == U::None);

    // Exactly at the 4000 mV threshold counts as absent.
    CHECK(usb.update(50000, U::kPresentMv) == U::None);
    CHECK(usb.update(65000, U::kPresentMv) == U::Lost);

    // Booted on battery: no event until USB appears.
    U battery(false, 15000);
    CHECK(battery.update(0, 0) == U::None);
    CHECK(battery.update(60000, 0) == U::None);
    CHECK(battery.update(61000, 5000) == U::Restored);

    // millis() wraparound during the hold.
    U wrap(true, 15000);
    CHECK(wrap.update(0xFFFFF000u, 0) == U::None);
    CHECK(wrap.update(0xFFFFF000u + 14999u, 0) == U::None);
    CHECK(wrap.update(0xFFFFF000u + 15000u, 0) == U::Lost);
}

static void retryBackoff() {
    RetryBackoff retry(1000, 30000);
    CHECK(retry.due(0));  // First attempt is immediate.
    retry.fail(0);
    CHECK(retry.failures() == 1);
    CHECK(!retry.due(999));
    CHECK(retry.due(1000));
    retry.fail(1000);       // Next wait 2 s.
    CHECK(!retry.due(2999));
    CHECK(retry.due(3000));
    retry.fail(3000);       // 4 s
    retry.fail(7000);       // 8 s
    retry.fail(15000);      // 16 s
    CHECK(!retry.due(30999));
    CHECK(retry.due(31000));
    retry.fail(31000);      // Capped at 30 s.
    CHECK(!retry.due(60999));
    CHECK(retry.due(61000));
    retry.fail(61000);
    CHECK(retry.due(91000));
    CHECK(retry.failures() == 7);
    retry.succeed();
    CHECK(retry.failures() == 0);
    CHECK(retry.due(91001));
    retry.fail(100000);     // Back to 1 s after a success.
    CHECK(retry.due(101000));

    RetryBackoff wrap(1000, 30000);
    wrap.fail(0xFFFFFE00u);
    CHECK(!wrap.due(0xFFFFFFFFu));
    CHECK(wrap.due(0xFFFFFE00u + 1000u));
}

static void sealStat() {
    // A file that stat() finds is sealed.
    CHECK(classifyStat(0, 0, true) == SpoolFile::Present);
    CHECK(classifyStat(0, 0, false) == SpoolFile::Present);
    // Missing file in a verified folder: nothing to seal.
    CHECK(classifyStat(-1, ENOENT, true) == SpoolFile::Absent);
    // ENOENT without a readable folder may mean the mount is gone.
    CHECK(classifyStat(-1, ENOENT, false) == SpoolFile::Unknown);
    // I/O errors must never be reported as "sealed".
    CHECK(classifyStat(-1, EIO, true) == SpoolFile::Unknown);
    CHECK(classifyStat(-1, ENODEV, true) == SpoolFile::Unknown);
    CHECK(classifyStat(-1, 0, true) == SpoolFile::Unknown);
}

int main() {
    peerAddress();
    peerLatch();
    usbDebounce();
    retryBackoff();
    sealStat();
    printf("%d logic cases passed\n", cases);
}
