#include "tray.hpp"
#include <cstddef>
#include <cstring>
#include <thread>

#include <qbuffer.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qurl.h>
#include <quuid.h>

#include <windows.h>
#include <shellapi.h>

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
		auto shown = this->items.valueList().contains(item);
		if (item->isHidden() && shown) this->items.removeObject(item);
		else if (!item->isHidden() && !shown) this->items.insertObject(item);
	}

private:
	Backend();

	QHash<QString, SystemTrayItem*> all;
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
	void claim() {
		SetWindowPos(this->tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
		} else if (FindWindowW(L"Shell_TrayWnd", nullptr) != this->tray && GetTickCount64() - this->lastClaim > 10000) {
			// Explorer's taskbar can end up above ours (it re-raises itself): take the place back.
			qCInfo(logTray) << "Reclaiming the tray";
			this->claim();
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
			// Explorer gets everything first-hand too: app bars, icon rects, and the icons.
			auto result = host->forward(wParam, lParam);
			if (copy && copy->lpData && copy->dwData == COPYDATA_TRAY
			    && copy->cbData >= offsetof(TrayMessage, nid) + offsetof(NotifyIconData32, szTip))
			{
				host->onTrayMessage(copy);
				return TRUE;
			}
			return result;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
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
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
};

Backend::Backend() { TrayHost::start(); }

} // namespace

// SystemTrayItem

SystemTrayItem::SystemTrayItem(QString key, QString id, QString title, QObject* parent)
    : QObject(parent)
    , mKey(std::move(key))
    , mId(std::move(id))
    , mTitle(std::move(title)) {}

void SystemTrayItem::apply(const IconUpdate& update) {
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
