#include "hyprland.hpp"

#include <string>

#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qprocess.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qwindow.h>

#include <windows.h>

#include "../../core/logcat.hpp"
#include "../../core/qmlglobal.hpp"
#include "../../window/proxywindow.hpp"
#include "../native.hpp"

namespace qs::win32::compat {

QS_LOGGING_CATEGORY(logHyprCompat, "quickshell.win32.hyprland", QtInfoMsg);

namespace {

// --- shortcut sink: ii-host delivers shortcuts here --------------------------------------

constexpr ULONG_PTR COPYDATA_PRESSED = 0x49490001;
constexpr ULONG_PTR COPYDATA_RELEASED = 0x49490002;

QList<GlobalShortcut*>& shortcuts() {
	static QList<GlobalShortcut*> list; // NOLINT
	return list;
}

LRESULT CALLBACK sinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_COPYDATA) {
		auto* data = reinterpret_cast<COPYDATASTRUCT*>(lParam); // NOLINT
		if (data->dwData == COPYDATA_PRESSED || data->dwData == COPYDATA_RELEASED) {
			auto name = QString::fromWCharArray(
			    static_cast<const wchar_t*>(data->lpData),
			    static_cast<qsizetype>(data->cbData / sizeof(wchar_t)) - 1
			);
			auto pressed = data->dwData == COPYDATA_PRESSED;
			// Deliver after returning so ii-host is never blocked on QML.
			QMetaObject::invokeMethod(
			    QCoreApplication::instance(),
			    [name, pressed]() { GlobalShortcut::dispatch(name, pressed); },
			    Qt::QueuedConnection
			);
			return TRUE;
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureSink() {
	static HWND sink = nullptr;
	if (sink) return;

	WNDCLASSW wc {};
	wc.lpfnWndProc = sinkWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"IiShellShortcutSink";
	RegisterClassW(&wc);
	// Top-level (hidden) rather than message-only so FindWindow from ii-host finds it.
	sink = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"ii shell shortcuts", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
	ChangeWindowMessageFilterEx(sink, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
	qCInfo(logHyprCompat) << "Shortcut sink ready";
}

// --- focus grabs ------------------------------------------------------------------------

QList<HyprlandFocusGrab*>& activeGrabs() {
	static QList<HyprlandFocusGrab*> list; // NOLINT
	return list;
}

HHOOK mouseHook = nullptr;

LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION
	    && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN))
	{
		auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam); // NOLINT
		auto* under = GetAncestor(WindowFromPoint(info->pt), GA_ROOT);
		auto hwnd = reinterpret_cast<quintptr>(under);

		for (auto* grab: activeGrabs()) {
			if (!grab->owns(hwnd)) {
				QMetaObject::invokeMethod(grab, &HyprlandFocusGrab::clearFromOutsideClick, Qt::QueuedConnection);
			}
		}
	}
	// Never swallow the click: it still goes wherever the user clicked.
	return CallNextHookEx(mouseHook, code, wParam, lParam);
}

void updateMouseHook() {
	if (!activeGrabs().isEmpty() && !mouseHook) {
		mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
	} else if (activeGrabs().isEmpty() && mouseHook) {
		UnhookWindowsHookEx(mouseHook);
		mouseHook = nullptr;
	}
}

HyprlandIpcQml* INSTANCE = nullptr; // NOLINT

} // namespace

// GlobalShortcut

GlobalShortcut::GlobalShortcut(QObject* parent): QObject(parent) {
	ensureSink();
	shortcuts().append(this);
}

GlobalShortcut::~GlobalShortcut() { shortcuts().removeAll(this); }

void GlobalShortcut::setAppid(QString appid) {
	if (appid == this->mAppid) return;
	this->mAppid = std::move(appid);
	emit this->appidChanged();
}

void GlobalShortcut::setName(QString name) {
	if (name == this->mName) return;
	this->mName = std::move(name);
	emit this->nameChanged();
}

void GlobalShortcut::setDescription(QString description) {
	if (description == this->mDescription) return;
	this->mDescription = std::move(description);
	emit this->descriptionChanged();
}

void GlobalShortcut::setTriggerDescription(QString description) {
	if (description == this->mTriggerDescription) return;
	this->mTriggerDescription = std::move(description);
	emit this->triggerDescriptionChanged();
}

