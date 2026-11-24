#include "capture.h"
#include "carf_logic.h"
#include "local_config.h"
#include "obd_frame.h"
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <esp_timer.h>

QueueHandle_t readings;
std::atomic<bool> captureEnabled{true};
std::atomic<bool> forgetPeer{false};
std::atomic<bool> utcKnown{false};
std::atomic<const char*> captureStatus{"Waiting for SD"};
static BluetoothSerial obd;
static portMUX_TYPE addressLock = portMUX_INITIALIZER_UNLOCKED;
static PeerLatch openedPeer;  // Guarded by addressLock.

void makeUuid(char (&uuid)[37]) {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 15) | 0x40;
    b[8] = (b[8] & 63) | 0x80;
    snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Both BluetoothSerial connect() paths request ESP_SPP_SEC_AUTHENTICATE |
// ESP_SPP_SEC_ENCRYPT, so a successful client open is an authenticated link.
static void onBluetoothEvent(esp_spp_cb_event_t event, esp_spp_cb_param_t* info) {
    if (event == ESP_SPP_OPEN_EVT && info->open.status == ESP_SPP_SUCCESS) {
        portENTER_CRITICAL(&addressLock);
        openedPeer.record(info->open.rem_bda);
        portEXIT_CRITICAL(&addressLock);
    }
}

static uint32_t openedGeneration() {
    portENTER_CRITICAL(&addressLock);
    uint32_t generation = openedPeer.generation;
    portEXIT_CRITICAL(&addressLock);
    return generation;
}

// connect() can return before onBluetoothEvent() has run, so wait briefly for
// an open newer than `seen`. Save it before any vehicle check, because the
// MX+ only stays discoverable for two minutes after its button is pressed.
static bool rememberOpenedPeer(Preferences& pairing, uint32_t seen, uint8_t (&address)[6]) {
    uint8_t opened[6];
    bool fresh = false;
    for (uint32_t since = millis(); !fresh && millis() - since < 1000; delay(10)) {
        portENTER_CRITICAL(&addressLock);
        fresh = openedPeer.takeSince(seen, opened);
        portEXIT_CRITICAL(&addressLock);
    }
    if (!fresh || !usablePeerAddress(opened) ||
        pairing.putBytes("peer", opened, sizeof(opened)) != sizeof(opened)) {
        Serial.printf("MX+ address not saved (open event %s)\n", fresh ? "invalid" : "missing");
        return false;
    }
    memcpy(address, opened, sizeof(address));
    return true;
}

// Never drop a measured sample: wait for the loop task to drain the queue,
// for example while the SD card is remounted.
static void enqueue(const Reading& sample) {
    while (xQueueSend(readings, &sample, pdMS_TO_TICKS(1000)) != pdTRUE)
        captureStatus = "Waiting for SD";
}

// A response is complete only at the ELM prompt. Reconnect after timeout so a
// late reply cannot be attributed to the next command.
static bool exchange(const char* command, String& reply, uint32_t timeoutMs) {
    reply = "";
    obd.print(command);
    obd.write('\r');
    uint32_t since = millis();
    while (millis() - since < timeoutMs && captureEnabled && !forgetPeer && obd.hasClient()) {
        while (obd.available()) {
            int c = obd.read();
            if (c == '>') return true;
            if (c >= 0) {
                if (reply.length() >= 1024 || !reply.concat((char)c)) return false;
            }
        }
        delay(5);
    }
    return false;
}

static bool responseValue(const String& reply, uint8_t pid, int bytes, uint32_t& value) {
    bool found = false;
    const char* begin = reply.c_str();
    for (const char* p = begin; ; ++p) {
        if (*p && *p != '\r' && *p != '\n') continue;
        uint32_t decoded;
        if (decodePidLine(begin, p - begin, pid, bytes, decoded)) {
            if (pid % 32 != 0) { value = decoded; return true; }
            // Supported-PID bitmaps may come from several ECUs.
            value |= decoded;
            found = true;
        }
        if (!*p) break;
        begin = p + 1;
    }
    return found;
}

