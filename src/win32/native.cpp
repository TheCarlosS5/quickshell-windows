#include "native.hpp"
#include <cmath>
#include <cstddef>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qwindow.h>

#include <dwmapi.h>
#include <windows.h>
// clang-format off
#include <shellapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <wtsapi32.h>
// clang-format on

#include "../core/logcat.hpp"

namespace qs::win32 {

QS_LOGGING_CATEGORY(logWin32, "quickshell.win32", QtInfoMsg);
QS_LOGGING_CATEGORY(logAppBar, "quickshell.win32.appbar", QtInfoMsg);

namespace {

HWND toHwnd(quintptr h) { return reinterpret_cast<HWND>(h); } // NOLINT

QRect toQRect(const RECT& r) { return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1)); }

} // namespace

quintptr hwnd(QWindow* window) {
	if (window == nullptr || window->handle() == nullptr) return 0;
	return static_cast<quintptr>(window->winId());
}

void applyShellWindowStyle(QWindow* window, bool focusable) {
	auto flags = window->flags();
	flags |= Qt::FramelessWindowHint | Qt::Tool | Qt::NoDropShadowWindowHint;
	flags.setFlag(Qt::WindowDoesNotAcceptFocus, !focusable);
	window->setFlags(flags);
	// Focusable panels (the desktop) take the keyboard when clicked, never just by appearing:
	// keyboard grabs activate explicitly (WinPanelWindow::updateKeyboardGrab).
	window->setProperty("_q_showWithoutActivating", true);
	// Alt+F4 and other close requests are ignored on these (see NativeEventRouter).
	window->setProperty("_qs_shellSurface", true);

	auto h = toHwnd(hwnd(window));
	if (!h) return;

	// Shell surfaces draw their own shape; no Win11 rounding, accent border or open/close animation.
	DWORD corner = DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
	COLORREF border = DWMWA_COLOR_NONE;
	DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
	BOOL disable = TRUE;
	DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

// LayerManager

namespace {
// The highest visible window covering the whole monitor, if any.
HWND coveringWindow(quintptr monitor) {
	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(monitor), &info)) return nullptr; // NOLINT

	struct Search {
		RECT monitor;
		HWND found = nullptr;
	} search {info.rcMonitor};

	EnumWindows(
	    [](HWND hwnd, LPARAM data) -> BOOL {
		    auto* s = reinterpret_cast<Search*>(data); // NOLINT
		    if (!IsWindowVisible(hwnd)) return TRUE;
		    DWORD cloaked = 0;
		    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		    if (cloaked) return TRUE;
		    // Other apps' click-through or tool overlays covering the screen (cursor and FPS
		    // overlays, screen recorders...) are not something the user is playing or watching.
		    DWORD pid = 0;
		    GetWindowThreadProcessId(hwnd, &pid);
		    auto overlay = WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
		    if (pid != GetCurrentProcessId() && (GetWindowLongW(hwnd, GWL_EXSTYLE) & overlay)) return TRUE;
		    RECT r;
		    GetWindowRect(hwnd, &r);
		    if (r.left <= s->monitor.left && r.top <= s->monitor.top && r.right >= s->monitor.right
		        && r.bottom >= s->monitor.bottom)
		    {
			    s->found = hwnd;
			    return FALSE;
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search) // NOLINT
	);
	return search.found;
}

void CALLBACK onForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
	QMetaObject::invokeMethod(LayerManager::instance(), &LayerManager::restackDesktop, Qt::QueuedConnection);
	QMetaObject::invokeMethod(LayerManager::instance(), &LayerManager::updateFullscreen, Qt::QueuedConnection);
}
} // namespace

LayerManager::LayerManager(QObject* parent): QObject(parent) {
	// Keeps focusable desktop-layer windows in place when they are activated (see the router).
	NativeEventRouter::instance();
	// Out-of-context hook, nothing injected.
	SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
}

LayerManager* LayerManager::instance() {
	static auto* manager = new LayerManager(); // NOLINT
	return manager;
}

void LayerManager::setLayer(QWindow* window, Layer layer) {
	if (!this->layers.contains(window)) {
		QObject::connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
			if (visible) this->restack();
		});
		QObject::connect(window, &QObject::destroyed, this, [this, window]() {
			this->layers.remove(window);
		});
	}

	this->layers.insert(window, layer);
	this->apply(window, layer);
	this->restack();
}

