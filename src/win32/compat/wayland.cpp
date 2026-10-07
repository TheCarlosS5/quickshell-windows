#include "wayland.hpp"

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qqmllist.h>

#include <algorithm>
#include <cmath>

#include <qquickwindow.h>

#include <windows.h>
#include <dwmapi.h>

#include "../../core/logcat.hpp"
#include "../native.hpp"
#include "../panel_window.hpp"

namespace qs::win32::compat {

QS_LOGGING_CATEGORY(logCompat, "quickshell.win32.compat", QtWarningMsg);

// WlrLayershell

WlrLayershell::WlrLayershell(QObject* target): QObject(target), target(target) {}

WlrLayershell* WlrLayershell::qmlAttachedProperties(QObject* object) {
	return new WlrLayershell(object);
}

void WlrLayershell::setLayer(WlrLayer::Enum layer) {
	if (layer == this->mLayer) return;
	this->mLayer = layer;

	if (auto* panel = WinPanelWindow::forObject(this->target)) {
		panel->setLayer(static_cast<Layer>(layer));
	}

	emit this->layerChanged();
}

void WlrLayershell::setNamespace(QString ns) {
	if (ns == this->mNamespace) return;
	this->mNamespace = std::move(ns);
	emit this->namespaceChanged();
}

void WlrLayershell::setKeyboardFocus(WlrKeyboardFocus::Enum focus) {
	if (focus == this->mFocus) return;
	this->mFocus = focus;

	if (auto* panel = WinPanelWindow::forObject(this->target)) {
		panel->setKeyboardFocus(static_cast<quint8>(focus));
	}

	emit this->keyboardFocusChanged();
}

// ToplevelManager

ToplevelManagerQml::ToplevelManagerQml(QObject* parent): QObject(parent) {
	QObject::connect(
	    WindowTracker::instance(),
	    &WindowTracker::activeChanged,
	    this,
	    &ToplevelManagerQml::activeToplevelChanged
	);
}

ObjectModel<WindowHandle>* ToplevelManagerQml::toplevels() {
	return WindowTracker::instance()->windows();
}

WindowHandle* ToplevelManagerQml::activeToplevel() { return WindowTracker::instance()->active(); }

// ScreencopyView

ScreencopyView::ScreencopyView(QQuickItem* parent): QQuickItem(parent) {
	QObject::connect(this, &QQuickItem::visibleChanged, this, &ScreencopyView::updateThumbnail);
	QObject::connect(this, &QQuickItem::opacityChanged, this, &ScreencopyView::updateThumbnail);
}

ScreencopyView::~ScreencopyView() { this->unregisterThumbnail(); }

void ScreencopyView::setCaptureSource(QObject* source) {
	if (source == this->mSource) return;
	this->mSource = source;
	this->registerThumbnail();
	emit this->captureSourceChanged();
}

void ScreencopyView::itemChange(ItemChange change, const ItemChangeData& data) {
	QQuickItem::itemChange(change, data);
	if (change != ItemSceneChange) return;

	if (this->trackedWindow) QObject::disconnect(this->trackedWindow, nullptr, this, nullptr);
	this->trackedWindow = data.window;
	if (data.window) {
		// Follow the item every frame (animations move previews around in the overview).
		QObject::connect(data.window, &QQuickWindow::afterAnimating, this, &ScreencopyView::updateThumbnail);
		QObject::connect(data.window, &QWindow::visibleChanged, this, &ScreencopyView::updateThumbnail);
	}
	this->registerThumbnail();
}

void ScreencopyView::registerThumbnail() {
	auto* window = this->trackedWindow.data();
	auto* handle = qobject_cast<WindowHandle*>(this->mSource);
	auto target = window ? qs::win32::hwnd(window) : 0;
	auto source = handle ? static_cast<quintptr>(handle->hwnd()) : 0;

	this->unregisterThumbnail();
	if (!target || !source) return;

	HTHUMBNAIL thumb = nullptr;
	if (FAILED(DwmRegisterThumbnail(reinterpret_cast<HWND>(target), reinterpret_cast<HWND>(source), &thumb))) { // NOLINT
		return;
	}
	this->thumbnail = reinterpret_cast<quintptr>(thumb);
	this->thumbnailTarget = target;

	SIZE size {};
	if (SUCCEEDED(DwmQueryThumbnailSourceSize(thumb, &size))) {
		this->mSourceSize = QSize(size.cx, size.cy);
		emit this->sourceSizeChanged();
	}
	emit this->hasContentChanged();
	this->updateThumbnail();
}

void ScreencopyView::unregisterThumbnail() {
	if (!this->thumbnail) return;
	DwmUnregisterThumbnail(reinterpret_cast<HTHUMBNAIL>(this->thumbnail)); // NOLINT
	this->thumbnail = 0;
	this->thumbnailTarget = 0;
	emit this->hasContentChanged();
}

void ScreencopyView::updateThumbnail() {
	if (!this->thumbnail || !this->trackedWindow) return;
	auto* window = this->trackedWindow.data();

	// Effective visibility and opacity through the item's ancestors.
	auto visible = window->isVisible() && this->isVisible() && this->width() > 0 && this->height() > 0;
	qreal opacity = 1;
	for (const QQuickItem* item = this; item; item = item->parentItem()) opacity *= item->opacity();

	auto dpr = window->devicePixelRatio();
	auto topLeft = this->mapToScene(QPointF(0, 0));
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY;
	props.fVisible = visible && opacity > 0.01;
	props.opacity = static_cast<BYTE>(std::clamp(opacity, 0.0, 1.0) * 255);
	props.rcDestination = RECT {
	    static_cast<LONG>(std::lround(topLeft.x() * dpr)),
	    static_cast<LONG>(std::lround(topLeft.y() * dpr)),
	    static_cast<LONG>(std::lround((topLeft.x() + this->width()) * dpr)),
	    static_cast<LONG>(std::lround((topLeft.y() + this->height()) * dpr)),
	};
	DwmUpdateThumbnailProperties(reinterpret_cast<HTHUMBNAIL>(this->thumbnail), &props); // NOLINT
}

void ScreencopyView::setPaintCursor(bool paint) {
	if (paint == this->mPaintCursor) return;
	this->mPaintCursor = paint;
	emit this->paintCursorChanged();
}

void ScreencopyView::setLive(bool live) {
	if (live == this->mLive) return;
	this->mLive = live;
	emit this->liveChanged();
}

void ScreencopyView::setConstraintSize(QSizeF size) {
	if (size == this->mConstraint) return;
	this->mConstraint = size;
	emit this->constraintSizeChanged();
}

// IdleInhibitor

int IdleInhibitor::activeCount = 0; // NOLINT

IdleInhibitor::~IdleInhibitor() {
	if (this->mEnabled) {
		this->mEnabled = false;
		--IdleInhibitor::activeCount;
		this->apply();
	}
}

void IdleInhibitor::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	IdleInhibitor::activeCount += enabled ? 1 : -1;
	this->apply();
	emit this->enabledChanged();
}

