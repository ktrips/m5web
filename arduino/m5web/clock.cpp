#include "clock.h"

#include <time.h>

namespace Clock {

namespace {

constexpr long kGmtOffsetSec = 9 * 3600;    // JST, UTC+9
constexpr int kDaylightOffsetSec = 0;       // Japan doesn't observe DST
constexpr time_t kPlausibleEpoch = 1600000000;  // ~2020-09-13; below this means "not synced yet"

}  // namespace

void begin() { configTime(kGmtOffsetSec, kDaylightOffsetSec, "ntp.nict.jp", "pool.ntp.org"); }

bool isSynced() { return time(nullptr) >= kPlausibleEpoch; }

String nowHHMM() {
    if (!isSynced()) return "";
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    return String(buf);
}

String nowDateTime() {
    if (!isSynced()) return "";
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
             timeinfo.tm_min);
    return String(buf);
}

}  // namespace Clock
