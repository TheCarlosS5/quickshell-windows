#include <algorithm>

#include <qcoreapplication.h>
#include <qevent.h>
#include <qscreen.h>
#include <qdir.h>
#include <qguiapplication.h>
#include <qtenvironmentvariables.h>
#include <qlogging.h>
#include <qqml.h>
#include <qquickwindow.h>
#include <qregion.h>
#include <qtimer.h>
#include <qwindow.h>

#include <windows.h>

#include "../core/plugin.hpp"
#include "../window/proxywindow.hpp"
#include "fonts.hpp"
#include "native.hpp"
#include "panel_window.hpp"

namespace {

// A graphics device that cannot be recreated (a driver reset under a heavy game) leaves a window that never draws
// again. Ending the process lets ii-host start a fresh shell with new windows, instead of keeping broken ones.
void watchSceneGraphs() {
	for (auto* window: QGuiApplication::topLevelWindows()) {
		auto* quick = qobject_cast<QQuickWindow*>(window);
		if (!quick || quick->property("_qsSceneGraphWatched").toBool()) continue;
		quick->setProperty("_qsSceneGraphWatched", true);
		QObject::connect(quick, &QQuickWindow::sceneGraphError, [](QQuickWindow::SceneGraphError error, const QString& message) {
			qCritical() << "scene graph error" << error << message << "- exiting so ii-host starts a new shell";
			QCoreApplication::exit(70);
		});
	}
}

// ii-host (II_HOST_HWND) is told once a second, from this GUI thread, that the shell is alive. A frozen GUI thread
// (with the basic render loop it also draws) stops it, and the host gives Windows' taskbar back and restarts the
// shell. II_TEST_FREEZE=<n> (tests only) freezes this thread on purpose after n beats.
void startHeartbeat() {
	const auto host = qEnvironmentVariable("II_HOST_HWND").toULongLong();
	if (host == 0) return;
	auto* hostHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(host)); // NOLINT
	const auto freezeAfter = qEnvironmentVariableIntValue("II_TEST_FREEZE");
	auto* timer = new QTimer(QCoreApplication::instance());
	timer->setInterval(1000);
	QObject::connect(timer, &QTimer::timeout, [hostHwnd, freezeAfter, beats = 0]() mutable {
		PostMessageW(hostHwnd, WM_APP + 5, GetCurrentProcessId(), 0); // ii-host's WM_II_HEARTBEAT
		watchSceneGraphs();
		if (freezeAfter > 0 && ++beats >= freezeAfter) {
			qWarning() << "II_TEST_FREEZE: freezing the GUI thread on purpose";
			for (;;) Sleep(1000);
		}
	});
	timer->start();
}

// Ordinary windows (settings, store, welcome, dialogs) ask for a size in device-independent pixels: at 150 % a
// 1100 x 750 window is taller than a 1080p screen's work area, Windows puts its title bar above the top edge and it
// can no longer be moved. The first time such a window is shown it is shrunk to its monitor's work area (its minimum
// size too, if that is what does not fit) and placed with its whole frame inside.
class WindowFitter: public QObject {
public:
	bool eventFilter(QObject* object, QEvent* event) override {
		if (event->type() != QEvent::Show || !object->isWindowType()) return false;
		auto* window = static_cast<QWindow*>(object); // NOLINT
		if (window->property("_qsFitted").toBool() || !window->isTopLevel() || window->property("_qs_shellSurface").toBool()) return false;
		auto type = window->type();
		if ((type != Qt::Window && type != Qt::Dialog) || window->flags().testFlag(Qt::FramelessWindowHint)) return false;
		window->setProperty("_qsFitted", true);
		auto* screen = window->screen();
		if (!screen) return false;
		const auto area = screen->availableGeometry();
		const auto frame = window->frameMargins(); // known once the native window exists (it does at Show)
		const int gap = 16;
		const int maxW = area.width() - frame.left() - frame.right() - 2 * gap;
		const int maxH = area.height() - frame.top() - frame.bottom() - 2 * gap;
		if (maxW < 200 || maxH < 200) return false;
		if (window->minimumWidth() > maxW || window->minimumHeight() > maxH) {
			window->setMinimumSize(QSize(std::min(window->minimumWidth(), maxW), std::min(window->minimumHeight(), maxH)));
		}
		auto size = QSize(std::min(window->width(), maxW), std::min(window->height(), maxH));
		if (size != window->size()) window->resize(size);
		// The whole frame inside the work area; centred when it was not.
		auto outer = QRect(window->x() - frame.left(), window->y() - frame.top(), size.width() + frame.left() + frame.right(),
		                   size.height() + frame.top() + frame.bottom());
		if (!area.contains(outer)) {
			outer.moveCenter(area.center());
			window->setPosition(outer.left() + frame.left(), outer.top() + frame.top());
		}
		return false;
	}
};

class Win32Plugin: public QsEnginePlugin {
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override {
		// Qt Quick's threaded render loop stops advancing animations in our layered,
		// click-through shell windows until some input arrives (verified in Windows Sandbox:
		// the overview stayed on its first frame until the mouse moved). The basic loop
		// drives animations from the GUI thread and renders them reliably.
		if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", "basic");

		// A shell is never the foreground app, so on battery Windows may throttle it as background
		// work (EcoQoS: efficiency cores, low clocks): hovers and animations then stutter. Opt out.
		PROCESS_POWER_THROTTLING_STATE throttling {};
		throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		throttling.StateMask = 0;
		SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

		// <exe>/bin holds Windows stand-ins for the small Linux commands configs call
		// (qalc, xdg-open, notify-send, ...); child processes resolve them through PATH.
		auto bin = QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("bin"));
		qputenv("PATH", (bin + ';' + qEnvironmentVariable("PATH")).toLocal8Bit());

		// Routes native messages from the start: windows with a custom frame need them even
		// in processes without panels (e.g. a config's settings app).
		qs::win32::NativeEventRouter::instance();

		startHeartbeat();
		QCoreApplication::instance()->installEventFilter(new WindowFitter()); // ordinary windows fit their screen

		qs::win32::loadBundledFonts();
		qs::win32::setupIconThemes();

		// QWindow::setMask is SetWindowRgn on Windows and would clip rendering;
		// route input masks through cursor-driven click-through instead.
		ProxyWindowBase::setInputMaskHandler([](QWindow* window, const QRegion& mask, bool hasMask) {
			qs::win32::InputRegions::instance()->setRegion(window, mask, hasMask);
		});
	}

	void registerTypes() override {
		qmlRegisterType<qs::win32::WinPanelInterface>("Quickshell._Win32Overlay", 1, 0, "PanelWindow");

		qmlRegisterModuleImport(
		    "Quickshell",
		    QQmlModuleImportModuleAny,
		    "Quickshell._Win32Overlay",
		    QQmlModuleImportLatest
		);
	}
};

QS_REGISTER_PLUGIN(Win32Plugin);

} // namespace
