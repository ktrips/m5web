#pragma once

// CamS3's own web UI + JSON API — the direct-connect-mode counterpart of
// m5web's src/web_server.*, plus a small always-on route set (status,
// mode switch, shutter) shared with kViaAtom mode. See web_server.cpp's
// top-of-file comment for the route list and which routes are
// mode-conditional.
namespace WebServer_ {

void begin();
void loop();  // call every iteration of the main loop

}  // namespace WebServer_