void LayerManager::remove(QWindow* window) {
	this->layers.remove(window);
	this->yielding.remove(window);
	QObject::disconnect(window, nullptr, this, nullptr);
}

void LayerManager::apply(QWindow* window, Layer layer) {
	auto above = layer == Layer::Top || layer == Layer::Overlay;
	window->setFlag(Qt::WindowStaysOnTopHint, above);
	// Not WindowStaysOnBottomHint: HWND_BOTTOM would put us under the desktop itself.
	window->setFlag(Qt::WindowStaysOnBottomHint, false);
	if (!above) this->restackDesktop();
}

void LayerManager::restackDesktop() {
	auto* progman = FindWindowW(L"Progman", nullptr);
	if (!progman) return;

	// Desired order, top to bottom: Bottom-layer windows, then Background-layer windows,
	// then Progman.
	QList<HWND> ours;
	for (auto pass: {Layer::Bottom, Layer::Background}) {
		for (auto [window, layer]: this->layers.asKeyValueRange()) {
			if (layer != pass || !window->isVisible()) continue;
			if (auto* h = toHwnd(hwnd(window))) ours.append(h);
		}
	}
	if (ours.isEmpty()) return;

	// Already in place? Walking up from Progman we must meet ours in reverse order.
	auto* walk = GetWindow(progman, GW_HWNDPREV);
	auto inPlace = true;
	for (auto i = ours.size() - 1; i >= 0; --i) {
		if (walk != ours[i]) {
			inPlace = false;
			break;
		}
		walk = GetWindow(walk, GW_HWNDPREV);
	}
	if (inPlace) return;

	// Lowest window above Progman that is not ours. If it is topmost there are no normal
	// windows at all; inserting after a topmost window would make ours topmost, so use the
	// top of the normal band instead.
	auto* above = GetWindow(progman, GW_HWNDPREV);
	while (above && ours.contains(above)) above = GetWindow(above, GW_HWNDPREV);
	auto aboveIsTopmost = above && (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST);

	HWND insertAfter = (!above || aboveIsTopmost) ? HWND_NOTOPMOST : above;
	for (auto* h: ours) {
		SetWindowPos(h, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
		insertAfter = h;
	}
}

bool LayerManager::isDesktopLayer(quintptr hwnd) const {
	for (auto [window, layer]: this->layers.asKeyValueRange()) {
		if (layer != Layer::Bottom && layer != Layer::Background) continue;
		if (qs::win32::hwnd(window) == hwnd) return true;
	}
	return false;
}

void LayerManager::restack() {
	this->restackDesktop();

	// Top first, then Overlay, so Overlay ends up at the very top of the topmost band.
	for (auto pass: {Layer::Top, Layer::Overlay}) {
		for (auto [window, layer]: this->layers.asKeyValueRange()) {
			if (layer != pass || !window->isVisible()) continue;
			auto h = toHwnd(hwnd(window));
			if (!h) continue;

			auto monitor = reinterpret_cast<quintptr>(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
			auto demoted = this->yields(window, pass) && this->fullscreenMonitors.contains(monitor);

			SetWindowPos(
			    h,
			    demoted ? HWND_BOTTOM : HWND_TOPMOST,
			    0,
			    0,
			    0,
			    0,
			    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
			);
		}
	}
}

void LayerManager::setYieldsToFullscreen(QWindow* window, bool yields) {
	if (yields == this->yielding.contains(window)) return;
	if (yields) this->yielding.insert(window);
	else this->yielding.remove(window);
	this->restack();
}

void LayerManager::setFullscreenAppActive(quintptr monitor, bool active) {
	if (active) {
		if (!this->reportedFullscreen.contains(monitor)) this->reportedFullscreen.append(monitor);
	} else {
		this->reportedFullscreen.removeAll(monitor);
	}
	this->updateFullscreen();
}

// The shell steps aside on a monitor while Explorer reports a fullscreen app there and the
// highest window covering that monitor is another app's (a game, a video). Our own full-screen
// surfaces trigger the report too: the desktop (activated by a click, or when the app in front
// is minimized) means there is no fullscreen app; the overlay or the overview opened over a
// game leave the state as it was. Re-checked on every foreground change, so a stale report
// never leaves the bar and the dock hidden.
void LayerManager::updateFullscreen() {
	auto changed = false;
	auto mine = [](HWND hwnd) {
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		return pid == GetCurrentProcessId();
	};

	for (auto monitor: QList<quintptr>(this->fullscreenMonitors)) {
		auto* top = coveringWindow(monitor);
		auto keep = this->reportedFullscreen.contains(monitor) && top
		         && (!mine(top) || !this->isDesktopLayer(reinterpret_cast<quintptr>(top)));
		if (keep) continue;
		qCInfo(logWin32) << "Fullscreen app left monitor" << monitor;
		this->fullscreenMonitors.removeAll(monitor);
		changed = true;
	}
	for (auto monitor: this->reportedFullscreen) {
		if (this->fullscreenMonitors.contains(monitor)) continue;
		auto* top = coveringWindow(monitor);
		if (!top || mine(top)) continue;
		qCInfo(logWin32) << "Fullscreen app entered monitor" << monitor;
		this->enterFullscreen(monitor);
		changed = true;
	}
	if (changed) this->restack();
}

void LayerManager::enterFullscreen(quintptr monitor) {
	this->fullscreenMonitors.append(monitor);

	// Leaving the topmost band needs an explicit HWND_NOTOPMOST first.
	for (auto [window, layer]: this->layers.asKeyValueRange()) {
		if (!this->yields(window, layer)) continue;
		auto h = toHwnd(hwnd(window));
		if (!h) continue;
		if (reinterpret_cast<quintptr>(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)) != monitor) {
			continue;
		}
		SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

// InputRegions

namespace {
void CALLBACK onCursorMoved(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
	if (idObject == OBJID_CURSOR) InputRegions::instance()->cursorMoved();
}
} // namespace

// Which windows let clicks through depends on where the pointer is. Windows reports every pointer
// move (out of context, nothing injected), so the check runs only while the mouse moves; a slow
// timer catches what moves under a still pointer. Polling every 16 ms kept laptops from idling.
InputRegions::InputRegions(QObject* parent): QObject(parent) {
	this->timer.setInterval(250);
	QObject::connect(&this->timer, &QTimer::timeout, this, &InputRegions::poll);
	SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, onCursorMoved, 0, 0, WINEVENT_OUTOFCONTEXT);
}

void InputRegions::cursorMoved() {
	if (this->pollQueued || this->entries.isEmpty()) return;
	this->pollQueued = true;
	QMetaObject::invokeMethod(this, [this]() {
		this->pollQueued = false;
		this->poll();
	}, Qt::QueuedConnection);
}

InputRegions* InputRegions::instance() {
	static auto* regions = new InputRegions(); // NOLINT
	return regions;
}

void InputRegions::setRegion(QWindow* window, const QRegion& region, bool hasMask) {
	if (!hasMask) {
		this->remove(window);
		return;
	}

	if (!this->entries.contains(window)) {
		QObject::connect(window, &QObject::destroyed, this, [this, window]() {
			this->entries.remove(window);
			if (this->entries.isEmpty()) this->timer.stop();
		});
	}

	auto& entry = this->entries[window];
	entry.region = region;

	// An empty mask means the window never takes input.
	if (region.isEmpty()) this->setPassthrough(window, true);

	if (!this->timer.isActive()) this->timer.start();
	this->poll();
}

void InputRegions::remove(QWindow* window) {
	if (!this->entries.contains(window)) return;
	this->setPassthrough(window, false);
	this->entries.remove(window);
	QObject::disconnect(window, &QObject::destroyed, this, nullptr);
	if (this->entries.isEmpty()) this->timer.stop();
}

void InputRegions::poll() {
	// Never flip input transparency in the middle of a drag or click.
	if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;

	POINT cursor;
	if (!GetCursorPos(&cursor)) return;

	for (auto [window, entry]: this->entries.asKeyValueRange()) {
		auto h = toHwnd(hwnd(window));
		if (!h || !window->isVisible()) continue;

		if (entry.region.isEmpty()) {
			this->setPassthrough(window, true);
			continue;
		}

		RECT rect;
		if (!GetWindowRect(h, &rect)) continue;
		if (!PtInRect(&rect, cursor)) continue;

		auto dpr = window->devicePixelRatio();
		auto local = QPoint(
		    static_cast<int>(std::floor((cursor.x - rect.left) / dpr)),
		    static_cast<int>(std::floor((cursor.y - rect.top) / dpr))
		);

		this->setPassthrough(window, !entry.region.contains(local));
	}
}

void InputRegions::setPassthrough(QWindow* window, bool passthrough) {
	auto& entry = this->entries[window];
	auto h = toHwnd(hwnd(window));
	if (!h) return;

	// Only the native mouse hit-testing style changes. Qt's WindowTransparentForInput would
	// also make Qt drop keyboard input, breaking text fields in partially click-through
	// windows (the overview search).
	auto ex = GetWindowLongW(h, GWL_EXSTYLE);
	auto want = passthrough ? (ex | WS_EX_TRANSPARENT | WS_EX_LAYERED) : (ex & ~WS_EX_TRANSPARENT);
	if (entry.passthrough == passthrough && ex == want) return;
	entry.passthrough = passthrough;
	if (ex == want) return;

	if (!(ex & WS_EX_LAYERED)) {
		SetWindowLongW(h, GWL_EXSTYLE, want);
		// A freshly layered window is invisible until it has layered attributes.
		SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
	} else {
		SetWindowLongW(h, GWL_EXSTYLE, want);
	}
}

// AppBar

namespace {

// What shell32's SHAppBarMessage sends to the taskbar window (WM_COPYDATA, dwData 0), as seen
// on Windows 11 24H2/25H2 (tools/sandbox/tests/appbar-capture2.ps1). Position queries carry
// their APPBARDATA in a shared memory block owned by the taskbar's process.
struct AppBarData3264 {
	DWORD cbSize;
	DWORD hWnd;
	UINT uCallbackMessage;
	UINT uEdge;
	RECT rc;
	LONGLONG lParam;
};

struct AppBarCommand {
	AppBarData3264 abd;
	DWORD dwMessage;
	DWORD padding1;
	ULONGLONG hShared;     // valid in dwSharedOwner
	DWORD dwSharedOwner;   // the taskbar's process
	DWORD padding2;
};

static_assert(sizeof(AppBarData3264) == 40);
static_assert(sizeof(AppBarCommand) == 64);
static_assert(offsetof(AppBarCommand, hShared) == 48);
static_assert(offsetof(AppBarCommand, dwSharedOwner) == 56);

// SHAppBarMessage, addressed to Explorer's taskbar. SHAppBarMessage itself goes to whichever
// Shell_TrayWnd FindWindow returns, which is the shell's own tray while it runs; from inside
// this process shell32 then hands over an in-process pointer that Explorer cannot read, and
// ABM_QUERYPOS/ABM_SETPOS silently do nothing. Sent the way it arrives from any other app.
UINT_PTR explorerAppBarMessage(DWORD message, APPBARDATA* data) {
	HWND tray = nullptr;
	DWORD explorer = 0;
	while ((tray = FindWindowExW(nullptr, tray, L"Shell_TrayWnd", nullptr))) {
		GetWindowThreadProcessId(tray, &explorer);
		if (explorer != GetCurrentProcessId()) break;
	}
	if (!tray) return SHAppBarMessage(message, data);

	AppBarCommand command {};
	command.abd.cbSize = sizeof(AppBarData3264);
	command.abd.hWnd = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(data->hWnd)); // NOLINT
	command.abd.uCallbackMessage = data->uCallbackMessage;
	command.abd.uEdge = data->uEdge;
	command.abd.rc = data->rc;
	command.abd.lParam = data->lParam;
	command.dwMessage = message;

	HANDLE shared = nullptr;
	if (message == ABM_QUERYPOS || message == ABM_SETPOS || message == ABM_GETTASKBARPOS) {
		shared = SHAllocShared(&command.abd, sizeof(command.abd), explorer);
		if (!shared) return SHAppBarMessage(message, data);
		command.hShared = reinterpret_cast<ULONG_PTR>(shared); // NOLINT
		command.dwSharedOwner = explorer;
	}

	COPYDATASTRUCT copy {};
	copy.dwData = 0;
	copy.cbData = sizeof(command);
	copy.lpData = &command;
	DWORD_PTR result = 0;
	auto sent = SendMessageTimeoutW(
	    tray,
	    WM_COPYDATA,
	    reinterpret_cast<WPARAM>(data->hWnd),
	    reinterpret_cast<LPARAM>(&copy),
	    SMTO_ABORTIFHUNG,
	    5000,
	    &result
	);

	if (shared) {
		if (sent) {
			if (auto* out = static_cast<AppBarData3264*>(SHLockShared(shared, explorer))) {
				data->rc = out->rc;
				SHUnlockShared(out);
			}
		}
		SHFreeShared(shared, explorer);
	}

	return sent ? result : 0;
}

} // namespace

AppBar::AppBar(QWindow* window): window(window) {
	this->watchdog.setInterval(3000);
	QObject::connect(&this->watchdog, &QTimer::timeout, this, &AppBar::verify);
}

AppBar::~AppBar() { this->unregisterBar(); }

void AppBar::update(Qt::Edge edge, qint32 thickness) {
	this->edge = edge;
	this->thickness = thickness;

	if (edge == 0 || thickness <= 0) {
		this->watchdog.stop();
		this->unregisterBar();
		return;
	}

	if (this->registerBar()) this->apply();
	this->misses = 0;
	this->watchdog.start();
}

bool AppBar::registerBar() {
	auto h = hwnd(this->window);
	if (this->registered && h == this->mHwnd) return true;
	if (this->registered) this->unregisterBar();
	if (!h) return false;

	APPBARDATA abd {};
	abd.cbSize = sizeof(abd);
	abd.hWnd = toHwnd(h);
	abd.uCallbackMessage = NativeEventRouter::appBarMessage();

	if (!explorerAppBarMessage(ABM_NEW, &abd)) {
		// Explorer busy, or it still has this window from before: clear it, the watchdog retries.
		explorerAppBarMessage(ABM_REMOVE, &abd);
		if (!this->warned) qCWarning(logAppBar) << "ABM_NEW failed for" << this->window << "- will retry";
		this->warned = true;
		return false;
	}

	this->mHwnd = h;
	this->registered = true;
	this->warned = false;
	NativeEventRouter::instance()->addAppBar(h, this);
	qCInfo(logAppBar) << "Registered appbar" << Qt::hex << h;
	return true;
}

bool AppBar::reservationHolds() const {
	auto* monitor = MonitorFromWindow(toHwnd(this->mHwnd), MONITOR_DEFAULTTONEAREST);
	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info)) return true;

	auto px = static_cast<LONG>(std::lround(this->thickness * this->window->devicePixelRatio()));
	const auto& work = info.rcWork;
	const auto& screen = info.rcMonitor;
	switch (this->edge) {
	case Qt::TopEdge: return work.top >= screen.top + px;
	case Qt::BottomEdge: return work.bottom <= screen.bottom - px;
	case Qt::LeftEdge: return work.left >= screen.left + px;
	case Qt::RightEdge: return work.right <= screen.right - px;
	default: return true;
	}
}