void IdleInhibitor::setWindow(QObject* window) {
	if (window == this->mWindow) return;
	this->mWindow = window;
	emit this->windowChanged();
}

void IdleInhibitor::apply() {
	// ES_CONTINUOUS state is per-thread; all inhibitors live on the GUI thread.
	if (IdleInhibitor::activeCount > 0) {
		SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
	} else {
		SetThreadExecutionState(ES_CONTINUOUS);
	}
	qCInfo(logCompat) << "Idle inhibitors active:" << IdleInhibitor::activeCount;
}

// WlSessionLock

WlSessionLock::WlSessionLock(QObject* parent): QObject(parent) {
	QObject::connect(
	    SessionEvents::instance(),
	    &SessionEvents::unlocked,
	    this,
	    [this]() {
		    if (!this->mLocked) return;
		    this->mLocked = false;
		    emit this->lockStateChanged();
		    emit this->secureStateChanged();
		    emit this->windowsUnlocked();
	    }
	);
}

void WlSessionLock::setLocked(bool locked) {
	if (locked == this->mLocked) return;
	this->mLocked = locked;

	if (locked) {
		qCInfo(logCompat) << "Session lock requested, using the Windows lock screen";
		LockWorkStation();
	}

	emit this->lockStateChanged();
	emit this->secureStateChanged();
}

void WlSessionLock::setSurface(QQmlComponent* surface) {
	if (surface == this->mSurface) return;
	this->mSurface = surface;
	emit this->surfaceComponentChanged();
}

// WlSessionLockSurface

QQmlListProperty<QObject> WlSessionLockSurface::data() {
	return QQmlListProperty<QObject>(this, &this->mData);
}

} // namespace qs::win32::compat
