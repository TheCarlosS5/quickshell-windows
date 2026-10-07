#pragma once

namespace qs::win32 {

// Registers fonts from <exe dir>/fonts and %LOCALAPPDATA%/ii-windows/fonts for this process
// only, plus Linux-style family aliases.
void loadBundledFonts();

} // namespace qs::win32
