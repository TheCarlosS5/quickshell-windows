#include "virtualdesktops.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qwineventnotifier.h>

#include <windows.h>
#include <objbase.h>
#include <shobjidl_core.h>

#include "../core/logcat.hpp"

namespace qs::win32 {

namespace {
QS_LOGGING_CATEGORY(logDesktops, "quickshell.win32.desktops", QtInfoMsg);

constexpr auto KEY_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr auto PARENT_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
// ii-host's keyboard hook lets input with this tag through untouched (host/src/keyboard.cpp).
constexpr ULONG_PTR OWN_INPUT_TAG = 0x11A1D00D;

QByteArray readBinary(HKEY key, const wchar_t* subkey, const wchar_t* value) {
	DWORD size = 0;
	if (RegGetValueW(key, subkey, value, RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
	QByteArray data(static_cast<qsizetype>(size), '\0');
	if (RegGetValueW(key, subkey, value, RRF_RT_REG_BINARY, nullptr, data.data(), &size) != ERROR_SUCCESS) return {};
	data.resize(size);
	return data;
}

QUuid uuidFromBytes(const char* bytes) {
	GUID guid;
	std::memcpy(&guid, bytes, sizeof(guid));
	return {guid};
}

// Presses the keys in order and releases them in reverse, as one SendInput: nothing can be left
// held down halfway.
void sendChord(const QList<quint16>& keys) {
	std::vector<INPUT> inputs;
	auto add = [&](quint16 vk, bool up) {
		INPUT in {};
		in.type = INPUT_KEYBOARD;
		in.ki.wVk = vk;
		in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
		auto extended = vk == VK_LWIN || vk == VK_LEFT || vk == VK_RIGHT;
		in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
		in.ki.dwExtraInfo = OWN_INPUT_TAG;
		inputs.push_back(in);
	};
	for (auto vk: keys) add(vk, false);
	for (auto it = keys.crbegin(); it != keys.crend(); ++it) add(*it, true);
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}
} // namespace

VirtualDesktops* VirtualDesktops::instance() {
	static auto* desktops = new VirtualDesktops(); // NOLINT
	return desktops;
}

VirtualDesktops::VirtualDesktops(QObject* parent): QObject(parent) {
	// CLSID_VirtualDesktopManager (shobjidl_core.h only declares it for newer target versions).
	constexpr CLSID clsid {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
	IVirtualDesktopManager* manager = nullptr;
	if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)))) {
		this->manager = manager;
	} else {
		qCWarning(logDesktops) << "IVirtualDesktopManager unavailable: windows count as on the current desktop";
	}

	this->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	this->notifier = new QWinEventNotifier(this->event, this);
	QObject::connect(this->notifier, &QWinEventNotifier::activated, this, [this]() {
		this->reload();
		this->watch();
	});
	this->reload();
	this->watch();

	this->pacer.setInterval(180);
	QObject::connect(&this->pacer, &QTimer::timeout, this, &VirtualDesktops::step);
}

// Watches the VirtualDesktops key, or, until Explorer creates it, the Explorer key for new subkeys.
void VirtualDesktops::watch() {
	if (this->key) RegCloseKey(static_cast<HKEY>(this->key));
	this->key = nullptr;
	HKEY key = nullptr;
	auto exists = RegOpenKeyExW(HKEY_CURRENT_USER, KEY_PATH, 0, KEY_NOTIFY, &key) == ERROR_SUCCESS;
	if (!exists && RegOpenKeyExW(HKEY_CURRENT_USER, PARENT_PATH, 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) return;
	this->key = key;
	auto filter = exists ? REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET : REG_NOTIFY_CHANGE_NAME;
	RegNotifyChangeKeyValue(key, exists, filter, static_cast<HANDLE>(this->event), TRUE);
}

void VirtualDesktops::reload() {
	QList<QUuid> ids;
	auto list = readBinary(HKEY_CURRENT_USER, KEY_PATH, L"VirtualDesktopIDs");
	if (list.size() % 16 == 0) {
		for (qsizetype i = 0; i < list.size(); i += 16) ids.append(uuidFromBytes(list.constData() + i));
	}

	qsizetype current = 0;
	auto currentId = readBinary(HKEY_CURRENT_USER, KEY_PATH, L"CurrentVirtualDesktop");
	if (currentId.size() == 16) current = std::max<qsizetype>(0, ids.indexOf(uuidFromBytes(currentId.constData())));

	QStringList names;
	for (const auto& id: ids) {
		auto subkey = QString::fromWCharArray(KEY_PATH) + "\\Desktops\\" + id.toString(QUuid::WithBraces).toUpper();
		wchar_t name[256] {};
		DWORD size = sizeof(name);
		auto ok = RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(subkey.utf16()), L"Name", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS; // NOLINT
		names.append(ok ? QString::fromWCharArray(name) : QString());
	}

	if (ids == this->ids && current == this->mCurrent && names == this->names) return;
	this->ids = ids;
	this->names = names;
	this->mCurrent = current;
	qCInfo(logDesktops) << "Desktops:" << this->count() << "current:" << this->mCurrent + 1;
	emit this->changed();
}

qsizetype VirtualDesktops::desktopOf(quintptr hwnd) const {
	if (!this->manager) return 0;
	GUID id {};
	auto* manager = static_cast<IVirtualDesktopManager*>(this->manager);
	if (FAILED(manager->GetWindowDesktopId(reinterpret_cast<HWND>(hwnd), &id)) || id == GUID_NULL) return -1; // NOLINT
	if (this->ids.isEmpty()) return 0;
	return this->ids.indexOf(QUuid(id));
}

bool VirtualDesktops::onOtherDesktop(quintptr hwnd) const {
	if (!this->manager) return false;
	BOOL onCurrent = TRUE;
	auto* manager = static_cast<IVirtualDesktopManager*>(this->manager);
	if (FAILED(manager->IsWindowOnCurrentVirtualDesktop(reinterpret_cast<HWND>(hwnd), &onCurrent))) return false; // NOLINT
	return !onCurrent && this->desktopOf(hwnd) >= 0;
}

void VirtualDesktops::switchTo(qsizetype index) {
	index = std::clamp<qsizetype>(index, 0, this->count() - 1);
	// From where the queued switches will leave us.
	auto from = this->mCurrent;
	for (const auto& chord: this->pending) {
		if (chord.contains(VK_RIGHT)) ++from;
		if (chord.contains(VK_LEFT)) --from;
	}
	for (auto i = from; i < index; ++i) this->pending.append({VK_LWIN, VK_CONTROL, VK_RIGHT});
	for (auto i = from; i > index; --i) this->pending.append({VK_LWIN, VK_CONTROL, VK_LEFT});
	this->step();
}

void VirtualDesktops::create() {
	this->pending.append({VK_LWIN, VK_CONTROL, 'D'});
	this->step();
}

void VirtualDesktops::remove(qsizetype index) {
	if (this->count() <= 1) return;
	this->switchTo(index);
	this->pending.append({VK_LWIN, VK_CONTROL, VK_F4});
	this->step();
}

void VirtualDesktops::step() {
	if (this->pending.isEmpty()) {
		this->pacer.stop();
		return;
	}
	if (this->pacer.isActive() && sender() != &this->pacer) return; // the next one is on its way
	sendChord(this->pending.takeFirst());
	this->pacer.start();
}

} // namespace qs::win32