void AppBar::verify() {
	if (this->window == nullptr || this->edge == 0 || this->thickness <= 0) return;

	if (!this->registered) {
		if (this->registerBar()) this->apply();
		return;
	}

	// Explorer updates the work area asynchronously: only act on two misses in a row.
	if (this->reservationHolds()) {
		this->misses = 0;
		return;
	}
	if (++this->misses < 2) return;

	qCInfo(logAppBar) << "Appbar" << Qt::hex << this->mHwnd << "lost its reserved space, registering again";
	this->misses = 0;
	this->unregisterBar();
	if (this->registerBar()) this->apply();
}

void AppBar::unregisterBar() {
	if (!this->registered) return;

	APPBARDATA abd {};
	abd.cbSize = sizeof(abd);
	abd.hWnd = toHwnd(this->mHwnd);
	explorerAppBarMessage(ABM_REMOVE, &abd);

	NativeEventRouter::instance()->removeAppBar(this->mHwnd);
	qCInfo(logAppBar) << "Removed appbar" << Qt::hex << this->mHwnd;
	this->registered = false;
	this->mHwnd = 0;
	this->mReserved = QRect();
}

void AppBar::apply() {
	if (!this->registered || this->window == nullptr) return;

	auto* monitor = MonitorFromWindow(toHwnd(this->mHwnd), MONITOR_DEFAULTTONEAREST);
	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info)) return;

	auto px = static_cast<LONG>(std::lround(this->thickness * this->window->devicePixelRatio()));

	APPBARDATA abd {};
	abd.cbSize = sizeof(abd);
	abd.hWnd = toHwnd(this->mHwnd);
	abd.rc = info.rcMonitor;

	switch (this->edge) {
	case Qt::TopEdge: abd.uEdge = ABE_TOP; break;
	case Qt::BottomEdge: abd.uEdge = ABE_BOTTOM; break;
	case Qt::LeftEdge: abd.uEdge = ABE_LEFT; break;
	case Qt::RightEdge: abd.uEdge = ABE_RIGHT; break;
	default: return;
	}

	explorerAppBarMessage(ABM_QUERYPOS, &abd);

	// QUERYPOS may move the near edge away from other appbars; keep our thickness from there.
	switch (this->edge) {
	case Qt::TopEdge: abd.rc.bottom = abd.rc.top + px; break;
	case Qt::BottomEdge: abd.rc.top = abd.rc.bottom - px; break;
	case Qt::LeftEdge: abd.rc.right = abd.rc.left + px; break;
	case Qt::RightEdge: abd.rc.left = abd.rc.right - px; break;
	default: break;
	}

	explorerAppBarMessage(ABM_SETPOS, &abd);
	this->mReserved = toQRect(abd.rc);
	qCInfo(logAppBar) << "Appbar" << Qt::hex << this->mHwnd << Qt::dec << "reserves" << this->mReserved;
}

