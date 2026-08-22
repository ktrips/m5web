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

// How much of the haiku/poem pipeline runs automatically whenever a new
// photo shows up (M5StickV capture or phone upload), independent of the
// M5StickV設定 card's own auto/preview print mode (see camera_link.h):
//   "none"     - never auto-generate; only the "俳句を作る" button does (default)
//   "generate" - auto-generate and show it in the editable box, no auto-print
//   "print"    - auto-generate and auto-print
// Purely a browser-side concern like the two fields above — this board just
// persists and hands the value back (see data/index.html's autoHaikuFor()).
String autoMode();
void setAutoMode(const String &mode);

}  // namespace Haiku
