#include "panel_window.hpp"
#include <map>

#include <qlist.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlengine.h>
#include <qquickwindow.h>
#include <qscreen.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include <windows.h>

#include "../core/generation.hpp"
#include "../window/panelinterface.hpp"
#include "../window/proxywindow.hpp"
#include "native.hpp"

namespace qs::win32 {

// Panels of one engine generation in show order. As in the X11 backend, each panel's
// geometry is shrunk by the exclusive zones of the panels shown before it.
class WinPanelStack {
public:
	static WinPanelStack* instance() {
		static auto* stack = new WinPanelStack(); // NOLINT
		return stack;
	}

	[[nodiscard]] const QList<WinPanelWindow*>& panels(WinPanelWindow* panel) {
		return this->mPanels[panel->knownGeneration];
	}

	void addPanel(WinPanelWindow* panel) {
		panel->knownGeneration = EngineGeneration::findObjectGeneration(panel);
		auto& panels = this->mPanels[panel->knownGeneration];
		if (!panels.contains(panel)) panels.push_back(panel);
	}

	void removePanel(WinPanelWindow* panel) {
		if (!panel->knownGeneration) return;
		auto* generation = panel->knownGeneration;
		auto& panels = this->mPanels[generation];

		if (panels.removeOne(panel)) {
			if (panels.isEmpty()) {
				this->mPanels.erase(generation);
				return;
			}

			for (auto* other: panels) other->updateDimensions(false);
		}
	}

