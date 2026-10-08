#include "tray.hpp"
#include <cstddef>
#include <cstring>
#include <functional>
#include <thread>

#include <qbuffer.h>
#include <qbytearray.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qurl.h>
#include <quuid.h>
#include <qwineventnotifier.h>

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include "../../core/logcat.hpp"

namespace qs::win32::tray {

QS_LOGGING_CATEGORY(logTray, "quickshell.win32.tray", QtInfoMsg);

QString Status::toString(Status::Enum status) {
	switch (status) {
	case Passive: return "Passive";
	case NeedsAttention: return "NeedsAttention";
	default: return "Active";
	}
}

QString Category::toString(Category::Enum category) {
	switch (category) {
	case Hardware: return "Hardware";
	case SystemServices: return "SystemServices";
	case Communications: return "Communications";
	default: return "ApplicationStatus";
	}
}

namespace {

// What Shell_NotifyIcon sends to the tray window (WM_COPYDATA, dwData 1). Handles are 32-bit
// so 32- and 64-bit processes share one layout.
struct NotifyIconData32 {
	DWORD cbSize;
	DWORD hWnd;
	UINT uID;
	UINT uFlags;
	UINT uCallbackMessage;
	DWORD hIcon;
	WCHAR szTip[128];
	DWORD dwState;
	DWORD dwStateMask;
	WCHAR szInfo[256];
	UINT uVersion;
	WCHAR szInfoTitle[64];
	DWORD dwInfoFlags;
	GUID guidItem;
	DWORD hBalloonIcon;
};

struct TrayMessage {
	DWORD signature;
	DWORD message;
	NotifyIconData32 nid;
};

constexpr ULONG_PTR COPYDATA_TRAY = 1;
constexpr UINT_PTR TIMER_CHECK = 1;
constexpr UINT_PTR TIMER_RAISE = 2;

QString fromFixed(const WCHAR* text, size_t capacity) {
	size_t length = 0;
	while (length < capacity && text[length] != 0) ++length; // NOLINT
	return QString::fromWCharArray(text, static_cast<qsizetype>(length));
}

QString executableOf(DWORD pid) {
	QString path;
	if (auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
		wchar_t buffer[MAX_PATH * 2];
		DWORD size = MAX_PATH * 2;
		if (QueryFullProcessImageNameW(process, 0, buffer, &size)) path = QString::fromWCharArray(buffer, size);
		CloseHandle(process);
	}
	return path;
}

// Saves the icon as a PNG named by its content (animated icons reuse files) and returns its URL.
QString saveIcon(HICON icon) {
	if (!icon) return {};
	auto image = QImage::fromHICON(icon);
	if (image.isNull()) return {};

	QByteArray png;
	QBuffer buffer(&png);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");

	static const auto dir = []() {
		auto path = QDir(QDir::tempPath()).filePath("ii-windows/tray");
		QDir(path).removeRecursively(); // icons of a previous run
		QDir().mkpath(path);
		return path;
	}();
	auto name = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex()) + ".png";
	auto path = QDir(dir).filePath(name);
	if (!QFile::exists(path)) {
		QFile file(path);
		if (file.open(QFile::WriteOnly)) file.write(png);
	}
	return QUrl::fromLocalFile(path).toString();
}

// --- Which icons Windows shows on the taskbar ---------------------------------------------

// HKCU\Control Panel\NotifyIconSettings has one key per icon Explorer has seen: its executable
// (with a known-folder GUID instead of e.g. "C:\Program Files"), its uID or GUID, and
// IsPromoted=1 when it is shown on the taskbar instead of behind the arrow. Read only.
class Promotions: public QObject {
public:
	explicit Promotions(QObject* parent): QObject(parent) {
		this->reload();
		RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_NOTIFY | KEY_READ, &this->key);
		if (!this->key) return;
		this->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		this->notifier = new QWinEventNotifier(this->event, this);
		QObject::connect(this->notifier, &QWinEventNotifier::activated, this, [this]() {
			this->reload();
			this->watch();
			if (this->changed) this->changed();
		});
		this->watch();
	}

