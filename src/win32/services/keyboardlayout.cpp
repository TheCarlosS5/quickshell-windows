#include "keyboardlayout.hpp"
#include <vector>

#include <windows.h>

namespace qs::win32::keyboardlayout {

namespace {

QString localeName(HKL layout) {
	auto lang = static_cast<LANGID>(reinterpret_cast<ULONG_PTR>(layout) & 0xFFFF); // NOLINT
	wchar_t name[LOCALE_NAME_MAX_LENGTH] {};
	if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) == 0) return {};
	return QString::fromWCharArray(name);
}

QString displayName(const QString& locale) {
	wchar_t name[128] {};
	if (GetLocaleInfoEx(reinterpret_cast<LPCWSTR>(locale.utf16()), LOCALE_SLOCALIZEDDISPLAYNAME, name, 128) == 0) return locale; // NOLINT
	return QString::fromWCharArray(name);
}

} // namespace

KeyboardLayouts::KeyboardLayouts(QObject* parent): QObject(parent) {
	// Windows sends no notification for another app's layout change: a light poll (one cheap
	// call), which only emits when something changed.
	this->timer.setInterval(500);
	QObject::connect(&this->timer, &QTimer::timeout, this, &KeyboardLayouts::poll);
	this->timer.start();
	this->poll();
}

void KeyboardLayouts::poll() {
	QStringList codes;
	auto count = GetKeyboardLayoutList(0, nullptr);
	if (count > 0) {
		std::vector<HKL> layouts(static_cast<size_t>(count));
		count = GetKeyboardLayoutList(count, layouts.data());
		for (int i = 0; i < count; ++i) {
			auto code = localeName(layouts[static_cast<size_t>(i)]);
			if (!code.isEmpty() && !codes.contains(code)) codes.append(code);
		}
	}

	auto* foreground = GetForegroundWindow();
	auto thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
	auto current = localeName(GetKeyboardLayout(thread));
	if (current.isEmpty() && !codes.isEmpty()) current = codes.first();

	if (codes == this->mCodes && current == this->mCurrent) return;
	this->mCodes = codes;
	if (current != this->mCurrent) {
		this->mCurrent = current;
		this->mCurrentName = displayName(current);
	}
	emit this->changed();
}

} // namespace qs::win32::keyboardlayout
