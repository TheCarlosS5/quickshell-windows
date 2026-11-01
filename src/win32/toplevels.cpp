#include "toplevels.hpp"
#include <array>
#include <utility>

#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qscreen.h>

#include <dwmapi.h>
#include <windows.h>
// clang-format off
#include <appmodel.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
// clang-format on

#include "../core/qmlglobal.hpp"
#include "crash.hpp"
#include "screens.hpp"
#include "virtualdesktops.hpp"

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

// Packaged apps (Windows Terminal, for one) often leave the window property unset; the
// process itself still knows its AppUserModelID, which is what the Start menu lists.
QString packagedAppUserModelId(DWORD pid) {
	auto* proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) return {};
	std::array<wchar_t, APPLICATION_USER_MODEL_ID_MAX_LENGTH> buf {};
	auto size = static_cast<UINT32>(buf.size());
	QString id;
	if (GetApplicationUserModelId(proc, &size, buf.data()) == ERROR_SUCCESS) id = QString::fromWCharArray(buf.data());
	CloseHandle(proc);
	return id;
}

// By HMONITOR: matching QScreen::name() with the device name found nothing in recent Qt (screens.hpp), so windows
// had no screen and services/DesktopCover.qml never saw the desktop covered.
QuickshellScreenInfo* screenInfoOf(HWND hwnd) {
	auto* screen = qs::win32::screenOf(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
	return screen ? QuickshellTracked::instance()->screenInfo(screen) : nullptr;
}

// Set as soon as the tracker exists, before anything in its constructor can wait. The hook callbacks
// must NOT call WindowTracker::instance(): the constructor makes COM calls to Explorer (which pump
// messages while waiting), the pump delivers a WinEvent, and instance() would then wait for the
// constructor that this very thread is in the middle of - a self-deadlock that left the shell stuck
// at start-up now and then (seen on a stack dump: _Init_thread_header under WindowTracker::refresh).
WindowTracker* constructedTracker = nullptr; // NOLINT

// Not one of WindowHandle::Part: "check whether it is (still) an app window".
constexpr quint8 MEMBERSHIP = 32;

void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
	// Only top-level windows matter (a destroyed one has no ancestor any more: let it through).
	if (event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_DESTROY
	    && (!hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd))
		return;
	// Our own windows (the dock's previews, the bar's popups, the overview...) are never tracked, but each
	// one that appeared, hid or moved used to start a full re-scan of every window: a stall on the main
	// thread every time a popup opened. Only the foreground change is still needed (who is in front).
	if (event != EVENT_SYSTEM_FOREGROUND && hwnd) {
		static const DWORD ownPid = GetCurrentProcessId();
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == ownPid) return;
	}
	if (constructedTracker) constructedTracker->onEvent(event, reinterpret_cast<quintptr>(hwnd)); // NOLINT
}