static bool initializeAdapter(String& reply, uint32_t& supported) {
    for (const char* command : {"ATD", "ATE0", "ATL0", "ATH0", "ATS1", "ATSP0"}) {
        if (!exchange(command, reply, 2500) || reply.indexOf("OK") < 0) return false;
    }
    return exchange("0100", reply, 15000) && responseValue(reply, 0, 4, supported);
}

static bool poll(uint32_t supported, uint8_t pid, int bytes, int& result, String& reply) {
    result = -1;
    if (!(supported & (1UL << (32 - pid)))) return true;
    char request[5];
    snprintf(request, sizeof(request), "01%02X", pid);
    if (!exchange(request, reply, 3000)) return false;
    uint32_t raw;
    if (responseValue(reply, pid, bytes, raw)) result = raw;
    return true;
}

// Bitmap pages in the header-on probe response are classic CAN single frames.
static uint32_t bitmap(const String& reply, unsigned page, bool headers) {
    uint32_t combined = 0;
    if (!headers) {
        responseValue(reply, page, 4, combined);
        return combined;
    }
    const char* start = reply.c_str();
    for (const char* end = start; ; ++end) {
        if (*end && *end != '\r' && *end != '\n') continue;
        unsigned id, pci, service, pid, a, b, c, d;
        char line[64] = {};
        size_t length = end - start;
        if (length < sizeof(line)) memcpy(line, start, length);
        if (sscanf(line, "%3x %2x %2x %2x %2x %2x %2x %2x",
                   &id, &pci, &service, &pid, &a, &b, &c, &d) == 8 &&
            id >= 0x7E8 && id <= 0x7EF && pci == 6 && service == 0x41 && pid == page)
            combined |= (a << 24) | (b << 16) | (c << 8) | d;
        if (!*end) break;
        start = end + 1;
    }
    return combined;
}

static bool adapterSetting(const String& command, String& reply) {
    if (!exchange(command.c_str(), reply, 2000)) return false;
    reply.trim();
    return reply == "OK";
}

// Restore functional addressing before returning to the baseline poller.
// A failed exchange ends the session. ATD resets volatile settings on reconnect.
static bool observe(const Probe& probe, bool can11, Reading& sample, String& reply) {
    if (probe.tx && !can11) return true;
    if (can11 && !adapterSetting("ATH1", reply)) return false;
    if (probe.tx) {
        if (!adapterSetting(String("ATSH") + probe.tx, reply) ||
            !adapterSetting(String("ATCRA") + probe.rx, reply)) return false;
        if (probe.extension && (!adapterSetting(String("STCAF 1,") + probe.extension, reply) ||
                                !adapterSetting(String("STFFCA ") + probe.rx + ",7FF", reply))) return false;
    }
    bool complete = exchange(probe.request, reply, 3000);
    strlcpy(sample.request, probe.request, sizeof(sample.request));
    sample.canHeaders = can11;
    if (!complete) {
        strlcpy(sample.response, "TIMEOUT OR INCOMPLETE", sizeof(sample.response));
        return false;
    }
    reply.trim();
    strlcpy(sample.response, reply.length() < sizeof(sample.response) ? reply.c_str() : "REPLY TOO LONG",
            sizeof(sample.response));
    if (probe.extension && (!adapterSetting("STCAF 0", reply) || !adapterSetting("STFA", reply))) return false;
    if (probe.tx && (!adapterSetting("ATSH7DF", reply) || !adapterSetting("ATCRA", reply))) return false;
    return !can11 || adapterSetting("ATH0", reply);
}

struct FleetPoller {
    unsigned page = 0;
    unsigned index = 0;
    uint32_t masks[8] = {};
    bool can11 = false;