void AppBar::handleCallback(quintptr wParam, qintptr lParam) {
	switch (wParam) {
	case ABN_POSCHANGED: this->apply(); break;
	case ABN_FULLSCREENAPP: emit this->fullscreenAppChanged(lParam != 0); break;
	default: break;
	}
}

// SessionEvents

namespace {
LRESULT CALLBACK sessionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_WTSSESSION_CHANGE) SessionEvents::instance()->dispatch(static_cast<quint32>(wParam));
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

SessionEvents* SessionEvents::instance() {
	static auto* events = new SessionEvents(); // NOLINT
	return events;
}

SessionEvents::SessionEvents(QObject* parent): QObject(parent) {
	WNDCLASSW wc {};
	wc.lpfnWndProc = sessionWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"QuickshellSessionEvents";
	RegisterClassW(&wc);
	auto* h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	this->hwnd = reinterpret_cast<quintptr>(h);
	if (!WTSRegisterSessionNotification(h, NOTIFY_FOR_THIS_SESSION)) {
		qCWarning(logWin32) << "WTSRegisterSessionNotification failed" << GetLastError();
	}
}

void SessionEvents::dispatch(quint32 event) {
	if (event == WTS_SESSION_LOCK) {
		qCInfo(logWin32) << "Session locked";
		emit this->locked();
	} else if (event == WTS_SESSION_UNLOCK) {
		qCInfo(logWin32) << "Session unlocked";
		emit this->unlocked();
	}
}

