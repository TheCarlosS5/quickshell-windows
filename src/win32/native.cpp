#include "native.hpp"
#include <cmath>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qwindow.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <windows.h>

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
	QObject::disconnect(window, nullptr, this, nullptr);
}

void LayerManager::apply(QWindow* window, Layer layer) {
	auto above = layer == Layer::Top || layer == Layer::Overlay;
	window->setFlag(Qt::WindowStaysOnTopHint, above);
	window->setFlag(Qt::WindowStaysOnBottomHint, !above);
}

void LayerManager::restack() {
	// Top first, then Overlay, so Overlay ends up at the very top of the topmost band.
	for (auto pass: {Layer::Top, Layer::Overlay}) {
		for (auto [window, layer]: this->layers.asKeyValueRange()) {
			if (layer != pass || !window->isVisible()) continue;
			auto h = toHwnd(hwnd(window));
			if (!h) continue;

			auto monitor = reinterpret_cast<quintptr>(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
			auto demoted = pass == Layer::Top && this->fullscreenMonitors.contains(monitor);

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

void LayerManager::setFullscreenAppActive(quintptr monitor, bool active) {
	if (active == this->fullscreenMonitors.contains(monitor)) return;
	qCInfo(logWin32) << "Fullscreen app" << (active ? "entered" : "left") << "monitor" << monitor;

	if (active) {
		this->fullscreenMonitors.append(monitor);

		// Leaving the topmost band needs an explicit HWND_NOTOPMOST first.
		for (auto [window, layer]: this->layers.asKeyValueRange()) {
			if (layer != Layer::Top) continue;
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
	if (entry.passthrough == passthrough && window->flags().testFlag(Qt::WindowTransparentForInput) == passthrough) {
		return;
	}

	entry.passthrough = passthrough;
	window->setFlag(Qt::WindowTransparentForInput, passthrough);
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
