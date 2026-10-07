#pragma once

#include <memory>
#include <optional>

#include <qnamespace.h>
#include <qobject.h>
#include <qpointer.h>
#include <qproperty.h>
#include <qqmlintegration.h>
#include <qquickwindow.h>
#include <qscreen.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

#include "../core/doc.hpp"
#include "../core/util.hpp"
#include "../window/panelinterface.hpp"
#include "../window/proxywindow.hpp"
#include "native.hpp"

class EngineGeneration;

namespace qs::win32 {

class WinPanelWindow: public ProxyWindowBase {
	QSDOC_BASECLASS(PanelWindowInterface);
	Q_OBJECT;
	// clang-format off
	QSDOC_HIDE Q_PROPERTY(Anchors anchors READ anchors WRITE setAnchors NOTIFY anchorsChanged);
	QSDOC_HIDE Q_PROPERTY(qint32 exclusiveZone READ exclusiveZone WRITE setExclusiveZone NOTIFY exclusiveZoneChanged);
	QSDOC_HIDE Q_PROPERTY(ExclusionMode::Enum exclusionMode READ exclusionMode WRITE setExclusionMode NOTIFY exclusionModeChanged);
	QSDOC_HIDE Q_PROPERTY(Margins margins READ margins WRITE setMargins NOTIFY marginsChanged);
	QSDOC_HIDE Q_PROPERTY(bool aboveWindows READ aboveWindows WRITE setAboveWindows NOTIFY aboveWindowsChanged);
	QSDOC_HIDE Q_PROPERTY(bool focusable READ focusable WRITE setFocusable NOTIFY focusableChanged);
	// clang-format on
	QML_ELEMENT;

public:
	explicit WinPanelWindow(QObject* parent = nullptr);
	~WinPanelWindow() override;
	Q_DISABLE_COPY_MOVE(WinPanelWindow);

	static WinPanelWindow* forObject(QObject* obj);

	void connectWindow() override;
	void trySetWidth(qint32 implicitWidth) override;
	void trySetHeight(qint32 implicitHeight) override;
	void setScreen(QuickshellScreenInfo* screen) override;

	[[nodiscard]] bool aboveWindows() const { return this->bAboveWindows; }
	void setAboveWindows(bool aboveWindows) { this->bAboveWindows = aboveWindows; }

	[[nodiscard]] Anchors anchors() const { return this->bAnchors; }
	void setAnchors(Anchors anchors) { this->bAnchors = anchors; }

	[[nodiscard]] qint32 exclusiveZone() const { return this->bExclusiveZone; }
	void setExclusiveZone(qint32 exclusiveZone) {
		Qt::beginPropertyUpdateGroup();
		this->bExclusiveZone = exclusiveZone;
		this->bExclusionMode = ExclusionMode::Normal;
		Qt::endPropertyUpdateGroup();
	}

	[[nodiscard]] ExclusionMode::Enum exclusionMode() const { return this->bExclusionMode; }
	void setExclusionMode(ExclusionMode::Enum exclusionMode) { this->bExclusionMode = exclusionMode; }

	[[nodiscard]] Margins margins() const { return this->bMargins; }
	void setMargins(Margins margins) { this->bMargins = margins; }

	[[nodiscard]] bool focusable() const { return this->bFocusable; }
	void setFocusable(bool focusable) { this->bFocusable = focusable; }

	// Explicit layer from the layer-shell compatibility API; overrides aboveWindows.
	[[nodiscard]] Layer layer() const;
	void setLayer(Layer layer);

signals:
	QSDOC_HIDE void anchorsChanged();
	QSDOC_HIDE void exclusiveZoneChanged();
	QSDOC_HIDE void exclusionModeChanged();
	QSDOC_HIDE void marginsChanged();
	QSDOC_HIDE void aboveWindowsChanged();
	QSDOC_HIDE void focusableChanged();
	void layerChanged();

private slots:
	void updatePanelStack();
	void updateDimensionsSlot();
	void onFullscreenAppChanged(bool active);

private:
	void updateScreen();
	void updateExclusion(bool propagate = true);
	void updateExclusionCb() { this->updateExclusion(); }
	void updateLayer();
	void updateFocusable();
	void updateDimensions(bool propagate = true);
	void updateDimensionsCb() { this->updateDimensions(); }

	QPointer<QScreen> mTrackedScreen = nullptr;
	EngineGeneration* knownGeneration = nullptr;
	std::unique_ptr<AppBar> appBar;
	std::optional<Layer> mLayer;

	// clang-format off
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(WinPanelWindow, bool, bAboveWindows, true, &WinPanelWindow::aboveWindowsChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, bool, bFocusable, &WinPanelWindow::focusableChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, Anchors, bAnchors, &WinPanelWindow::anchorsChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, Margins, bMargins, &WinPanelWindow::marginsChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, qint32, bExclusiveZone, &WinPanelWindow::exclusiveZoneChanged);
	Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(WinPanelWindow, ExclusionMode::Enum, bExclusionMode, ExclusionMode::Auto, &WinPanelWindow::exclusionModeChanged);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, qint32, bcExclusiveZone);
	Q_OBJECT_BINDABLE_PROPERTY(WinPanelWindow, Qt::Edge, bcExclusionEdge);

	QS_BINDING_SUBSCRIBE_METHOD(WinPanelWindow, bAboveWindows, updateLayer, onValueChanged);
	QS_BINDING_SUBSCRIBE_METHOD(WinPanelWindow, bAnchors, updateDimensionsCb, onValueChanged);
	QS_BINDING_SUBSCRIBE_METHOD(WinPanelWindow, bMargins, updateDimensionsCb, onValueChanged);
	QS_BINDING_SUBSCRIBE_METHOD(WinPanelWindow, bcExclusiveZone, updateExclusionCb, onValueChanged);
	QS_BINDING_SUBSCRIBE_METHOD(WinPanelWindow, bFocusable, updateFocusable, onValueChanged);
	// clang-format on

	friend class WinPanelStack;
};

class WinPanelInterface: public PanelWindowInterface {
	Q_OBJECT;

public:
	explicit WinPanelInterface(QObject* parent = nullptr);

	void onReload(QObject* oldInstance) override;

	[[nodiscard]] ProxyWindowBase* proxyWindow() const override;

	[[nodiscard]] Anchors anchors() const override;
	void setAnchors(Anchors anchors) override;

	[[nodiscard]] Margins margins() const override;
	void setMargins(Margins margins) override;

	[[nodiscard]] qint32 exclusiveZone() const override;
	void setExclusiveZone(qint32 exclusiveZone) override;

	[[nodiscard]] ExclusionMode::Enum exclusionMode() const override;
	void setExclusionMode(ExclusionMode::Enum exclusionMode) override;

	[[nodiscard]] bool aboveWindows() const override;
	void setAboveWindows(bool aboveWindows) override;

	[[nodiscard]] bool focusable() const override;
	void setFocusable(bool focusable) override;

private:
	WinPanelWindow* panel;
};

} // namespace qs::win32