// NativeEventRouter

NativeEventRouter* NativeEventRouter::instance() {
	static NativeEventRouter* router = nullptr; // NOLINT
	if (router == nullptr) {
		router = new NativeEventRouter();
		QCoreApplication::instance()->installNativeEventFilter(router);
	}
	return router;
}

quint32 NativeEventRouter::appBarMessage() {
	static auto message = RegisterWindowMessageW(L"QuickshellAppBarNotify");
	return message;
}

void NativeEventRouter::setDesktopCloseHandler(std::function<void()> handler) {
	instance()->desktopCloseHandler = std::move(handler);
}

void NativeEventRouter::addAppBar(quintptr hwnd, AppBar* bar) { this->appBars.insert(hwnd, bar); }
void NativeEventRouter::removeAppBar(quintptr hwnd) { this->appBars.remove(hwnd); }

namespace {
// A window that draws its own title bar: QML declares `property bool customTitleBar: true` on it
// (ii's settings app). It keeps a real Windows frame (shadow, rounded corners, Snap, resizable
// edges) but no caption: the client area extends over the caption, and the top edge resizes.
bool hasCustomTitleBar(HWND hwnd) {
	for (auto* window: QGuiApplication::topLevelWindows()) {
		// handle() first: winId() would create a window that is still being created.
		if (window->handle() == nullptr) continue;
		if (reinterpret_cast<HWND>(window->winId()) == hwnd) return window->property("customTitleBar").toBool(); // NOLINT
	}
	return false;
}

bool isShellSurface(HWND hwnd) {
	for (auto* window: QGuiApplication::topLevelWindows()) {
		if (window->handle() == nullptr) continue;
		if (reinterpret_cast<HWND>(window->winId()) == hwnd) return window->property("_qs_shellSurface").toBool(); // NOLINT
	}
	return false;
}

int resizeBorder(HWND hwnd) {
	auto dpi = GetDpiForWindow(hwnd);
	return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

bool customTitleBarMessage(MSG* msg, qintptr* result) {
	// The frame is computed during CreateWindowEx, before Qt knows the window: recompute it once
	// the window is about to be shown.
	if (msg->message == WM_SHOWWINDOW && msg->wParam == TRUE && hasCustomTitleBar(msg->hwnd)) {
		SetWindowPos(msg->hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		return false;
	}
	if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
		if (!hasCustomTitleBar(msg->hwnd)) return false;
		auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam); // NOLINT
		auto top = params->rgrc[0].top;
		*result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);
		// Left, right and bottom keep their borders; the caption goes. Maximized windows
		// overhang the screen by the border, which must not cut the top of the content.
		params->rgrc[0].top = top + (IsZoomed(msg->hwnd) ? resizeBorder(msg->hwnd) : 0);
		return true;
	}
	if (msg->message == WM_NCHITTEST) {
		if (!hasCustomTitleBar(msg->hwnd)) return false;
		auto hit = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, msg->lParam);
		if (hit == HTCLIENT && !IsZoomed(msg->hwnd)) {
			RECT r;
			GetWindowRect(msg->hwnd, &r);
			auto y = GET_Y_LPARAM(msg->lParam);
			auto x = GET_X_LPARAM(msg->lParam);
			auto border = resizeBorder(msg->hwnd);
			if (y < r.top + border) hit = x < r.left + 2 * border ? HTTOPLEFT : x >= r.right - 2 * border ? HTTOPRIGHT : HTTOP;
		}
		*result = hit;
		return true;
	}
	return false;
}
} // namespace

