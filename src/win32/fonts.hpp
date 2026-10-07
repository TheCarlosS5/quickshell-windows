#pragma once

namespace qs::win32 {

// Registers fonts from <exe dir>/fonts and %LOCALAPPDATA%/ii-windows/fonts for this process
// only, plus Linux-style family aliases.
void loadBundledFonts();

// Points Qt's freedesktop icon theme lookup at the bundled themes (illogical-impulse uses
// breeze-plus on top of breeze). II_ICON_THEME overrides the theme name.
void setupIconThemes();

} // namespace qs::win32
