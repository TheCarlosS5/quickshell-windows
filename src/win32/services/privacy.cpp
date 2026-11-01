#include "privacy.hpp"

#include <string>
#include <thread>

#include <qfileinfo.h>
#include <qtenvironmentvariables.h>
#include <qmetaobject.h>
#include <qpointer.h>

#include <windows.h>

#include "../crash.hpp"

namespace qs::win32::privacy {

namespace {

constexpr const wchar_t* STORE = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore";

ULONGLONG qword(HKEY key, const wchar_t* name) {
	ULONGLONG value = 0;
	DWORD size = sizeof(value);
	if (RegGetValueW(key, nullptr, name, RRF_RT_REG_QWORD, nullptr, &value, &size) != ERROR_SUCCESS) return 0;
	return value;
}

bool inUse(HKEY app) { return qword(app, L"LastUsedTimeStart") != 0 && qword(app, L"LastUsedTimeStop") == 0; }

// "C:#Program Files#Discord#Discord.exe" -> "Discord"; "Microsoft.WindowsSoundRecorder_8wekyb3d8bbwe" -> "WindowsSoundRecorder"
QString friendlyName(const QString& key, bool packaged) {
	if (!packaged) {
		auto path = QString(key).replace('#', '\\');
		auto name = QFileInfo(path).completeBaseName();
		if (!name.isEmpty()) name[0] = name[0].toUpper();
		return name.isEmpty() ? key : name;
	}
	auto family = key.section('_', 0, 0);
	auto name = family.section('.', -1);
	return name.isEmpty() ? key : name;
}

void collect(HKEY parent, bool packaged, QStringList& out) {
	wchar_t name[512];
	for (DWORD i = 0;; ++i) {
		DWORD length = 512;
		if (RegEnumKeyExW(parent, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
		auto key = QString::fromWCharArray(name, static_cast<qsizetype>(length));
		if (packaged && key == "NonPackaged") continue;
		HKEY app = nullptr;
		if (RegOpenKeyExW(parent, name, 0, KEY_READ, &app) != ERROR_SUCCESS) continue;
		if (inUse(app)) {
			auto friendly = friendlyName(key, packaged);
			// ii itself (its region capture, for one) is not news.
			if (!friendly.startsWith("quickshell", Qt::CaseInsensitive) && !friendly.startsWith("ii-", Qt::CaseInsensitive)
			    && !out.contains(friendly))
				out.append(friendly);
		}
		RegCloseKey(app);
	}
}

QStringList appsUsing(HKEY store, const wchar_t* capability) {
	QStringList apps;
	HKEY key = nullptr;
	if (RegOpenKeyExW(store, capability, 0, KEY_READ, &key) != ERROR_SUCCESS) return apps;
	collect(key, true, apps);
	HKEY nonPackaged = nullptr;
	if (RegOpenKeyExW(key, L"NonPackaged", 0, KEY_READ, &nonPackaged) == ERROR_SUCCESS) {
		collect(nonPackaged, false, apps);
		RegCloseKey(nonPackaged);
	}
	RegCloseKey(key);
	return apps;
}

} // namespace

PrivacyMonitor::PrivacyMonitor(QObject* parent): QObject(parent) {
	if (crash::disabled("privacy")) return;
	QPointer<PrivacyMonitor> self(this);
	std::thread([self]() {
		crash::setThreadSubsystem("privacy");
		HKEY store = nullptr;
		// Tests (II_DATA_DIR set): a throwaway key under HKCU instead of Windows' own consent store.
		auto testStore = qEnvironmentVariable("II_PRIVACY_TEST_STORE");
		auto path = !testStore.isEmpty() && !qEnvironmentVariableIsEmpty("II_DATA_DIR") ? testStore.toStdWString() : std::wstring(STORE);
		if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_NOTIFY, &store) != ERROR_SUCCESS) return;
		auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		QStringList lastMic, lastCam, lastLoc;
		bool first = true;
		for (;;) {
			// Armed before reading, so a change between the read and the wait is not missed.
			RegNotifyChangeKeyValue(store, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event, TRUE);
			auto mic = appsUsing(store, L"microphone");
			auto cam = appsUsing(store, L"webcam");
			auto loc = appsUsing(store, L"location");
			if (first || mic != lastMic || cam != lastCam || loc != lastLoc) {
				first = false;
				lastMic = mic;
				lastCam = cam;
				lastLoc = loc;
				if (!self) break;
				QMetaObject::invokeMethod(self, [self, mic, cam, loc]() {
					if (self) self->apply(mic, cam, loc);
				});
			}
			WaitForSingleObject(event, INFINITE);
			Sleep(150); // a burst of writes (start + stop of several apps) is read once
		}
		CloseHandle(event);
		RegCloseKey(store);
	}).detach();
}

void PrivacyMonitor::apply(const QStringList& microphone, const QStringList& camera, const QStringList& location) {
	this->mMicrophone = microphone;
	this->mCamera = camera;
	this->mLocation = location;
	emit this->changed();
}

} // namespace qs::win32::privacy
