#include <qcoreapplication.h>
#include <qdir.h>
#include <qguiapplication.h>
#include <qtenvironmentvariables.h>
#include <qqml.h>
#include <qregion.h>
#include <qwindow.h>

#include "../core/plugin.hpp"
#include "../window/proxywindow.hpp"
#include "fonts.hpp"
#include "native.hpp"
#include "panel_window.hpp"

namespace {

class Win32Plugin: public QsEnginePlugin {
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override {
		// Qt Quick's threaded render loop stops advancing animations in our layered,
		// click-through shell windows until some input arrives (verified in Windows Sandbox:
		// the overview stayed on its first frame until the mouse moved). The basic loop
		// drives animations from the GUI thread and renders them reliably.
		if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", "basic");

		// <exe>/bin holds Windows stand-ins for the small Linux commands configs call
		// (qalc, xdg-open, notify-send, ...); child processes resolve them through PATH.
		auto bin = QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("bin"));
		qputenv("PATH", (bin + ';' + qEnvironmentVariable("PATH")).toLocal8Bit());

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
