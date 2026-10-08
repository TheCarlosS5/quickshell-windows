#include "native.hpp"
#include <cmath>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qwindow.h>

#include <dwmapi.h>
#include <windows.h>
// clang-format off
#include <shellapi.h>
#include <wtsapi32.h>
// clang-format on

#include "../core/logcat.hpp"

namespace qs::win32 {

QS_LOGGING_CATEGORY(logWin32, "quickshell.win32", QtWarningMsg);

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
// The topmost visible window (z-order) that covers the whole monitor belongs to us?
bool fullscreenWindowIsOurs(quintptr monitor) {
	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(monitor), &info)) return false; // NOLINT

	struct Search {
		RECT monitor;
		DWORD pid = 0;
	} search {info.rcMonitor};

	EnumWindows(
	    [](HWND hwnd, LPARAM data) -> BOOL {
		    auto* s = reinterpret_cast<Search*>(data); // NOLINT
		    if (!IsWindowVisible(hwnd)) return TRUE;
		    DWORD cloaked = 0;
		    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		    if (cloaked) return TRUE;
		    RECT r;
		    GetWindowRect(hwnd, &r);
		    if (r.left <= s->monitor.left && r.top <= s->monitor.top && r.right >= s->monitor.right
		        && r.bottom >= s->monitor.bottom)
		    {
			    GetWindowThreadProcessId(hwnd, &s->pid);
			    return FALSE;
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search) // NOLINT
	);

	return search.pid == GetCurrentProcessId();
}

void CALLBACK onForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
	QMetaObject::invokeMethod(LayerManager::instance(), &LayerManager::restackDesktop, Qt::QueuedConnection);
}
} // namespace

LayerManager::LayerManager(QObject* parent): QObject(parent) {
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
	if (active == this->fullscreenMonitors.contains(monitor)) return;

	// Our own full-screen surfaces (overview, session menu) also trigger ABN_FULLSCREENAPP;
	// only real applications (games, video players) should push the shell aside.
	if (active && fullscreenWindowIsOurs(monitor)) {
		qCInfo(logWin32) << "Ignoring fullscreen notification for our own window";
		return;
	}
	qCInfo(logWin32) << "Fullscreen app" << (active ? "entered" : "left") << "monitor" << monitor;

	if (active) {
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
	} else {
		this->fullscreenMonitors.removeAll(monitor);
	}

	this->restack();
}

// InputRegions

InputRegions::InputRegions(QObject* parent): QObject(parent) {
	this->timer.setInterval(16);
	this->timer.setTimerType(Qt::PreciseTimer);
	QObject::connect(&this->timer, &QTimer::timeout, this, &InputRegions::poll);
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

AppBar::AppBar(QWindow* window): window(window) {}

AppBar::~AppBar() { this->unregisterBar(); }

void AppBar::update(Qt::Edge edge, qint32 thickness) {
	this->edge = edge;
	this->thickness = thickness;

	if (edge == 0 || thickness <= 0) {
		this->unregisterBar();
		return;
	}

	this->registerBar();
	this->apply();
}

void AppBar::registerBar() {
	auto h = hwnd(this->window);
	if (this->registered && h == this->mHwnd) return;
	if (this->registered) this->unregisterBar();
	if (!h) return;

	APPBARDATA abd {};
	abd.cbSize = sizeof(abd);
	abd.hWnd = toHwnd(h);
	abd.uCallbackMessage = NativeEventRouter::appBarMessage();

	if (!SHAppBarMessage(ABM_NEW, &abd)) {
		qCWarning(logWin32) << "ABM_NEW failed for" << this->window;
		return;
	}

	this->mHwnd = h;
	this->registered = true;
	NativeEventRouter::instance()->addAppBar(h, this);
	qCInfo(logWin32) << "Registered appbar" << Qt::hex << h;
}

void AppBar::unregisterBar() {
	if (!this->registered) return;

	APPBARDATA abd {};
	abd.cbSize = sizeof(abd);
	abd.hWnd = toHwnd(this->mHwnd);
	SHAppBarMessage(ABM_REMOVE, &abd);

	NativeEventRouter::instance()->removeAppBar(this->mHwnd);
	qCInfo(logWin32) << "Removed appbar" << Qt::hex << this->mHwnd;
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

	SHAppBarMessage(ABM_QUERYPOS, &abd);

	// QUERYPOS may move the near edge away from other appbars; keep our thickness from there.
	switch (this->edge) {
	case Qt::TopEdge: abd.rc.bottom = abd.rc.top + px; break;
	case Qt::BottomEdge: abd.rc.top = abd.rc.bottom - px; break;
	case Qt::LeftEdge: abd.rc.right = abd.rc.left + px; break;
	case Qt::RightEdge: abd.rc.left = abd.rc.right - px; break;
	default: break;
	}

	SHAppBarMessage(ABM_SETPOS, &abd);
	this->mReserved = toQRect(abd.rc);
	qCInfo(logWin32) << "Appbar" << Qt::hex << this->mHwnd << Qt::dec << "reserves" << this->mReserved;
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

void NativeEventRouter::addAppBar(quintptr hwnd, AppBar* bar) { this->appBars.insert(hwnd, bar); }
void NativeEventRouter::removeAppBar(quintptr hwnd) { this->appBars.remove(hwnd); }

bool NativeEventRouter::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* /*result*/
) {
	if (eventType != "windows_generic_MSG") return false;
	auto* msg = static_cast<MSG*>(message);

	if (msg->message == NativeEventRouter::appBarMessage()) {
		if (auto* bar = this->appBars.value(reinterpret_cast<quintptr>(msg->hwnd))) {
			bar->handleCallback(msg->wParam, msg->lParam);
			return true;
		}
	}

	return false;
}

} // namespace qs::win32