// Shell hook messages (plan 2026-10-31, F17): Windows tells registered windows when an app flashes its taskbar
// button (FlashWindowEx), which is how apps ask for attention (a message arrived, a download finished).
UINT shellHookMessage = 0; // NOLINT
LRESULT CALLBACK shellHookProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == shellHookMessage && shellHookMessage != 0 && constructedTracker) {
		// HSHELL_FLASH (REDRAW + high bit) only reaches the taskman window (Explorer's taskbar); other listeners get
		// the flash as a plain HSHELL_REDRAW, which also comes with title and icon changes: onFlash tells them apart.
		if ((wParam & 0x7fff) == HSHELL_REDRAW) {
			constructedTracker->onFlash(reinterpret_cast<quintptr>(reinterpret_cast<HWND>(lParam)), (wParam & 0x8000) != 0); // NOLINT
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
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

void WindowHandle::update(quint8 parts, quintptr foreground) {
	auto* h = toHwnd(this->mHwnd);

	if (parts & Desktop) {
		auto desktop = static_cast<qint32>(VirtualDesktops::instance()->desktopOf(this->mHwnd));
		if (desktop != this->mDesktop) {
			this->mDesktop = desktop;
			emit this->desktopChanged();
		}
	}

	if (parts & Title) {
		auto title = windowText(h);
		if (title != this->mTitle) {
			this->mTitle = title;
			emit this->titleChanged();
		}
	}

	if (parts & AppId) {
		// Prefer the AppUserModelID (what the taskbar groups by); fall back to the exe name,
		// which is what most desktop-entry style lookups expect.
		auto appId = appUserModelId(h);
		if (appId.isEmpty()) {
			// OpenProcess + GetApplicationUserModelId on every refresh of every window was a good part of the
			// stalls: the process' id is the same for its whole life.
			if (!this->mPackagedIdKnown) {
				this->mPackagedId = packagedAppUserModelId(static_cast<DWORD>(this->mPid));
				this->mPackagedIdKnown = true;
			}
			appId = this->mPackagedId;
		}
		if (appId.isEmpty()) appId = QFileInfo(this->mExecutable).completeBaseName();
		// File Explorer windows carry no id of their own; the Start menu (and the dock pin) knows
		// Explorer by this one.
		if (appId.compare("explorer", Qt::CaseInsensitive) == 0) appId = "Microsoft.Windows.Explorer";
		if (appId != this->mAppId) {
			this->mAppId = appId;
			emit this->appIdChanged();
		}
	}

	if (parts & Activated) {
		auto activated = foreground == this->mHwnd;
		if (activated != this->mActivated) {
			this->mActivated = activated;
			emit this->activatedChanged();
		}
	}

	if (!(parts & State)) return;

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

	auto* screen = screenInfoOf(h);
	auto screens = screen ? QList<QuickshellScreenInfo*> {screen} : QList<QuickshellScreenInfo*> {};
	if (screens != this->mScreens) {
		this->mScreens = screens;
		emit this->screensChanged();
	}
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
	// Plan 2026-10-31, D1: a WinEvent re-reads only the window it is about (pendingTimer); the full scan is the
	// safety net, at most once a second after an event that may have missed something, and every 10 s.
	this->debounce.setSingleShot(true);
	this->debounce.setInterval(1000);
	QObject::connect(&this->debounce, &QTimer::timeout, this, &WindowTracker::refresh);
	this->pendingTimer.setSingleShot(true);
	this->pendingTimer.setInterval(50);
	QObject::connect(&this->pendingTimer, &QTimer::timeout, this, &WindowTracker::processPending);
	this->periodic.setInterval(10000);
	QObject::connect(&this->periodic, &QTimer::timeout, this, [this]() {
		// Every third one (30 s) also re-reads what has no event of its own: app ids, and desktops as a check.
		if (++this->periodicCount % 3 == 0) this->fullParts |= WindowHandle::AppId | WindowHandle::Desktop;
		this->refresh();
	});
	this->periodic.start();
	if (qEnvironmentVariableIntValue("QS_TOPLEVELS_STATS") == 1) {
		this->statsTimer.setInterval(10000);
		QObject::connect(&this->statsTimer, &QTimer::timeout, this, &WindowTracker::logStats);
		this->statsTimer.start();
	}
	constructedTracker = this;

	// Out-of-context hooks: callbacks arrive on this thread's message loop, nothing is injected.
	SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
	// Desktops created, closed or switched: windows' desktop indexes change (each window, right away), and a
	// full scan after it (windows that only now count).
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::changed, this, [this]() {
		for (auto it = this->byHwnd.cbegin(); it != this->byHwnd.cend(); ++it)
			this->markDirty(it.key(), MEMBERSHIP | WindowHandle::Desktop | WindowHandle::State);
		this->scheduleRefresh();
	});

	// Apps asking for attention (F17): a hidden window registered for the shell hook messages. Not message-only:
	// those do not get the shell's notifications on every Windows build.
	WNDCLASSW wc {};
	wc.lpfnWndProc = shellHookProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"QsShellHook";
	RegisterClassW(&wc);
	auto* hook = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
	if (hook) {
		shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");
		if (RegisterShellHookWindow(hook)) {
			this->shellHookWindow = hook;
		}
		else {
			qWarning() << "toplevels: RegisterShellHookWindow failed" << GetLastError();
			DestroyWindow(hook);
		}
	}

	this->fullParts = WindowHandle::All;
	this->refresh();
}

void WindowTracker::onFlash(quintptr hwnd, bool certain) {
	auto* handle = this->byHwnd.value(hwnd);
	// The window in front does not need to ask (some apps flash when they get the focus).
	if (!handle || hwnd == reinterpret_cast<quintptr>(GetForegroundWindow())) return; // NOLINT
	if (!certain) {
		// A redraw that is a new title (its NAMECHANGE may not be handled yet): not a request for attention.
		auto title = windowText(toHwnd(hwnd));
		if (title != handle->title()) {
			handle->update(WindowHandle::Title, reinterpret_cast<quintptr>(GetForegroundWindow())); // NOLINT
			return;
		}
		// The same title, but it changed a moment ago: its REDRAW came after the debounced update. A real flash repeats.
		if (GetTickCount64() - this->lastNameChange.value(hwnd, 0) < 700) return;
	}
	handle->setUrgent(true);
}

void WindowTracker::scheduleRefresh() {
	if (!this->debounce.isActive()) this->debounce.start();
}

void WindowTracker::onEvent(quint32 event, quintptr hwnd) {
	this->stats.events += 1;
	if (event == EVENT_SYSTEM_FOREGROUND) {
		this->setForeground(hwnd);
		return;
	}
	if (!hwnd) {
		this->scheduleRefresh();
		return;
	}
	auto tracked = this->byHwnd.contains(hwnd);
	switch (event) {
	case EVENT_OBJECT_NAMECHANGE:
		if (tracked) this->lastNameChange.insert(hwnd, GetTickCount64());
		// A tracked window's title; another window may count once it has one.
		this->markDirty(hwnd, tracked ? WindowHandle::Title : MEMBERSHIP);
		break;
	case EVENT_OBJECT_LOCATIONCHANGE:
	case EVENT_SYSTEM_MINIMIZESTART:
	case EVENT_SYSTEM_MINIMIZEEND:
		if (tracked) this->markDirty(hwnd, WindowHandle::State);
		break;
	case EVENT_OBJECT_CLOAKED:
	case EVENT_OBJECT_UNCLOAKED:
		// Also what a window moved to another desktop gets.
		this->markDirty(hwnd, MEMBERSHIP | WindowHandle::Desktop | WindowHandle::State);
		break;
	default: // CREATE, DESTROY, SHOW, HIDE
		this->markDirty(hwnd, MEMBERSHIP | WindowHandle::State | WindowHandle::Title);
		break;
	}
}

void WindowTracker::markDirty(quintptr hwnd, quint8 parts) {
	this->pending[hwnd] |= parts;
	if (!this->pendingTimer.isActive()) this->pendingTimer.start();
}

void WindowTracker::setForeground(quintptr hwnd) {
	auto* now = this->byHwnd.value(hwnd);
	if (this->mActive && this->mActive != now) this->mActive->update(WindowHandle::Activated, hwnd);
	if (now) {
		now->update(WindowHandle::Activated, hwnd);
		now->setUrgent(false); // seen: no longer asking
	}
	if (now != this->mActive) {
		this->mActive = now;
		emit this->activeChanged();
	}
	// A window not known yet coming to the front: maybe a new one.
	if (!now && hwnd) {
		static const DWORD ownPid = GetCurrentProcessId();
		DWORD pid = 0;
		GetWindowThreadProcessId(toHwnd(hwnd), &pid);
		if (pid != ownPid) this->markDirty(hwnd, MEMBERSHIP | WindowHandle::State | WindowHandle::Title);
	}
}

void WindowTracker::addWindow(quintptr hwnd, quintptr foreground) {
	auto* handle = new WindowHandle(hwnd, this);
	handle->update(WindowHandle::All, foreground);
	this->byHwnd.insert(hwnd, handle);
	this->mWindows.insertObject(handle);
}

void WindowTracker::removeWindow(WindowHandle* handle) {
	this->byHwnd.remove(static_cast<quintptr>(handle->hwnd()));
	this->lastNameChange.remove(static_cast<quintptr>(handle->hwnd()));
	this->mWindows.removeObject(handle);
	emit handle->closed();
	handle->deleteLater();
}

void WindowTracker::processPending() {
	crash::ScopedSubsystem subsystem("toplevels");
	auto pending = std::exchange(this->pending, {});
	auto foreground = reinterpret_cast<quintptr>(GetForegroundWindow()); // NOLINT
	auto changed = false;
	for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
		auto hwnd = it.key();
		auto parts = it.value();
		auto* handle = this->byHwnd.value(hwnd);
		auto counts = !(parts & MEMBERSHIP) || (IsWindow(toHwnd(hwnd)) && WindowTracker::isAppWindow(hwnd));
		this->stats.windowUpdates += 1;
		if (handle && !counts) {
			this->removeWindow(handle);
			changed = true;
		} else if (handle) {
			handle->update(parts & WindowHandle::All, foreground);
		} else if (counts && (parts & MEMBERSHIP)) {
			this->addWindow(hwnd, foreground);
			changed = true;
		}
	}
	auto* active = this->byHwnd.value(foreground);
	if (active != this->mActive) {
		if (this->mActive) this->mActive->update(WindowHandle::Activated, foreground);
		if (active) active->update(WindowHandle::Activated, foreground);
		this->mActive = active;
		emit this->activeChanged();
	}
	if (changed) emit this->windowsChanged();
}

