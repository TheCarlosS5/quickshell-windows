#include "toplevels.hpp"
#include <array>

#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qscreen.h>

#include <dwmapi.h>
#include <windows.h>
// clang-format off
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
// clang-format on

#include "../core/qmlglobal.hpp"

namespace qs::win32 {

namespace {

HWND toHwnd(quintptr h) { return reinterpret_cast<HWND>(h); } // NOLINT

QString windowText(HWND hwnd) {
	std::array<wchar_t, 512> buf {};
	auto n = GetWindowTextW(hwnd, buf.data(), static_cast<int>(buf.size()));
	return QString::fromWCharArray(buf.data(), n);
}

QString className(HWND hwnd) {
	std::array<wchar_t, 256> buf {};
	auto n = GetClassNameW(hwnd, buf.data(), static_cast<int>(buf.size()));
	return QString::fromWCharArray(buf.data(), n);
}

QString processImage(DWORD pid) {
	auto* proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) return {};
	std::array<wchar_t, MAX_PATH * 2> buf {};
	auto size = static_cast<DWORD>(buf.size());
	QString path;
	if (QueryFullProcessImageNameW(proc, 0, buf.data(), &size)) path = QString::fromWCharArray(buf.data(), size);
	CloseHandle(proc);
	return path;
}

QString appUserModelId(HWND hwnd) {
	IPropertyStore* store = nullptr;
	if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return {};
	PROPVARIANT value;
	PropVariantInit(&value);
	QString id;
	if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR) {
		id = QString::fromWCharArray(value.pwszVal);
	}
	PropVariantClear(&value);
	store->Release();
	return id;
}

QuickshellScreenInfo* screenOf(HWND hwnd) {
	MONITORINFOEXW info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return nullptr;
	auto device = QString::fromWCharArray(info.szDevice);
	for (auto* screen: QGuiApplication::screens()) {
		if (screen->name() == device) return QuickshellTracked::instance()->screenInfo(screen);
	}
	return nullptr;
}

void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
	if (event == EVENT_OBJECT_LOCATIONCHANGE && (!hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd)) return;
	WindowTracker::instance()->scheduleRefresh();
}

} // namespace

// WindowHandle

WindowHandle::WindowHandle(quintptr hwnd, QObject* parent): QObject(parent), mHwnd(hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(toHwnd(hwnd), &pid);
	this->mPid = pid;
	this->mExecutable = processImage(pid);
	this->mHyprland = new HyprlandToplevelInfo(this);
}

HyprlandToplevelInfo::HyprlandToplevelInfo(WindowHandle* handle): QObject(handle), handle(handle) {
	QObject::connect(handle, &WindowHandle::titleChanged, this, &HyprlandToplevelInfo::titleChanged);
	QObject::connect(handle, &WindowHandle::activatedChanged, this, &HyprlandToplevelInfo::activatedChanged);
}

bool WindowHandle::refresh(quintptr foreground) {
	auto* h = toHwnd(this->mHwnd);
	if (!IsWindow(h) || !WindowTracker::isAppWindow(this->mHwnd)) return false;

	auto title = windowText(h);
	if (title != this->mTitle) {
		this->mTitle = title;
		emit this->titleChanged();
	}

	// Prefer the AppUserModelID (what the taskbar groups by); fall back to the exe name,
	// which is what most desktop-entry style lookups expect.
	auto appId = appUserModelId(h);
	if (appId.isEmpty()) appId = QFileInfo(this->mExecutable).completeBaseName();
	if (appId != this->mAppId) {
		this->mAppId = appId;
		emit this->appIdChanged();
	}

	auto activated = foreground == this->mHwnd;
	if (activated != this->mActivated) {
		this->mActivated = activated;
		emit this->activatedChanged();
	}

	auto minimized = IsIconic(h) != FALSE;
	if (minimized != this->mMinimized) {
		this->mMinimized = minimized;
		emit this->minimizedChanged();
	}

	auto maximized = IsZoomed(h) != FALSE;
	if (maximized != this->mMaximized) {
		this->mMaximized = maximized;
		emit this->maximizedChanged();
	}

	RECT rect;
	GetWindowRect(h, &rect);
	auto geometry = QRect(QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1));
	if (geometry != this->mGeometry) {
		this->mGeometry = geometry;
		emit this->geometryChanged();
	}

	MONITORINFO info {};
	info.cbSize = sizeof(info);
	auto fullscreen = false;
	if (!minimized && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &info)) {
		fullscreen = rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top
		          && rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
	}
	if (fullscreen != this->mFullscreen) {
		this->mFullscreen = fullscreen;
		emit this->fullscreenChanged();
	}

	auto* screen = screenOf(h);
	auto screens = screen ? QList<QuickshellScreenInfo*> {screen} : QList<QuickshellScreenInfo*> {};
	if (screens != this->mScreens) {
		this->mScreens = screens;
		emit this->screensChanged();
	}

	return true;
}

void WindowHandle::activate() { WindowTracker::forceForeground(this->mHwnd); }

void WindowHandle::close() { PostMessageW(toHwnd(this->mHwnd), WM_CLOSE, 0, 0); }

