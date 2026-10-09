#include "virtualdesktops.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

#include <qelapsedtimer.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qprocess.h>
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
bool sendChord(const QList<quint16>& keys) {
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
	auto sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
	if (sent == inputs.size()) return true;
	if (sent > 0) {
		// Partly sent: release everything (VK 0xE8 first so a lone Win-up doesn't open Start).
		inputs.clear();
		add(0xE8, false);
		add(0xE8, true);
		for (auto it = keys.crbegin(); it != keys.crend(); ++it) add(*it, true);
		SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
	}
	return false;
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
	// Re-arm the (one-shot) notification before reading, so a change in between is not lost.
	QObject::connect(this->notifier, &QWinEventNotifier::activated, this, [this]() {
		this->watch();
		this->reload();
	});
	this->watch();
	this->reload();

	// One check at startup, off the GUI thread's way: exit code 0 = supported.
	auto* probe = new QProcess(this);
	QObject::connect(probe, &QProcess::finished, this, [this, probe](int code, QProcess::ExitStatus status) {
		this->mDirect = status == QProcess::NormalExit && code == 0;
		qCInfo(logDesktops) << "Direct desktop control (ii-shim vdesk):" << (this->mDirect ? "yes" : "no");
		probe->deleteLater();
	});
	QObject::connect(probe, &QProcess::errorOccurred, probe, &QObject::deleteLater);
	probe->start("ii-shim", {"vdesk", "probe"});

	this->pacer.setInterval(120);
	QObject::connect(&this->pacer, &QTimer::timeout, this, &VirtualDesktops::step);
	// Each shortcut sent waits for Windows to show its effect before the next one.
	QObject::connect(this, &VirtualDesktops::changed, this, [this]() {
		if (this->inFlight) {
			this->inFlight = false;
			this->pacer.start();
		}
	});
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
	RegNotifyChangeKeyValue(key, exists, filter | REG_NOTIFY_THREAD_AGNOSTIC, static_cast<HANDLE>(this->event), TRUE);
}

void VirtualDesktops::reload() {
	QList<QUuid> ids;
	auto list = readBinary(HKEY_CURRENT_USER, KEY_PATH, L"VirtualDesktopIDs");
	if (list.size() % 16 == 0) {
		for (qsizetype i = 0; i < list.size(); i += 16) ids.append(uuidFromBytes(list.constData() + i));
	}

	qsizetype current = 0;
	auto currentId = readBinary(HKEY_CURRENT_USER, KEY_PATH, L"CurrentVirtualDesktop");
	DWORD session = 0;
	if (currentId.size() != 16 && ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
		// Some builds keep the current desktop per logon session.
		auto path = QString::fromWCharArray(PARENT_PATH) + QString("\\SessionInfo\\%1\\VirtualDesktops").arg(session);
		currentId = readBinary(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), L"CurrentVirtualDesktop"); // NOLINT
	}
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
	if (!this->manager) return -1;
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

bool VirtualDesktops::runVdesk(const QStringList& args) {
	return QProcess::startDetached("ii-shim", QStringList {"vdesk"} + args);
}

bool VirtualDesktops::moveWindow(quintptr hwnd, qsizetype index) {
	if (!this->mDirect || index < 0 || index >= this->count()) return false;
	return runVdesk({"move", QString::number(hwnd), QString::number(index + 1)});
}

void VirtualDesktops::switchTo(qsizetype index) {
	index = std::clamp<qsizetype>(index, 0, this->count() - 1);
	if (this->mDirect && index != this->mCurrent && runVdesk({"switch", QString::number(index + 1)})) {
		this->clear();
		return;
	}
	this->target = index;
	this->closing = QUuid();
	this->returnTo = QUuid();
	this->step();
}

void VirtualDesktops::create() {
	// One at a time: a second request before Windows made the first one is the same request.
	this->creating = true;
	this->target = -1;
	this->closing = QUuid();
	this->returnTo = QUuid();
	this->step();
}

void VirtualDesktops::remove(qsizetype index) {
	if (this->count() <= 1 || index < 0 || index >= this->ids.size()) return;
	if (this->mDirect && runVdesk({"remove", QString::number(index + 1)})) return;
	// By id: indexes shift if desktops change meanwhile. Like Task View, closing another desktop
	// leaves the user where they were (Windows only closes the current one: go there and back).
	this->closing = this->ids.at(index);
	this->returnTo = index == this->mCurrent ? QUuid() : this->ids.value(this->mCurrent);
	this->target = index;
	this->step();
}

void VirtualDesktops::clear() {
	this->inFlight = false;
	this->creating = false;
	this->target = -1;
	this->closing = QUuid();
	this->returnTo = QUuid();
	this->pacer.stop();
}

// Sends the next shortcut once Windows showed the previous one's effect (or gave up on it).
void VirtualDesktops::step() {
	auto idle = !this->creating && this->target < 0 && this->closing.isNull() && this->returnTo.isNull();
	if (idle) {
		this->pacer.stop();
		return;
	}
	if (this->inFlight && this->sentAt.elapsed() < 1500) {
		this->pacer.start(); // still waiting for Windows
		return;
	}
	if (this->inFlight) {
		// Nothing happened: an elevated app in front can block injected input. Give up.
		qCWarning(logDesktops) << "Windows did not take the desktop shortcut; giving up";
		this->clear();
		return;
	}
	// Keys the user holds would mix into the shortcut (and their release would be stolen).
	for (int vk: {VK_LWIN, VK_RWIN, VK_CONTROL, VK_MENU, VK_SHIFT}) {
		if (GetAsyncKeyState(vk) & 0x8000) {
			this->pacer.start();
			return;
		}
	}

	QList<quint16> chord;
	if (this->creating) {
		this->creating = false;
		chord = {VK_LWIN, VK_CONTROL, 'D'};
	} else if (this->target >= 0 && this->target != this->mCurrent && this->target < this->count()) {
		chord = {VK_LWIN, VK_CONTROL, static_cast<quint16>(this->target > this->mCurrent ? VK_RIGHT : VK_LEFT)};
	} else if (!this->closing.isNull() && this->ids.value(this->mCurrent) == this->closing) {
		this->closing = QUuid();
		this->target = -1;
		chord = {VK_LWIN, VK_CONTROL, VK_F4};
	} else if (!this->closing.isNull() && this->ids.contains(this->closing)) {
		// The desktop to close moved (another one was closed meanwhile): follow it.
		this->target = this->ids.indexOf(this->closing);
		this->pacer.start();
		return;
	} else if (!this->returnTo.isNull() && this->ids.contains(this->returnTo) && this->ids.indexOf(this->returnTo) != this->mCurrent) {
		// Closed: back to where the user was (its index may have shifted).
		this->target = this->ids.indexOf(this->returnTo);
		this->returnTo = QUuid();
		this->pacer.start();
		return;
	} else {
		// Arrived, or the desktop to close is already gone: done.
		this->clear();
		return;
	}
	if (!sendChord(chord)) {
		qCWarning(logDesktops) << "SendInput failed; desktop request dropped";
		this->clear();
		return;
	}
	this->inFlight = true;
	this->sentAt.start();
	this->pacer.start();
}

} // namespace qs::win32
