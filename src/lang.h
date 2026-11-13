// Brazilian Portuguese for the console menus.
//
// The firmware ships European Portuguese menu texts (strings/eur_pt/strings.{json,lng}). At boot
// the emulator reads those files from the extracted firmware, replaces the values with its own
// Brazilian Portuguese translations, writes the result under run/<name>/lang/ and maps it over the
// original guest path. The .hsqs image and the extracted firmware are never modified, and no
// firmware text is stored in this repository. The language list then shows "Português (Brasil)".
//
// When the console's saved language (/var/lib/clover/profiles/0/lang.txt) is Portuguese, the
// "SHUTTING DOWN..." splash is redrawn as "DESLIGANDO..." with the emulator's own pixel font.
#pragma once
#include <string>

namespace lang {
void apply_ptbr(const std::string& rootfs, const std::string& rundir, const std::string& datadir);
}
