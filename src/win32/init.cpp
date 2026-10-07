#include <qguiapplication.h>
#include <qqml.h>
#include <qregion.h>
#include <qwindow.h>

#include "../core/plugin.hpp"
#include "../window/proxywindow.hpp"
#include "native.hpp"
#include "panel_window.hpp"

namespace {

class Win32Plugin: public QsEnginePlugin {
	QList<QString> dependencies() override { return {"window"}; }

	bool applies() override { return QGuiApplication::platformName() == "windows"; }

	void init() override {
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
