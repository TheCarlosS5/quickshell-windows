#pragma once

// Pairing Qt's screens with Windows' monitors (plan 2026-10-31, D2). QScreen::name() is the monitor's friendly name
// in recent Qt ("Mi Monitor"), not the device name (\\.\DISPLAY1) that GetMonitorInfo gives: matching by name found
// nothing, so windows had no screen and the desktop never counted as covered. The HMONITOR is the same on both sides.

#include <qguiapplication.h>
#include <qscreen.h>
#include <qscreen_platform.h>

#include <windows.h>

namespace qs::win32 {

inline HMONITOR monitorOf(QScreen* screen) {
	if (!screen) return nullptr;
	auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
	return native ? native->handle() : nullptr;
}

inline QScreen* screenOf(HMONITOR monitor) {
	if (!monitor) return nullptr;
	for (auto* screen: QGuiApplication::screens()) {
		if (monitorOf(screen) == monitor) return screen;
	}
	return nullptr;
}

} // namespace qs::win32
