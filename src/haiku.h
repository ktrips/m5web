#pragma once

#include <Arduino.h>

// Storage for the two "俳句設定" fields the browser needs when generating
// and printing a poem (see data/index.html's haikuPromptFor() and
// renderVerticalHaikuCanvas()) — same pattern as openai.h: this board does
// nothing with these values beyond persisting them and handing them back,
// since poem generation (OpenAI) and the vertical-print rendering both
// happen entirely client-side.
namespace Haiku {

void begin();

// "haiku" (五七五, fixed 3 lines) or "poem" (free verse, ~30 characters,
// no fixed line count). Defaults to "haiku" if never set.
String poemType();
void setPoemType(const String &type);

// Author name to print small and vertical in the bottom-left corner of a
// poem printout (see data/index.html). Empty string (the default) means
// no author line is drawn.
String author();
void setAuthor(const String &name);

// Whether the haiku/poem pipeline auto-generates whenever a new photo
// shows up (M5StickV capture or phone upload), independent of the
// 自動化設定 card's own「画像が転送されてきたら自動印刷」setting (see
// camera_link.h's Mode — that only decides whether the *photo* itself
// auto-prints):
//   "none"     - never auto-generate; only the "俳句を作る" button does (default)
//   "generate" - auto-generate and show it in the editable box
// Whether a generated haiku/poem then also auto-prints is the separate
// autoPrint field below — this field alone never auto-prints. Purely a
// browser-side concern like the two fields above — this board just
// persists and hands the value back (see data/index.html's autoHaikuFor()).
String autoMode();
void setAutoMode(const String &mode);

// Whether a generated haiku/poem auto-prints the moment it finishes
// generating — regardless of what triggered the generation (autoMode's
// auto-generate, or a manual "俳句を作る" press). Off (false) by default:
// a generated haiku/poem just sits in the editable box until a manual
// "俳句プリント" press. Edited from the 自動化設定 card (top of 設定タブ,
// data/index.html), not the 俳句設定 card itself — see that card's
// requestHaiku(), which checks this after every successful generation.
bool autoPrint();
void setAutoPrint(bool on);

}  // namespace Haiku