bool NativeEventRouter::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* result
) {
	if (eventType != "windows_generic_MSG") return false;
	auto* msg = static_cast<MSG*>(message);

	if (customTitleBarMessage(msg, result)) return true;

	// Shell surfaces (wallpaper, bar, dock, panels) are not windows the user closes: Alt+F4
	// on the focused desktop used to close the wallpaper. Alt+F4 there opens the shell's own
	// answer instead (ii: the session menu), as Windows shows "Shut Down Windows".
	auto isSysClose = msg->message == WM_SYSCOMMAND && (msg->wParam & 0xFFF0) == SC_CLOSE;
	if ((isSysClose || msg->message == WM_CLOSE) && isShellSurface(msg->hwnd)) {
		auto desktop = LayerManager::instance()->isDesktopLayer(reinterpret_cast<quintptr>(msg->hwnd)); // NOLINT
		qCInfo(logWin32) << "Ignored" << (isSysClose ? "SC_CLOSE" : "WM_CLOSE") << "on a shell surface"
		                 << (desktop ? "(desktop)" : "") << "result ptr:" << (result != nullptr);
		if (isSysClose && desktop && this->desktopCloseHandler) this->desktopCloseHandler();
		// Qt also filters messages it pulls from the queue itself, without a result to fill.
		if (result) *result = 0;
		return true;
	}

	// Activating a window (a click on the desktop gives it the keyboard) raises it to the top
	// of the normal band, over every app. Desktop-layer windows keep their place instead;
	// only our own restacking (an explicit window to insert after) moves them.
	if (msg->message == WM_WINDOWPOSCHANGING) {
		auto* pos = reinterpret_cast<WINDOWPOS*>(msg->lParam); // NOLINT
		if (!(pos->flags & SWP_NOZORDER) && pos->hwndInsertAfter == HWND_TOP
		    && !(GetWindowLongW(msg->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
		    && LayerManager::instance()->isDesktopLayer(reinterpret_cast<quintptr>(msg->hwnd)))
		{
			pos->flags |= SWP_NOZORDER;
		}
		return false;
	}

	if (msg->message == NativeEventRouter::appBarMessage()) {
		if (auto* bar = this->appBars.value(reinterpret_cast<quintptr>(msg->hwnd))) {
			bar->handleCallback(msg->wParam, msg->lParam);
			return true;
		}
	}

	return false;
}

} // namespace qs::win32
