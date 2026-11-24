#include "spool.h"
#include "carf_logic.h"
#include "local_config.h"
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr char folder[] = "/carf";
// POSIX paths through the FAT VFS that SD.begin() mounts at /sd. Unlike
// SD.exists(), stat() lets a missing file be told apart from an I/O error.
constexpr char folderVfs[] = "/sd/carf";
constexpr char activeVfs[] = "/sd/carf/current.open";
constexpr size_t batchRows = 24;
constexpr size_t lineCapacity = 896;
static char currentTrip[37] = {};
static size_t activeRows = 0;
const char* spoolStatus = "Starting SD";

static bool folderVerified() {
    struct stat st;
    return stat(folderVfs, &st) == 0 && S_ISDIR(st.st_mode);
}

bool sealBatch() {
    struct stat st;
    bool folderOk = folderVerified();
    errno = 0;
    int rc = stat(activeVfs, &st);
    int err = errno;
    switch (classifyStat(rc, err, folderOk)) {
    case SpoolFile::Unknown:
        // Do not reset the row count or trip: the file may still hold rows.
        spoolStatus = "SD check failed";
        return false;
    case SpoolFile::Present: {
        char id[37];
        makeUuid(id);
        char ready[64];
        snprintf(ready, sizeof(ready), "%s/%s.ndj", folderVfs, id);
        // FAT rename refuses an existing destination, so no separate check.
        if (rename(activeVfs, ready) != 0) {
            spoolStatus = "SD seal failed";
            return false;
        }
        break;
    }
    case SpoolFile::Absent:
        break;
    }
    activeRows = 0;
    currentTrip[0] = 0;
    return true;
}

bool mountSpool() {
    static bool spiStarted = false;
    if (!spiStarted) {
        SPI.begin(18, 38, 23, 4);
        spiStarted = true;
    }
    // Drop a stale mount so a reseated or recovered card is initialised again.
    SD.end();
    if (!SD.begin(4, SPI, 10000000)) {
        spoolStatus = "No FAT32 SD; retrying";
        return false;
    }
    struct stat st;
    errno = 0;
    bool folderOk = stat(folderVfs, &st) == 0
        ? S_ISDIR(st.st_mode)
        : errno == ENOENT && mkdir(folderVfs, 0777) == 0;
    // Recover the last open file after a reset or a storage fault.
    if (!folderOk || !sealBatch()) {
        if (!folderOk) spoolStatus = "SD folder error; retrying";
        SD.end();
        return false;
    }
    spoolStatus = "SD ready";
    return true;
}

void unmountSpool() {
    SD.end();
}

AppendResult appendReading(const Reading& s) {
    if ((activeRows >= batchRows || (activeRows && strcmp(currentTrip, s.trip))) && !sealBatch())
        return AppendResult::StorageFault;
    char line[lineCapacity];
    int n = snprintf(line, sizeof(line), "{\"trip_id\":\"%s\",\"elapsed_ms\":%llu",
                     s.trip, (unsigned long long)s.elapsed);
    if (s.utc > 0) n += snprintf(line+n, sizeof(line)-n, ",\"utc_s\":%lld", (long long)s.utc);
    if (s.rpmQuarter >= 0) n += snprintf(line+n, sizeof(line)-n, ",\"rpm\":%.2f", s.rpmQuarter / 4.0);
    if (s.speed >= 0) n += snprintf(line+n, sizeof(line)-n, ",\"speed_kph\":%d", s.speed);
    if (s.coolant != -1000) n += snprintf(line+n, sizeof(line)-n, ",\"coolant_c\":%d", s.coolant);
    if (s.vehicle != Vehicle::Generic)
        n += snprintf(line+n, sizeof(line)-n, ",\"vehicle\":\"%s\"", vehicleId(s.vehicle));
    if (s.request[0]) {
        n += snprintf(line+n, sizeof(line)-n,
                      ",\"observation\":{\"request\":\"%s\",\"format\":\"%s\",\"elapsed_ms\":%llu,\"reply\":\"",
                      s.request, s.canHeaders ? "can11" : "payload", (unsigned long long)s.observationMs);
        // Encode line breaks as separators. Adapter responses contain ASCII hex and status text.
        for (const char* p = s.response; *p; ++p) {
            if (size_t(n) + 4 >= sizeof(line)) return AppendResult::Rejected;
            char c = *p;
            if (c == '\r' || c == '\n') c = '|';
            if (c < 32 || c > 126 || c == '"' || c == '\\') c = '?';
            line[n++] = c;
        }
        line[n++] = '"';
        line[n++] = '}';
    }
    n += snprintf(line+n, sizeof(line)-n, "}\n");
    if (n <= 0 || n >= (int)sizeof(line)) {
        spoolStatus = "Sample too long; dropped";
        return AppendResult::Rejected;
    }

    // The Arduino File API discards flush/close errors. Check the FAT VFS
    // operations before removing this sample from the in-memory queue.
    int fd = open(activeVfs, O_WRONLY | O_CREAT | O_APPEND, 0666);
    bool written = fd >= 0 && write(fd, line, n) == n && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) written = false;
    if (!written) {
        // Never append again to a file that may end in a torn line. The
        // caller remounts; mountSpool() then seals this file before the
        // sample is retried into a fresh one. A row that did reach the card
        // is replayed with identical values, which the receiver accepts.
        spoolStatus = "SD write failed";
        return AppendResult::StorageFault;
    }
    ++activeRows;
    memcpy(currentTrip, s.trip, sizeof(currentTrip));
    spoolStatus = "Saved to SD";
    return AppendResult::Saved;
}