void GlobalShortcut::trigger(bool pressed) {
	if (pressed == this->mPressed && pressed) {
		// Repeated press without release: report it again (key repeat).
		emit this->pressed();
		return;
	}
	this->mPressed = pressed;
	emit this->pressedChanged();
	if (pressed) emit this->pressed();
	else emit this->released();
}

int GlobalShortcut::dispatch(const QString& fullName, bool pressed) {
	// Accept "name" and "appid:name" (Hyprland's global dispatcher form).
	auto name = fullName;
	QString appid;
	if (auto colon = fullName.indexOf(':'); colon >= 0) {
		appid = fullName.left(colon);
		name = fullName.mid(colon + 1);
	}

	auto matched = 0;
	for (auto* shortcut: QList(shortcuts())) {
		if (shortcut->name() != name) continue;
		if (!appid.isEmpty() && shortcut->appid() != appid) continue;
		shortcut->trigger(pressed);
		++matched;
	}

	qCDebug(logHyprCompat) << "shortcut" << fullName << (pressed ? "pressed" : "released") << "->" << matched;
	return matched;
}

// HyprlandFocusGrab

HyprlandFocusGrab::~HyprlandFocusGrab() {
	activeGrabs().removeAll(this);
	updateMouseHook();
}

void HyprlandFocusGrab::setActive(bool active) {
	if (active == this->mActive) return;
	this->mActive = active;

	if (active) activeGrabs().append(this);
	else activeGrabs().removeAll(this);
	updateMouseHook();

	emit this->activeChanged();
}

void HyprlandFocusGrab::setWindows(QList<QObject*> windows) {
	this->mWindows = std::move(windows);
	emit this->windowsChanged();
}

bool HyprlandFocusGrab::owns(quintptr hwnd) const {
	for (auto* object: this->mWindows) {
		auto* proxy = ProxyWindowBase::forObject(object);
		if (!proxy) continue;
		if (qs::win32::hwnd(proxy->backingWindow()) == hwnd) return true;
	}
	return false;
}

void HyprlandFocusGrab::clearFromOutsideClick() {
	if (!this->mActive) return;
	this->setActive(false);
	emit this->cleared();
}

// HyprlandMonitor

HyprlandMonitor::HyprlandMonitor(QScreen* screen, qint32 id, QObject* parent)
    : QObject(parent)
    , mScreen(screen)
    , mId(id) {
	QObject::connect(screen, &QScreen::geometryChanged, this, &HyprlandMonitor::geometryChanged);
	QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, &HyprlandMonitor::geometryChanged);
}

// Hyprland names monitors after connectors (DP-1); Windows only has \\.\DISPLAYn.
QString HyprlandMonitor::name() const {
	return this->mScreen ? this->mScreen->name().remove("\\\\.\\") : QString();
}
QString HyprlandMonitor::description() const {
	return this->mScreen ? this->mScreen->manufacturer() + ' ' + this->mScreen->model() : QString();
}
qint32 HyprlandMonitor::x() const { return this->mScreen ? this->mScreen->geometry().x() : 0; }
qint32 HyprlandMonitor::y() const { return this->mScreen ? this->mScreen->geometry().y() : 0; }

// Hyprland reports physical pixels plus a scale.
qint32 HyprlandMonitor::width() const {
	return this->mScreen ? qRound(this->mScreen->geometry().width() * this->mScreen->devicePixelRatio()) : 0;
}
qint32 HyprlandMonitor::height() const {
	return this->mScreen ? qRound(this->mScreen->geometry().height() * this->mScreen->devicePixelRatio()) : 0;
}
qreal HyprlandMonitor::scale() const { return this->mScreen ? this->mScreen->devicePixelRatio() : 1.0; }

void HyprlandMonitor::setFocused(bool focused) {
	if (focused == this->mFocused) return;
	this->mFocused = focused;
	emit this->focusedChanged();
	emit this->lastIpcObjectChanged();
}

HyprlandWorkspace* HyprlandMonitor::activeWorkspace() const {
	return INSTANCE ? INSTANCE->focusedWorkspace() : nullptr;
}

