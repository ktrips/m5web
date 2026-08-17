#pragma once

#include <Arduino.h>

// Wall-clock time via NTP — used only to stamp a small "printed at HH:MM"
// caption onto printouts (see caption.h); nothing else in this firmware
// depends on real time. JST (UTC+9, no DST) is hardcoded since this is a
// Japan-only device.
namespace Clock {

// Starts an NTP sync in the background (configTime() itself doesn't
// block — the actual sync happens asynchronously over the next few
// seconds). Call once after WifiManager reports a successful station
// connection; a no-op (still unsynced) until then, and nowHHMM() just
// returns "" in the meantime — a stamp is nice-to-have, never required.
void begin();

// True once the system clock holds a plausible wall-clock time (i.e. NTP
// has completed at least once since boot).
bool isSynced();

// Current local time as "HH:MM", or "" if not yet synced.
String nowHHMM();

// Current local date as "MM/DD", or "" if not yet synced.
String nowDate();

}  // namespace Clock
