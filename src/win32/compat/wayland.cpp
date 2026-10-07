#include "wayland.hpp"

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qqmllist.h>

#include <windows.h>

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

void ScreencopyView::setCaptureSource(QObject* source) {
	if (source == this->mSource) return;
	this->mSource = source;
	emit this->captureSourceChanged();
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
