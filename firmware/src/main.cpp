#include "capture.h"
#include "carf_logic.h"
#include "local_config.h"
#include "spool.h"
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>

std::atomic<Vehicle> selectedVehicle{Vehicle::Generic};
static Preferences vehiclePrefs;
static bool sdReady;
static bool captureStarted;
// First retry after 1 s, then 2, 4, ... up to 30 s between mount attempts.
static RetryBackoff sdRetry(1000, 30000);
static UsbPowerDebounce usbPower;
static std::atomic<bool> ntpUpdated{false};
static Preferences clockPrefs;
static uint32_t saved;
static Reading latest{};
static const char* lastStorageEvent = "Starting";
static bool clockStarted;
static bool networkConfigured() { return CARF_WIFI_SSID[0] && CARF_UPLOAD_TOKEN[0]; }

static void updateClock() {
    if (!clockStarted && WiFi.status() == WL_CONNECTED) {
        sntp_set_time_sync_notification_cb([](struct timeval*) { ntpUpdated = true; });
        configTime(0, 0, CARF_NTP);
        clockStarted = true;
    }
    if (ntpUpdated.exchange(false)) {
        time_t now = time(nullptr);
        struct tm utc;
        gmtime_r(&now, &utc);
        if (M5.Rtc.isEnabled()) {
            M5.Rtc.setDateTime(&utc);
            if (!clockPrefs.getBool("trusted")) clockPrefs.putBool("trusted", true);
        }
        utcKnown = true;
    }
}

void setup() {
    Serial.begin(115200);
    auto config = M5.config();
    config.internal_spk = false;
    config.internal_mic = false;
    M5.begin(config);
    M5.Display.setTextSize(2);
    M5.Display.setBrightness(75);
    M5.Display.println("CARF trip logger");
    vehiclePrefs.begin("carf-vehicle", false);
    auto vehicle = vehiclePrefs.getUChar("profile", 0);
    if (vehicle < static_cast<unsigned>(Vehicle::Count)) selectedVehicle = static_cast<Vehicle>(vehicle);
    clockPrefs.begin("carf-clock", false);
    utcKnown = clockPrefs.getBool("trusted") && M5.Rtc.isEnabled() &&
               !M5.Rtc.getVoltLow() && time(nullptr) >= 1577836800;
    usbPower = UsbPowerDebounce(M5.Power.getVBUSVoltage() > UsbPowerDebounce::kPresentMv);
    WiFi.persistent(false);
    if (CARF_WIFI_SSID[0]) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(CARF_WIFI_SSID, CARF_WIFI_PASSWORD);
    }
    readings = xQueueCreate(16, sizeof(Reading));
    if (!readings) captureStatus = "Queue alloc failed";
}

// Capture starts after the first successful mount so that every sample has a
// durable destination. Later SD faults only pause draining; the queue and the
// capture task keep their samples until the card is back.
static void startCapture() {
    if (captureStarted || !readings) return;
    captureStarted = xTaskCreate(captureTask, "carf-obd", 6144, nullptr, 1, nullptr) == pdPASS;
    if (!captureStarted) captureStatus = "Capture task failed";
}

static void storageFault() {
    unmountSpool();
    sdReady = false;
    sdRetry.fail(millis());
    Serial.printf("SD fault (%s); remount attempt %lu scheduled\n", spoolStatus,
                  (unsigned long)sdRetry.failures());
}

static void serviceStorage() {
    if (sdReady || !sdRetry.due(millis())) return;
    if (mountSpool()) {
        sdReady = true;
        sdRetry.succeed();
        startCapture();
    } else {
        sdRetry.fail(millis());
    }
}

static void renderStatus() {
    M5.Display.setCursor(0, 0);
    // Pad each fixed-width line to clear old text without flashing the LCD.
    M5.Display.printf("CARF: %-19s\n", vehicleTitle(selectedVehicle.load()));
    M5.Display.printf("%-25s\n", captureStatus.load());
    M5.Display.printf("Samples saved: %-10lu\n", (unsigned long)saved);
    char value[25] = "--";
    if (saved && latest.rpmQuarter >= 0) snprintf(value, sizeof(value), "%.0f", latest.rpmQuarter/4.0);
    M5.Display.printf("RPM: %-19s\n", value);
    M5.Display.printf("Wi-Fi: %-17s\n", WiFi.status() == WL_CONNECTED ? "home" : "offline");
    M5.Display.printf("UTC: %-19s\n", utcKnown ? "set" : "awaiting NTP");
    M5.Display.printf("%-25s\n", spoolStatus);
    M5.Display.printf("%-25s\n", networkConfigured() ? lastStorageEvent : "Set Wi-Fi + upload token");
    M5.Display.printf("%-25s\n", "Left: next vehicle");
    M5.Display.printf("%-25s\n", "Middle: pause/resume");
    M5.Display.printf("%-25s\n", "Right 3s: forget MX+");
}

void loop() {
    static uint32_t lastUI = 0, lastWrite = 0, nextUpload = 0;
    M5.update();
    if (M5.BtnA.wasClicked()) {
        auto next = (static_cast<unsigned>(selectedVehicle.load()) + 1) % static_cast<unsigned>(Vehicle::Count);
        selectedVehicle = static_cast<Vehicle>(next);
        vehiclePrefs.putUChar("profile", next);
    }
    if (M5.BtnC.wasReleaseFor(3000)) forgetPeer = true;
    if (M5.BtnB.wasClicked()) captureEnabled = !captureEnabled.load();
    if (millis() - lastUI >= 500) {
        // Pause only after USB stays away for 15 s, so an engine-crank dip or
        // one invalid PMIC reading does not split the trip. Resume at once.
        switch (usbPower.update(millis(), M5.Power.getVBUSVoltage())) {
        case UsbPowerDebounce::Lost: captureEnabled = false; break;
        case UsbPowerDebounce::Restored: captureEnabled = true; break;
        case UsbPowerDebounce::None: break;
        }
        renderStatus();
        lastUI = millis();
    }

    updateClock();
    serviceStorage();
    Reading sample;
    while (sdReady && readings && xQueuePeek(readings, &sample, 0) == pdTRUE) {
        AppendResult result = appendReading(sample);
        if (result == AppendResult::StorageFault) {
            storageFault();  // The sample stays queued for the remounted card.
            break;
        }
        xQueueReceive(readings, &sample, 0);
        if (result == AppendResult::Saved) {
            latest = sample;
            ++saved;
            lastWrite = millis();
        }
    }
    static uint32_t lastSeal = 0;
    // Short periodic batches let the receiver see driveway samples while
    // the engine is running, and release the final batch after polling stops.
    if (sdReady && lastWrite && (millis() - lastWrite >= 2500 || millis() - lastSeal >= 24000)) {
        if (sealBatch()) { lastWrite = 0; lastSeal = millis(); }
        else storageFault();
    }
    if (sdReady && networkConfigured() && WiFi.status() == WL_CONNECTED &&
        (int32_t)(millis() - nextUpload) >= 0) {
        bool progress = uploadNext();
        if (progress) lastStorageEvent = spoolStatus;
        nextUpload = millis() + (progress ? 100 : 4000);
    }
    delay(15);
}