	void updateLowerDimensions(WinPanelWindow* exclude) {
		if (!exclude->knownGeneration) return;
		auto found = false;
		for (auto* panel: this->mPanels[exclude->knownGeneration]) {
			if (panel == exclude) found = true;
			else if (found) panel->updateDimensions(false);
		}
	}

private:
	std::map<EngineGeneration*, QList<WinPanelWindow*>> mPanels;
};

WinPanelWindow::WinPanelWindow(QObject* parent): ProxyWindowBase(parent) {
	this->bcExclusiveZone.setBinding([this]() -> qint32 {
		switch (this->bExclusionMode.value()) {
		case ExclusionMode::Ignore: return 0;
		case ExclusionMode::Normal: return this->bExclusiveZone;
		case ExclusionMode::Auto:
			auto edge = this->bcExclusionEdge.value();
			auto margins = this->bMargins.value();

			if (edge == Qt::TopEdge || edge == Qt::BottomEdge) {
				return this->bImplicitHeight + margins.top + margins.bottom;
			} else if (edge == Qt::LeftEdge || edge == Qt::RightEdge) {
				return this->bImplicitWidth + margins.left + margins.right;
			} else {
				return 0;
			}
		}

		return 0;
	});

	this->bcExclusionEdge.setBinding([this] { return this->bAnchors.value().exclusionEdge(); });
}

WinPanelWindow::~WinPanelWindow() {
	this->appBar.reset();
	WinPanelStack::instance()->removePanel(this);
}

WinPanelWindow* WinPanelWindow::forObject(QObject* obj) {
	if (auto* panel = qobject_cast<WinPanelWindow*>(obj)) return panel;
	if (auto* iface = qobject_cast<WinPanelInterface*>(obj)) {
		return qobject_cast<WinPanelWindow*>(iface->proxyWindow());
	}
	return nullptr;
}

void WinPanelWindow::connectWindow() {
	this->ProxyWindowBase::connectWindow();

	// The native window must exist for styles, the appbar and stacking.
	this->window->create();
	applyShellWindowStyle(this->window, this->bFocusable);

	this->appBar = std::make_unique<AppBar>(this->window);
	QObject::connect(
	    this->appBar.get(),
	    &AppBar::fullscreenAppChanged,
	    this,
	    &WinPanelWindow::onFullscreenAppChanged
	);

	QObject::connect(
	    this->window,
	    &QQuickWindow::visibleChanged,
	    this,
	    &WinPanelWindow::updatePanelStack
	);

	this->updateScreen();
	this->updateLayer();
	this->updatePanelStack();
}

void WinPanelWindow::trySetWidth(qint32 implicitWidth) {
	if (!this->bAnchors.value().horizontalConstraint()) {
		this->ProxyWindowBase::trySetWidth(implicitWidth);
		this->updateDimensions();
	}
}

void WinPanelWindow::trySetHeight(qint32 implicitHeight) {
	if (!this->bAnchors.value().verticalConstraint()) {
		this->ProxyWindowBase::trySetHeight(implicitHeight);
		this->updateDimensions();
	}
}

void WinPanelWindow::setScreen(QuickshellScreenInfo* screen) {
	this->ProxyWindowBase::setScreen(screen);
	this->updateScreen();
}

Layer WinPanelWindow::layer() const {
	if (this->mLayer) return *this->mLayer;
	return this->bAboveWindows ? Layer::Top : Layer::Bottom;
}

void WinPanelWindow::setLayer(Layer layer) {
	if (this->mLayer == layer) return;
	this->mLayer = layer;
	this->updateLayer();
	emit this->layerChanged();
}

void WinPanelWindow::updateLayer() {
	if (this->window == nullptr) return;
	LayerManager::instance()->setLayer(this->window, this->layer());
}

void WinPanelWindow::updateFocusable() {
	if (this->window == nullptr) return;
	applyShellWindowStyle(this->window, this->bFocusable);
}

void WinPanelWindow::updateScreen() {
	auto* newScreen =
	    this->mScreen ? this->mScreen : (this->window ? this->window->screen() : nullptr);

	if (newScreen == this->mTrackedScreen) return;

	if (this->mTrackedScreen != nullptr) {
		QObject::disconnect(this->mTrackedScreen, nullptr, this, nullptr);
	}

	this->mTrackedScreen = newScreen;

	if (this->mTrackedScreen != nullptr) {
		QObject::connect(
		    this->mTrackedScreen,
		    &QScreen::geometryChanged,
		    this,
		    &WinPanelWindow::updateDimensionsSlot
		);
	}

	this->updateDimensions();
}

void WinPanelWindow::updateDimensionsSlot() { this->updateDimensions(); }

void WinPanelWindow::updateDimensions(bool propagate) {
	if (this->window == nullptr || this->window->handle() == nullptr
	    || this->mTrackedScreen == nullptr)
		return;

	auto screenGeometry = this->mTrackedScreen->geometry();

	if (this->bExclusionMode != ExclusionMode::Ignore) {
		for (auto* panel: WinPanelStack::instance()->panels(this)) {
			if (panel == this) break;
			if (panel->layer() != this->layer()) continue;
			if (panel->mTrackedScreen != this->mTrackedScreen) continue;

			auto edge = panel->bcExclusionEdge.value();
			auto exclusiveZone = panel->bcExclusiveZone.value();

			screenGeometry.adjust(
			    edge == Qt::LeftEdge ? exclusiveZone : 0,
			    edge == Qt::TopEdge ? exclusiveZone : 0,
			    edge == Qt::RightEdge ? -exclusiveZone : 0,
			    edge == Qt::BottomEdge ? -exclusiveZone : 0
			);
		}
	}

	auto geometry = QRect();
	auto anchors = this->bAnchors.value();
	auto margins = this->bMargins.value();

	if (anchors.horizontalConstraint()) {
		geometry.setX(screenGeometry.x() + margins.left);
		geometry.setWidth(screenGeometry.width() - margins.left - margins.right);
	} else {
		if (anchors.mLeft) {
			geometry.setX(screenGeometry.x() + margins.left);
		} else if (anchors.mRight) {
			geometry.setX(
			    screenGeometry.x() + screenGeometry.width() - this->implicitWidth() - margins.right
			);
		} else {
			geometry.setX(screenGeometry.x() + screenGeometry.width() / 2 - this->implicitWidth() / 2);
		}

		geometry.setWidth(this->implicitWidth());
	}

	if (anchors.verticalConstraint()) {
		geometry.setY(screenGeometry.y() + margins.top);
		geometry.setHeight(screenGeometry.height() - margins.top - margins.bottom);
	} else {
		if (anchors.mTop) {
			geometry.setY(screenGeometry.y() + margins.top);
		} else if (anchors.mBottom) {
			geometry.setY(
			    screenGeometry.y() + screenGeometry.height() - this->implicitHeight() - margins.bottom
			);
		} else {
			geometry.setY(screenGeometry.y() + screenGeometry.height() / 2 - this->implicitHeight() / 2);
		}

		geometry.setHeight(this->implicitHeight());
	}

	this->window->setGeometry(geometry);
	this->updateExclusion(propagate);
}

void WinPanelWindow::updatePanelStack() {
	if (this->window->isVisible()) {
		WinPanelStack::instance()->addPanel(this);
	} else {
		WinPanelStack::instance()->removePanel(this);
	}

	this->updateExclusion();
}

void WinPanelWindow::updateExclusion(bool propagate) {
	if (this->appBar == nullptr) return;

	auto visible = this->window != nullptr && this->window->isVisible();
	auto zone = this->bcExclusiveZone.value();

	// Only the topmost-band panels reserve desktop space; a panel under normal
	// windows reserving space would just leave a hole.
	auto reserves = visible && zone > 0 && this->bExclusionMode != ExclusionMode::Ignore
	             && (this->layer() == Layer::Top || this->layer() == Layer::Overlay);

	if (reserves) {
		// Each panel reserves only its own zone; Windows stacks appbars sharing an edge
		// (ABM_QUERYPOS moves ours past the ones registered before it).
		this->appBar->update(this->bcExclusionEdge.value(), zone);
	} else {
		this->appBar->update(static_cast<Qt::Edge>(0), 0);
	}

	if (propagate) WinPanelStack::instance()->updateLowerDimensions(this);
}

void WinPanelWindow::onFullscreenAppChanged(bool active) {
	auto h = hwnd(this->window);
	if (!h) return;
	auto monitor = MonitorFromWindow(reinterpret_cast<HWND>(h), MONITOR_DEFAULTTONEAREST); // NOLINT
	LayerManager::instance()->setFullscreenAppActive(reinterpret_cast<quintptr>(monitor), active);
}

// WinPanelInterface

WinPanelInterface::WinPanelInterface(QObject* parent)
    : PanelWindowInterface(parent)
    , panel(new WinPanelWindow(this)) {
	this->connectSignals();

	// clang-format off
	QObject::connect(this->panel, &WinPanelWindow::anchorsChanged, this, &WinPanelInterface::anchorsChanged);
	QObject::connect(this->panel, &WinPanelWindow::marginsChanged, this, &WinPanelInterface::marginsChanged);
	QObject::connect(this->panel, &WinPanelWindow::exclusiveZoneChanged, this, &WinPanelInterface::exclusiveZoneChanged);
	QObject::connect(this->panel, &WinPanelWindow::exclusionModeChanged, this, &WinPanelInterface::exclusionModeChanged);
	QObject::connect(this->panel, &WinPanelWindow::aboveWindowsChanged, this, &WinPanelInterface::aboveWindowsChanged);
	QObject::connect(this->panel, &WinPanelWindow::focusableChanged, this, &WinPanelInterface::focusableChanged);
	// clang-format on
}

void WinPanelInterface::onReload(QObject* oldInstance) {
	QQmlEngine::setContextForObject(this->panel, QQmlEngine::contextForObject(this));

	auto* old = qobject_cast<WinPanelInterface*>(oldInstance);
	this->panel->reload(old != nullptr ? old->panel : nullptr);
}

ProxyWindowBase* WinPanelInterface::proxyWindow() const { return this->panel; }

// NOLINTBEGIN
#define proxyPair(type, get, set)                                                                  \
	type WinPanelInterface::get() const { return this->panel->get(); }                               \
	void WinPanelInterface::set(type value) { this->panel->set(value); }

proxyPair(Anchors, anchors, setAnchors);
proxyPair(Margins, margins, setMargins);
proxyPair(qint32, exclusiveZone, setExclusiveZone);
proxyPair(ExclusionMode::Enum, exclusionMode, setExclusionMode);
proxyPair(bool, focusable, setFocusable);
proxyPair(bool, aboveWindows, setAboveWindows);

#undef proxyPair
// NOLINTEND

} // namespace qs::win32