static String nextPath() {
    File directory = SD.open(folder);
    if (!directory) spoolStatus = "Cannot read SD directory";
    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
        String path = file.path();
        if (!file.isDirectory() && path.endsWith(".ndj")) return path;
    }
    return "";
}

static bool keepForInspection(const String& path) {
    spoolStatus = "Check .hold file on SD";
    Serial.printf("Retaining damaged/rejected batch: %s\n", path.c_str());
    return SD.rename(path, path + ".hold");
}

bool uploadNext() {
    // Each sealed file has at most 24 lines generated by appendReading().
    static uint8_t body[batchRows * lineCapacity];
    String path = nextPath();
    if (path.isEmpty()) return false;
    File file = SD.open(path, FILE_READ);
    if (!file) { spoolStatus = "SD read failed"; return false; }
    size_t length = file.size();
    if (!length || length > sizeof(body)) {
        file.close();
        return keepForInspection(path);
    }
    size_t got = file.read(body, length);
    file.close();
    if (got != length) { spoolStatus = "SD read failed"; return false; }
    // A power cut can tear the final line. Deliver the valid prefix and
    // retain the original file on the card for inspection.
    size_t complete = length;
    while (complete && body[complete - 1] != '\n') --complete;
    if (!complete) return keepForInspection(path);
    uint8_t digest[32];
    if (mbedtls_sha256_ret(body, complete, digest, 0) != 0) return false;
    char expected[65];
    for (size_t i = 0; i < sizeof(digest); ++i)
        snprintf(expected + i*2, 3, "%02x", digest[i]);

    WiFiClient client;
    HTTPClient http;
    String url = CARF_RECEIVER_URL;
    if (!url.startsWith("http://") || !http.begin(client, url)) {
        spoolStatus = "Check LAN receiver URL";
        return false;
    }
    http.setConnectTimeout(1600);
    http.setTimeout(2500);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Authorization", String("Bearer ") + CARF_UPLOAD_TOKEN);
    http.addHeader("Content-Type", "application/x-ndjson");
    int status = http.POST(body, complete);
    bool acknowledged = status == 200 && http.getString() == expected;
    http.end();
    if (!acknowledged) {
        Serial.printf("Upload retained: HTTP %d\n", status);
        if (status == 400 || status == 409 || status == 413 || status == 422)
            return keepForInspection(path);
        spoolStatus = status == 401 || status == 403 ? "Check upload token" : "Waiting for receiver";
        return false;
    }
    if (complete != length) return keepForInspection(path);
    if (!SD.remove(path)) { spoolStatus = "Acked; SD retry"; return false; }
    spoolStatus = "Upload confirmed";
    return true;
}