QVariantMap HyprlandMonitor::lastIpcObject() const {
	// Hyprland's "reserved" = space taken by exclusive zones: the gap between the monitor
	// and its work area (our bar's appbar).
	QVariantList reserved {0, 0, 0, 0};
	if (this->mScreen) {
		struct Find {
			std::wstring device;
			MONITORINFOEXW info {};
			bool found = false;
		} find {this->mScreen->name().toStdWString()};
		EnumDisplayMonitors(
		    nullptr,
		    nullptr,
		    [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
			    auto* f = reinterpret_cast<Find*>(data); // NOLINT
			    MONITORINFOEXW info {};
			    info.cbSize = sizeof(info);
			    if (GetMonitorInfoW(monitor, &info) && f->device == info.szDevice) {
				    f->info = info;
				    f->found = true;
				    return FALSE;
			    }
			    return TRUE;
		    },
		    reinterpret_cast<LPARAM>(&find) // NOLINT
		);
		if (find.found) {
			const auto& m = find.info.rcMonitor;
			const auto& w = find.info.rcWork;
			reserved = QVariantList {
			    static_cast<int>(w.left - m.left),
			    static_cast<int>(w.top - m.top),
			    static_cast<int>(m.right - w.right),
			    static_cast<int>(m.bottom - w.bottom),
			};
		}
	}

	return {
	    {"id", this->mId},
	    {"name", this->name()},
	    {"description", this->description()},
	    {"x", this->x()},
	    {"y", this->y()},
	    {"width", this->width()},
	    {"height", this->height()},
	    {"scale", this->scale()},
	    {"focused", this->mFocused},
	    {"reserved", reserved},
	    {"activeWorkspace", QVariantMap {{"id", 1}, {"name", "1"}}},
	    {"specialWorkspace", QVariantMap {{"id", 0}, {"name", ""}}},
	    {"transform", 0},
	};
}

// HyprlandWorkspace

HyprlandMonitor* HyprlandWorkspace::monitor() const { return INSTANCE ? INSTANCE->focusedMonitor() : nullptr; }

UntypedObjectModel* HyprlandWorkspace::toplevels() { return WindowTracker::instance()->windows(); }

QVariantMap HyprlandWorkspace::lastIpcObject() const {
	return {
	    {"id", this->mId},
	    {"name", this->name()},
	    {"windows", WindowTracker::instance()->windows()->values().size()},
	};
}

// Hyprland

HyprlandIpcQml::HyprlandIpcQml(QObject* parent): QObject(parent) {
	INSTANCE = this;

	// Windows has one set of windows across all monitors: a single shared workspace.
	this->workspace = new HyprlandWorkspace(1, this);
	this->mWorkspaces.insertObject(this->workspace);

	QObject::connect(qGuiApp, &QGuiApplication::screenAdded, this, &HyprlandIpcQml::syncScreens);
	QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, this, &HyprlandIpcQml::syncScreens);
	QObject::connect(
	    WindowTracker::instance(),
	    &WindowTracker::activeChanged,
	    this,
	    &HyprlandIpcQml::activeToplevelChanged
	);
	this->syncScreens();

	// Hyprland's focused monitor follows the cursor.
	this->focusPoll.setInterval(250);
	QObject::connect(&this->focusPoll, &QTimer::timeout, this, &HyprlandIpcQml::updateFocusedMonitor);
	this->focusPoll.start();
}

HyprlandIpcQml* HyprlandIpcQml::instance() { return INSTANCE; }

void HyprlandIpcQml::syncScreens() {
	auto screens = QGuiApplication::screens();

	for (auto it = this->byScreen.begin(); it != this->byScreen.end();) {
		if (!screens.contains(it.key())) {
			this->mMonitors.removeObject(it.value());
			it.value()->deleteLater();
			it = this->byScreen.erase(it);
		} else {
			++it;
		}
	}

	for (auto* screen: screens) {
		if (this->byScreen.contains(screen)) continue;
		auto* monitor = new HyprlandMonitor(screen, this->nextMonitorId++, this);
		this->byScreen.insert(screen, monitor);
		this->mMonitors.insertObject(monitor);
	}

	this->updateFocusedMonitor();
}