void WindowTracker::logStats() {
	qInfo().nospace() << "toplevels: " << this->stats.events << " events, " << this->stats.windowUpdates
	                  << " window updates, " << this->stats.fullScans << " full scans, " << this->byHwnd.size()
	                  << " windows (last 10 s)";
	this->stats = {};
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

	// Cloaked: suspended UWP frames, hidden shell UI, and windows on other virtual desktops.
	// Those last ones are still the user's windows (Hyprland lists all workspaces' windows too).
	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	// Windows on other desktops are cloaked by the shell (not by their app, not inherited).
	if (cloaked != 0 && (cloaked != DWM_CLOAKED_SHELL || !VirtualDesktops::instance()->onOtherDesktop(handle))) return false;

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

	// 3. Windows lifts the foreground lock while Alt is held. Press Alt, switch, then release
	//    it behind an unassigned key (vkE8): a lone Alt press+release would put the window
	//    in keyboard menu mode, which swallows the next keystrokes.
	auto key = [](WORD vk, bool up) {
		INPUT in {};
		in.type = INPUT_KEYBOARD;
		in.ki.wVk = vk;
		in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
		return in;
	};
	auto altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	if (!altHeld) {
		auto down = key(VK_MENU, false);
		SendInput(1, &down, sizeof(INPUT));
	}
	SetForegroundWindow(hwnd);
	if (!altHeld) {
		INPUT rest[] = {key(0xE8, false), key(0xE8, true), key(VK_MENU, true)};
		SendInput(3, rest, sizeof(INPUT));
	}

	if (GetForegroundWindow() != hwnd) {
		qWarning() << "Could not move keyboard focus to" << Qt::hex << handle;
	}
}

void WindowTracker::refresh() {
	crash::ScopedSubsystem subsystem("toplevels");
	this->stats.fullScans += 1;
	auto parts = std::exchange(this->fullParts, WindowHandle::Title | WindowHandle::State | WindowHandle::Activated);
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

	// removals (found only lists app windows: isAppWindow ran for each)
	for (auto it = this->byHwnd.begin(); it != this->byHwnd.end();) {
		auto* handle = it.value();
		if (!found.contains(it.key())) {
			this->mWindows.removeObject(handle);
			emit handle->closed();
			handle->deleteLater();
			it = this->byHwnd.erase(it);
			changed = true;
		} else {
			handle->update(parts, foreground);
			++it;
		}
	}

	// additions (in z-order, so the first seen is the most recently used)
	for (auto hwnd: found) {
		if (this->byHwnd.contains(hwnd)) continue;
		this->addWindow(hwnd, foreground);
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