void WindowHandle::setMaximized(bool maximized) {
	ShowWindowAsync(toHwnd(this->mHwnd), maximized ? SW_MAXIMIZE : SW_RESTORE);
}

void WindowHandle::setMinimized(bool minimized) {
	ShowWindowAsync(toHwnd(this->mHwnd), minimized ? SW_MINIMIZE : SW_RESTORE);
}

// Window placement stays Windows' job; these exist for API compatibility only.
void WindowHandle::setFullscreen(bool /*fullscreen*/) {}
void WindowHandle::fullscreenOn(QuickshellScreenInfo* /*screen*/) {}
void WindowHandle::setRectangle(QObject* /*window*/, QRect /*rect*/) {}
void WindowHandle::unsetRectangle() {}

// WindowTracker

WindowTracker* WindowTracker::instance() {
	static auto* tracker = new WindowTracker(); // NOLINT
	return tracker;
}

WindowTracker::WindowTracker(QObject* parent): QObject(parent) {
	this->debounce.setSingleShot(true);
	this->debounce.setInterval(40);
	QObject::connect(&this->debounce, &QTimer::timeout, this, &WindowTracker::refresh);

	// Out-of-context hooks: callbacks arrive on this thread's message loop, nothing is injected.
	SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);

	this->refresh();
}

void WindowTracker::scheduleRefresh() {
	if (!this->debounce.isActive()) this->debounce.start();
}

bool WindowTracker::isAppWindow(quintptr handle) {
	auto* hwnd = toHwnd(handle);
	if (!IsWindowVisible(hwnd)) return false;
	if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId()) return false;

	auto ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
	auto appWindow = (ex & WS_EX_APPWINDOW) != 0;
	if (!appWindow) {
		if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
		if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
	}

	// Cloaked: suspended UWP frames, windows on other virtual desktops, hidden shell UI.
	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked != 0) return false;

	if (GetWindowTextLengthW(hwnd) == 0) return false;

	static const QStringList ignoredClasses = {
	    "Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Windows.UI.Core.CoreWindow",
	};
	return !ignoredClasses.contains(className(hwnd));
}

void WindowTracker::forceForeground(quintptr handle) {
	auto* hwnd = toHwnd(handle);
	if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
	if (GetForegroundWindow() == hwnd) return;

	// 1. Allowed outright when we already own the foreground or got the last input.
	if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;

	// 2. Share input state with the current foreground thread for the duration of the call.
	auto* fg = GetForegroundWindow();
	auto fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
	auto self = GetCurrentThreadId();
	auto attached = fgThread != 0 && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
	SetForegroundWindow(hwnd);
	BringWindowToTop(hwnd);
	if (attached) AttachThreadInput(self, fgThread, FALSE);
	if (GetForegroundWindow() == hwnd) return;

	// 3. Windows lifts the foreground lock while Alt is held. Press Alt, switch, release Alt:
	//    the previous app only sees Alt-down (no menu, menus open on release) and the
	//    release lands in our window, where a lone Alt does nothing.
	INPUT down {};
	down.type = INPUT_KEYBOARD;
	down.ki.wVk = VK_MENU;
	INPUT up = down;
	up.ki.dwFlags = KEYEVENTF_KEYUP;
	auto altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	if (!altHeld) SendInput(1, &down, sizeof(INPUT));
	SetForegroundWindow(hwnd);
	if (!altHeld) SendInput(1, &up, sizeof(INPUT));

	if (GetForegroundWindow() != hwnd) {
		qWarning() << "Could not move keyboard focus to" << Qt::hex << handle;
	}
}

void WindowTracker::refresh() {
	QList<quintptr> found;
	EnumWindows(
	    [](HWND hwnd, LPARAM data) -> BOOL {
		    if (WindowTracker::isAppWindow(reinterpret_cast<quintptr>(hwnd))) {
			    reinterpret_cast<QList<quintptr>*>(data)->append(reinterpret_cast<quintptr>(hwnd)); // NOLINT
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&found) // NOLINT
	);

	auto foreground = reinterpret_cast<quintptr>(GetForegroundWindow());
	auto changed = false;

	// removals
	for (auto it = this->byHwnd.begin(); it != this->byHwnd.end();) {
		auto* handle = it.value();
		if (!found.contains(it.key()) || !handle->refresh(foreground)) {
			this->mWindows.removeObject(handle);
			emit handle->closed();
			handle->deleteLater();
			it = this->byHwnd.erase(it);
			changed = true;
		} else {
			++it;
		}
	}

	// additions (in z-order, so the first seen is the most recently used)
	for (auto hwnd: found) {
		if (this->byHwnd.contains(hwnd)) continue;
		auto* handle = new WindowHandle(hwnd, this);
		if (!handle->refresh(foreground)) {
			delete handle;
			continue;
		}
		this->byHwnd.insert(hwnd, handle);
		this->mWindows.insertObject(handle);
		changed = true;
	}

	auto* active = this->byHwnd.value(foreground);
	if (active != this->mActive) {
		this->mActive = active;
		emit this->activeChanged();
	}

	if (changed) emit this->windowsChanged();
}

} // namespace qs::win32