void HyprlandIpcQml::updateFocusedMonitor() {
	POINT cursor;
	GetCursorPos(&cursor);
	MONITORINFOEXW info {};
	info.cbSize = sizeof(info);
	GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info);
	auto device = QString::fromWCharArray(info.szDevice);

	HyprlandMonitor* focused = nullptr;
	for (auto [screen, monitor]: this->byScreen.asKeyValueRange()) {
		if (screen->name() == device) focused = monitor;
	}
	if (!focused && !this->byScreen.isEmpty()) focused = this->monitorForScreen(QGuiApplication::primaryScreen());
	if (focused == this->mFocused) return;

	if (this->mFocused) this->mFocused->setFocused(false);
	this->mFocused = focused;
	if (focused) focused->setFocused(true);
	emit this->focusedMonitorChanged();
}

HyprlandMonitor* HyprlandIpcQml::monitorForScreen(QScreen* screen) const {
	return this->byScreen.value(screen);
}

HyprlandMonitor* HyprlandIpcQml::monitorFor(QuickshellScreenInfo* screen) {
	if (!screen) return nullptr;
	for (auto [qscreen, monitor]: this->byScreen.asKeyValueRange()) {
		if (QuickshellTracked::instance()->screenInfo(qscreen) == screen) return monitor;
	}
	return nullptr;
}

void HyprlandIpcQml::dispatch(const QString& request) {
	auto trimmed = request.trimmed();

	// Hyprland's newer Lua-style dispatchers, which current ii uses:
	//   hl.dsp.focus({window = "address:0x..."}), hl.dsp.window.close({...}), hl.dsp.global("name")
	if (trimmed.startsWith("hl.dsp.")) {
		auto open = trimmed.indexOf('(');
		auto name = trimmed.mid(7, open < 0 ? -1 : open - 7);
		auto body = open < 0 ? QString() : trimmed.mid(open + 1, trimmed.lastIndexOf(')') - open - 1);
		static const QRegularExpression addressRe(R"(address:(0x)?([0-9a-fA-F]+))");
		auto match = addressRe.match(body);
		auto address = match.hasMatch() ? static_cast<quintptr>(match.captured(2).toULongLong(nullptr, 16)) : 0;

		if (name == "focus" && address) {
			WindowTracker::forceForeground(address);
		} else if (name == "window.close" && address) {
			PostMessageW(reinterpret_cast<HWND>(address), WM_CLOSE, 0, 0); // NOLINT
		} else if (name == "global") {
			auto shortcut = body.trimmed();
			shortcut.remove('"');
			GlobalShortcut::dispatch(shortcut, true);
			GlobalShortcut::dispatch(shortcut, false);
		} else {
			// Workspaces, moving and pinning windows are window management; Windows keeps that.
			qCInfo(logHyprCompat) << "Ignoring unsupported dispatcher:" << request;
		}
		return;
	}

	auto space = trimmed.indexOf(' ');
	auto dispatcher = space < 0 ? trimmed : trimmed.left(space);
	auto args = space < 0 ? QString() : trimmed.mid(space + 1).trimmed();

	auto addressArg = [&]() -> quintptr {
		auto idx = args.indexOf("address:");
		if (idx < 0) return 0;
		return args.mid(idx + 8).section(' ', 0, 0).toULongLong(nullptr, 16);
	};

	if (dispatcher == "exec") {
		// Hyprland passes exec to a shell; cmd.exe is the Windows equivalent.
		QProcess::startDetached("cmd.exe", {"/c", args});
	} else if (dispatcher == "global") {
		GlobalShortcut::dispatch(args, true);
		GlobalShortcut::dispatch(args, false);
	} else if (dispatcher == "focuswindow") {
		if (auto hwnd = addressArg()) WindowTracker::forceForeground(hwnd);
	} else if (dispatcher == "closewindow") {
		if (auto hwnd = addressArg()) PostMessageW(reinterpret_cast<HWND>(hwnd), WM_CLOSE, 0, 0); // NOLINT
	} else if (dispatcher == "killactive") {
		if (auto* fg = GetForegroundWindow()) PostMessageW(fg, WM_CLOSE, 0, 0);
	} else {
		// Workspaces, layouts and window rules are window management; Windows keeps that.
		qCInfo(logHyprCompat) << "Ignoring unsupported dispatcher:" << request;
	}
}

} // namespace qs::win32::compat