    bool next(Reading& sample, String& reply) {
        sample.request[0] = 0;
        if (sample.vehicle == Vehicle::Generic) return true;
        if (page < 8) {
            char command[5];
            snprintf(command, sizeof(command), "01%02X", page * 32);
            Probe discovery{command, nullptr, nullptr, nullptr, 0};
            if (!observe(discovery, can11, sample, reply)) return false;
            masks[page] = bitmap(String(sample.response), page * 32, can11);
            page = (masks[page] & 1) ? page + 1 : 8;
            return true;
        }
        auto probe = profileProbe(sample.vehicle, index++);
        if (!probe) return true;
        unsigned pid = probe->pid;
        if (pid && !(masks[(pid-1)/32] & (1UL << (31-(pid-1)%32)))) return true;
        return observe(*probe, can11, sample, reply);
    }
};

void captureTask(void*) {
    obd.enableSSP();
    obd.register_callback(onBluetoothEvent);
    if (!obd.begin("CARF Core2", true)) {
        captureStatus = "Bluetooth init failed";
        vTaskDelete(nullptr);
        return;
    }
    Preferences pairing;
    pairing.begin("carf-obd", false);
    uint8_t address[6] = {};
    bool remembered = pairing.getBytesLength("peer") == sizeof(address) &&
                      pairing.getBytes("peer", address, sizeof(address)) == sizeof(address);
    String reply;
    reply.reserve(512);
    for (;;) {
        if (forgetPeer.exchange(false)) {
            if (remembered) {
                obd.unpairDevice(address);
                pairing.remove("peer");
                remembered = false;
            }
            captureStatus = "Adapter forgotten";
            delay(1000);
        }
        if (!captureEnabled) {
            captureStatus = "Capture paused";
            delay(250);
            continue;
        }
        captureStatus = remembered ? "Connecting to MX+" : "Pairing: press MX+ button";
        uint32_t seen = openedGeneration();
        bool connected = remembered ? obd.connect(address) : obd.connect(String(CARF_OBD_NAME));
        if (connected && !remembered) remembered = rememberOpenedPeer(pairing, seen, address);
        uint32_t supported = 0;
        if (connected) captureStatus = "Checking supported PIDs";
        if (connected && initializeAdapter(reply, supported) &&
            (supported & ((1UL << 20) | (1UL << 19) | (1UL << 27)))) {
            Reading sample{};
            sample.vehicle = selectedVehicle.load();
            FleetPoller fleet;
            if (sample.vehicle != Vehicle::Generic) {
                if (!exchange("ATDPN", reply, 2000)) { obd.disconnect(); delay(1000); continue; }
                reply.trim();
                fleet.can11 = reply == "A6" || reply == "6" || reply == "A8" || reply == "8";
            }
            makeUuid(sample.trip);
            int64_t firstMs = -1;
            uint32_t lastValid = millis();
            captureStatus = "Reading vehicle";
            while (captureEnabled && !forgetPeer && selectedVehicle.load() == sample.vehicle &&
                   obd.hasClient() && millis() - lastValid < 15000) {
                int tempRaw;
                if (!poll(supported, 0x0C, 2, sample.rpmQuarter, reply) ||
                    !poll(supported, 0x0D, 1, sample.speed, reply) ||
                    !poll(supported, 0x05, 1, tempRaw, reply)) break;
                if (sample.rpmQuarter >= 0 || sample.speed >= 0 || tempRaw >= 0) {
                    sample.coolant = tempRaw < 0 ? -1000 : tempRaw - 40;
                    int64_t nowMs = esp_timer_get_time() / 1000;
                    if (firstMs < 0) firstMs = nowMs;
                    sample.elapsed = nowMs - firstMs;
                    sample.utc = utcKnown ? time(nullptr) : 0;
                    bool ready = fleet.next(sample, reply);
                    sample.observationMs = esp_timer_get_time() / 1000 - firstMs;
                    if (selectedVehicle.load() != sample.vehicle) break;
                    enqueue(sample);
                    if (!ready) break;
                    captureStatus = "Reading vehicle";
                    lastValid = millis();
                }
                delay(1000);
            }
        } else if (connected) {
            captureStatus = "No supported ECU reply";
        }
        if (obd.hasClient()) obd.disconnect();
        delay(3000);
    }
}
