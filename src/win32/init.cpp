#include <qcoreapplication.h>
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
