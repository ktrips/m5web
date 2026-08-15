#pragma once

// HTTP routes for the m5web UI and its print API. Serves data/index.html
// (uploaded to LittleFS) plus a small JSON/form API consumed by that page.
namespace WebServer_ {

void begin();
void loop();  // call every iteration of the main loop

}  // namespace WebServer_