	~Promotions() override {
		if (this->key) RegCloseKey(this->key);
		if (this->event) CloseHandle(this->event);
	}
	Promotions(const Promotions&) = delete;
	Promotions& operator=(const Promotions&) = delete;

	[[nodiscard]] bool isPromoted(const QString& exe, quint32 uid, const QString& guid) const {
		auto path = exe.toLower();
		for (const auto& entry: this->entries) {
			if (entry.exe != path) continue;
			if (!guid.isEmpty() ? entry.guid == guid.toLower() : (entry.guid.isEmpty() && entry.uid == uid)) {
				return entry.promoted;
			}
		}
		return false; // Windows hides icons it has not been told to show
	}

	std::function<void()> changed;

private:
	struct Entry {
		QString exe;
		QString guid;
		quint32 uid = 0;
		bool promoted = false;
	};

	void watch() {
		RegNotifyChangeKeyValue(this->key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, this->event, TRUE);
	}

	// "{6D809377-...}\App\app.exe" -> "c:\program files\app\app.exe"
	static QString expandKnownFolder(const QString& path) {
		if (!path.startsWith('{')) return path.toLower();
		auto end = path.indexOf('}');
		if (end < 0) return path.toLower();
		GUID id {};
		if (FAILED(IIDFromString(reinterpret_cast<LPCOLESTR>(path.left(end + 1).utf16()), &id))) return path.toLower(); // NOLINT
		PWSTR folder = nullptr;
		QString result = path;
		if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder))) {
			result = QString::fromWCharArray(folder) + path.mid(end + 1);
		}
		CoTaskMemFree(folder);
		return result.toLower();
	}

	void reload() {
		this->entries.clear();
		HKEY root = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
		wchar_t name[64];
		for (DWORD i = 0;; ++i) {
			DWORD nameLength = 64;
			if (RegEnumKeyExW(root, i, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
			Entry entry;
			wchar_t text[MAX_PATH * 2];
			DWORD size = sizeof(text);
			if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS) continue;
			entry.exe = expandKnownFolder(QString::fromWCharArray(text));
			size = sizeof(text);
			if (RegGetValueW(root, name, L"IconGuid", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS) {
				entry.guid = QString::fromWCharArray(text).toLower();
			}
			DWORD value = 0;
			size = sizeof(value);
			if (RegGetValueW(root, name, L"UID", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) entry.uid = value;
			value = 0;
			size = sizeof(value);
			if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
				entry.promoted = value != 0;
			}
			this->entries.append(entry);
		}
		RegCloseKey(root);
	}

	HKEY key = nullptr;
	HANDLE event = nullptr;
	QWinEventNotifier* notifier = nullptr;
	QList<Entry> entries;
};

// --- GUI side ---------------------------------------------------------------------------

class Backend: public QObject {
public:
	static Backend* instance() {
		static auto* backend = new Backend(); // NOLINT
		return backend;
	}

	ObjectModel<SystemTrayItem> items {this};

	void update(const IconUpdate& update) {
		auto* item = this->all.value(update.key);

		if (update.kind == IconUpdate::Remove) {
			if (!item) return;
			this->all.remove(update.key);
			this->items.removeObject(item);
			item->deleteLater();
			return;
		}

		if (!item) {
			auto info = QFileInfo(update.exe);
			auto id = info.completeBaseName().toLower();
			if (id.isEmpty()) id = "unknown";
			for (const auto* other: this->all) {
				if (other->id() == id) {
					id += "-" + QString::number(update.uid);
					break;
				}
			}
			item = new SystemTrayItem(update.key, id, info.completeBaseName(), this);
			this->all.insert(update.key, item);
		}

		item->apply(update);
		if (isShellSystemIcon(item)) return; // never listed
		item->setPromoted(this->promotions->isPromoted(item->exe(), item->iconUid(), item->guid()));
		auto shown = this->items.valueList().contains(item);
		if (item->isHidden() && shown) this->items.removeObject(item);
		else if (!item->isHidden() && !shown) this->items.insertObject(item);
	}

private:
	Backend();

	// Explorer's own icons (network, volume, power, microphone/location in use...): the shell
	// has its own indicators for those. "Safely remove hardware" is kept.
	static bool isShellSystemIcon(const SystemTrayItem* item) {
		if (!item->exe().endsWith("\\explorer.exe", Qt::CaseInsensitive)) return false;
		static const QString hotplug = "{7820ae78-23e3-4229-82c1-e41cb67d5b9c}";
		return item->guid().isEmpty() || item->guid().compare(hotplug, Qt::CaseInsensitive) != 0;
	}

	QHash<QString, SystemTrayItem*> all;
	Promotions* promotions = nullptr;
};

// --- tray thread ------------------------------------------------------------------------

class TrayHost {
public:
	static void start() {
		std::thread([]() { TrayHost().run(); }).detach();
	}

private:
	// "Shell_TrayWnd" exists only while Explorer's own tray is settled: Explorer finds its taskbar
	// by class while it starts, and must not find ours then (its taskbar would stay blank).
	HWND tray = nullptr;
	// Always-there helper window: timers, TaskbarCreated and Explorer-exit notices.
	HWND watcher = nullptr;
	HWND explorerTray = nullptr;
	ULONGLONG explorerSince = 0; // when the current Explorer tray was first seen
	HANDLE explorerWait = nullptr;
	HANDLE explorerProcess = nullptr;
	UINT taskbarCreated = 0;
	ULONGLONG ignoreAnnounceUntil = 0;
	ULONGLONG lastClaim = 0;
	QHash<QString, HWND> owners;
	QHash<QString, quint32> versions;

	static constexpr UINT WM_EXPLORER_EXITED = WM_APP + 1;
	static constexpr ULONGLONG SETTLE_MS = 6000;

	static TrayHost*& current() {
		static TrayHost* host = nullptr;
		return host;
	}

	void run() {
		current() = this;
		this->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

		WNDCLASSW tray {};
		tray.lpfnWndProc = TrayHost::trayProc;
		tray.hInstance = GetModuleHandleW(nullptr);
		tray.lpszClassName = L"Shell_TrayWnd";
		RegisterClassW(&tray);

		WNDCLASSW watcher {};
		watcher.lpfnWndProc = TrayHost::watcherProc;
		watcher.hInstance = GetModuleHandleW(nullptr);
		watcher.lpszClassName = L"IiShellTrayWatcher";
		RegisterClassW(&watcher);
		// Top-level (hidden): message-only windows don't get the TaskbarCreated broadcast.
		this->watcher = CreateWindowExW(WS_EX_TOOLWINDOW, watcher.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, watcher.hInstance, nullptr);
		ChangeWindowMessageFilterEx(this->watcher, this->taskbarCreated, MSGFLT_ALLOW, nullptr);

		this->check();
		SetTimer(this->watcher, TIMER_CHECK, 1000, nullptr);
		// Explorer re-raises its taskbar on foreground changes: check right after each one.
		SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, TrayHost::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	void createTray() {
		this->tray = CreateWindowExW(
		    WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		    L"Shell_TrayWnd",
		    L"",
		    WS_POPUP,
		    0,
		    0,
		    0,
		    0,
		    nullptr,
		    nullptr,
		    GetModuleHandleW(nullptr),
		    nullptr
		);
		if (!this->tray) {
			qCWarning(logTray) << "Could not create the tray window:" << GetLastError();
			return;
		}
		ChangeWindowMessageFilterEx(this->tray, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
		this->claim();
		qCInfo(logTray) << "Tray ready";
	}

	void stepAside(const char* why) {
		if (!this->tray) return;
		DestroyWindow(this->tray);
		this->tray = nullptr;
		qCInfo(logTray) << "Tray stepped aside:" << why;
	}

	// Be the Shell_TrayWnd that FindWindow returns, then ask every app to (re)announce its icons.
	void raise() const {
		SetWindowPos(this->tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	// Takes the tray and asks every app to add its icons again (only when ours is new).
	void claim() {
		this->raise();
		this->lastClaim = GetTickCount64();
		this->ignoreAnnounceUntil = this->lastClaim + 3000;
		SendNotifyMessageW(HWND_BROADCAST, this->taskbarCreated, 0, 0);
	}

	HWND findExplorerTray() const {
		HWND candidate = nullptr;
		while ((candidate = FindWindowExW(nullptr, candidate, L"Shell_TrayWnd", nullptr))) {
			if (candidate != this->tray) return candidate;
		}
		return nullptr;
	}

	static ULONGLONG processAgeMs(DWORD pid) {
		ULONGLONG age = 0;
		if (auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
			FILETIME created {};
			FILETIME exited {};
			FILETIME kernel {};
			FILETIME user {};
			FILETIME now {};
			if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
				GetSystemTimeAsFileTime(&now);
				auto toInt = [](FILETIME t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
				age = (toInt(now) - toInt(created)) / 10000;
			}
			CloseHandle(process);
		}
		return age;
	}

	void watchExplorer(HWND explorer) {
		if (this->explorerWait) {
			UnregisterWaitEx(this->explorerWait, nullptr);
			this->explorerWait = nullptr;
		}
		if (this->explorerProcess) {
			CloseHandle(this->explorerProcess);
			this->explorerProcess = nullptr;
		}
		DWORD pid = 0;
		GetWindowThreadProcessId(explorer, &pid);
		// Seen for the first time: if Explorer itself just started, give it time to settle.
		auto age = processAgeMs(pid);
		this->explorerSince = GetTickCount64() - std::min(age, SETTLE_MS);

		this->explorerProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (!this->explorerProcess) return;
		auto* watcher = this->watcher;
		RegisterWaitForSingleObject(
		    &this->explorerWait,
		    this->explorerProcess,
		    [](PVOID context, BOOLEAN) { PostMessageW(static_cast<HWND>(context), WM_EXPLORER_EXITED, 0, 0); },
		    watcher,
		    INFINITE,
		    WT_EXECUTEONLYONCE
		);
	}

	void check() {
		auto* explorer = this->findExplorerTray();
		if (explorer != this->explorerTray) {
			this->explorerTray = explorer;
			if (explorer) {
				this->stepAside("Explorer's tray changed");
				this->watchExplorer(explorer);
			}
		}

		if (!explorer) {
			this->stepAside("Explorer is not running");
		} else if (!this->tray) {
			if (GetTickCount64() - this->explorerSince >= SETTLE_MS) this->createTray();
		} else if (FindWindowW(L"Shell_TrayWnd", nullptr) != this->tray) {
			// Explorer re-raises its taskbar whenever the foreground app changes: take the place
			// back at once, quietly. Announcing a new taskbar again would make every app re-add
			// its icons each time (seen every ~10 s on a real desktop: icons blinking, vanishing).
			this->raise();
		}

		// Apps that exit without removing their icon.
		for (auto it = this->owners.begin(); it != this->owners.end();) {
			if (IsWindow(it.value())) {
				++it;
				continue;
			}
			IconUpdate update;
			update.kind = IconUpdate::Remove;
			update.key = it.key();
			this->versions.remove(it.key());
			it = this->owners.erase(it);
			QMetaObject::invokeMethod(Backend::instance(), [update]() { Backend::instance()->update(update); }, Qt::QueuedConnection);
		}
	}

	LRESULT forward(WPARAM wParam, LPARAM lParam) const {
		auto* explorer = this->explorerTray;
		if (!explorer || !IsWindow(explorer)) explorer = this->findExplorerTray();
		if (!explorer) return 0;
		// Never wait long: some senders are Explorer's own threads, and a long wait here can
		// chain Explorer to itself through us (tried 15 s once: Explorer hung within minutes).
		DWORD_PTR result = 0;
		SendMessageTimeoutW(explorer, WM_COPYDATA, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result);
		return static_cast<LRESULT>(result);
	}

	void onTrayMessage(const COPYDATASTRUCT* copy) {
		TrayMessage message {};
		std::memcpy(&message, copy->lpData, std::min<size_t>(copy->cbData, sizeof(message)));
		const auto& nid = message.nid;

		IconUpdate update;
		auto byGuid = (nid.uFlags & NIF_GUID) != 0;
		update.key = byGuid ? QUuid(nid.guidItem).toString() : QString("%1:%2").arg(nid.hWnd).arg(nid.uID);
		update.hwnd = nid.hWnd;
		update.uid = nid.uID;
		if (byGuid) update.guid = update.key;
		auto* owner = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(nid.hWnd)); // NOLINT

		switch (message.message) {
		case NIM_DELETE:
			update.kind = IconUpdate::Remove;
			this->owners.remove(update.key);
			this->versions.remove(update.key);
			break;
		case NIM_SETVERSION:
			this->versions.insert(update.key, nid.uVersion);
			[[fallthrough]];
		case NIM_ADD:
		case NIM_MODIFY: {
			DWORD pid = 0;
			GetWindowThreadProcessId(owner, &pid);
			update.pid = pid;
			update.exe = executableOf(pid);
			this->owners.insert(update.key, owner);
			update.version = this->versions.value(update.key);
			if (nid.uFlags & NIF_MESSAGE) {
				update.setCallback = true;
				update.callbackMessage = nid.uCallbackMessage;
			}
			if (nid.uFlags & NIF_ICON) {
				update.setIcon = true;
				update.iconUrl = saveIcon(reinterpret_cast<HICON>(static_cast<ULONG_PTR>(nid.hIcon))); // NOLINT
			}
			if (nid.uFlags & NIF_TIP) {
				update.setTip = true;
				update.tip = fromFixed(nid.szTip, 128);
			}
			if ((nid.uFlags & NIF_STATE) && (nid.dwStateMask & NIS_HIDDEN)) {
				update.setHidden = true;
				update.hidden = (nid.dwState & NIS_HIDDEN) != 0;
			}
			break;
		}
		default: return;
		}

		QMetaObject::invokeMethod(Backend::instance(), [update]() { Backend::instance()->update(update); }, Qt::QueuedConnection);
	}

	static LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* host = current();
		if (host && msg == WM_COPYDATA) {
			auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam); // NOLINT
			if (copy && copy->lpData && copy->dwData == COPYDATA_TRAY
			    && copy->cbData >= offsetof(TrayMessage, nid) + offsetof(NotifyIconData32, szTip))
			{
				// Icon updates: release the sender at once, then pass our own copy on to Explorer.
				// Keeping it blocked until Explorer answers ties its threads to Explorer's through
				// ours, which is how a busy Explorer becomes a hung one.
				QByteArray data(static_cast<const char*>(copy->lpData), static_cast<qsizetype>(copy->cbData));
				COPYDATASTRUCT own = *copy;
				own.lpData = data.data();
				ReplyMessage(TRUE);
				host->forward(wParam, reinterpret_cast<LPARAM>(&own));
				host->onTrayMessage(&own);
				return TRUE;
			}
			// Everything else needs Explorer's real answer (app bars, icon rects): pass it through.
			return host->forward(wParam, lParam);
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
		auto* host = current();
		// Explorer reorders right after the switch; look a moment later.
		if (host && host->watcher) SetTimer(host->watcher, TIMER_RAISE, 50, nullptr);
	}

	static LRESULT CALLBACK watcherProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* host = current();
		if (!host) return DefWindowProcW(hwnd, msg, wParam, lParam);

		if (msg == WM_EXPLORER_EXITED) {
			host->stepAside("Explorer exited");
			host->explorerTray = nullptr;
			return 0;
		}
		if (msg == host->taskbarCreated && host->taskbarCreated != 0) {
			// Not our own announcement: Explorer (re)created its taskbar. Let it settle first.
			if (GetTickCount64() > host->ignoreAnnounceUntil) {
				host->stepAside("Explorer announced a new taskbar");
				host->explorerTray = nullptr;
			}
			return 0;
		}
		if (msg == WM_TIMER && wParam == TIMER_CHECK) {
			host->check();
			return 0;
		}
		if (msg == WM_TIMER && wParam == TIMER_RAISE) {
			KillTimer(hwnd, TIMER_RAISE);
			if (host->tray && FindWindowW(L"Shell_TrayWnd", nullptr) != host->tray) host->raise();
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
};

Backend::Backend() {
	this->promotions = new Promotions(this);
	// Explorer writes the entry of a new icon shortly after it appears, and the user can change
	// it in Settings at any time.
	this->promotions->changed = [this]() {
		for (auto* item: this->all) {
			item->setPromoted(this->promotions->isPromoted(item->exe(), item->iconUid(), item->guid()));
		}
	};
	TrayHost::start();
}

} // namespace

// SystemTrayItem

SystemTrayItem::SystemTrayItem(QString key, QString id, QString title, QObject* parent)
    : QObject(parent)
    , mKey(std::move(key))
    , mId(std::move(id))
    , mTitle(std::move(title)) {}

void SystemTrayItem::setPromoted(bool promoted) {
	if (promoted == this->mPromoted) return;
	this->mPromoted = promoted;
	emit this->promotedChanged();
}

void SystemTrayItem::apply(const IconUpdate& update) {
	if (!update.exe.isEmpty()) this->mExe = update.exe;
	if (!update.guid.isEmpty()) this->mGuid = update.guid;
	this->hwnd = update.hwnd;
	this->uid = update.uid;
	if (update.pid) this->pid = update.pid;
	this->version = update.version;
	if (update.setCallback) this->callbackMessage = update.callbackMessage;
	if (update.setHidden) this->mHidden = update.hidden;
	if (update.setIcon && update.iconUrl != this->mIcon) {
		this->mIcon = update.iconUrl;
		emit this->iconChanged();
	}
	if (update.setTip && update.tip != this->mTip) {
		this->mTip = update.tip;
		emit this->tooltipChanged();
	}
}

void SystemTrayItem::send(quint32 mouseMessage) const {
	if (!this->callbackMessage) return;
	auto* owner = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(this->hwnd)); // NOLINT
	if (this->version >= 4) {
		// NOTIFYICON_VERSION_4: event and icon id in lParam, anchor point in wParam.
		POINT cursor {};
		GetCursorPos(&cursor);
		PostMessageW(
		    owner,
		    this->callbackMessage,
		    MAKEWPARAM(cursor.x, cursor.y),
		    MAKELPARAM(mouseMessage, this->uid)
		);
	} else {
		PostMessageW(owner, this->callbackMessage, this->uid, mouseMessage);
	}
}

void SystemTrayItem::activate() {
	// The shell just got the click, so it may hand the foreground to the app it opens.
	AllowSetForegroundWindow(this->pid ? this->pid : ASFW_ANY);
	this->send(WM_LBUTTONDOWN);
	this->send(WM_LBUTTONUP);
	if (this->version >= 4) this->send(NIN_SELECT);
}

void SystemTrayItem::secondaryActivate() {
	AllowSetForegroundWindow(this->pid ? this->pid : ASFW_ANY);
	this->send(WM_LBUTTONDBLCLK);
	this->send(WM_LBUTTONUP);
}

void SystemTrayItem::display(QObject* /*parentWindow*/, qint32 /*relativeX*/, qint32 /*relativeY*/) {
	// Apps place the menu at the cursor, which is on the shell's icon.
	AllowSetForegroundWindow(this->pid ? this->pid : ASFW_ANY);
	this->send(WM_RBUTTONDOWN);
	this->send(WM_RBUTTONUP);
	if (this->version >= 4) this->send(WM_CONTEXTMENU);
}

// SystemTray

SystemTray::SystemTray(QObject* parent): QObject(parent) { Backend::instance(); }

UntypedObjectModel* SystemTray::items() { return &Backend::instance()->items; }

} // namespace qs::win32::tray
