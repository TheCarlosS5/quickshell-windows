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

	// Like Windows' own notifications, these keep out of the way of games and videos.
	static const QStringList yielding = {"quickshell:notificationPopup", "quickshell:screenCorners"};
	if (auto* panel = WinPanelWindow::forObject(this->target)) {
		panel->setYieldsToFullscreen(yielding.contains(this->mNamespace));
	}

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
	// Popups move their content between native windows as they open, without telling the items
	// in a way we can rely on: while there is a source, check the item's window and follow it.
	this->poll.setInterval(33);
	QObject::connect(&this->poll, &QTimer::timeout, this, [this]() {
		if (this->window() != this->trackedWindow.data()) this->trackWindow(this->window());
		else if (!this->thumbnail && this->trackedWindow) this->registerThumbnail();
		else this->updateThumbnail();
	});
	QObject::connect(this, &QQuickItem::visibleChanged, this, &ScreencopyView::updateThumbnail);
	QObject::connect(this, &QQuickItem::opacityChanged, this, &ScreencopyView::updateThumbnail);
}

ScreencopyView::~ScreencopyView() { this->unregisterThumbnail(); }

void ScreencopyView::setCaptureSource(QObject* source) {
	if (source == this->mSource) return;
	this->mSource = source;
	if (source) this->poll.start();
	else this->poll.stop();
	this->registerThumbnail();
	emit this->captureSourceChanged();
}

void ScreencopyView::itemChange(ItemChange change, const ItemChangeData& data) {
	QQuickItem::itemChange(change, data);
	if (change != ItemSceneChange) return;
	this->trackWindow(data.window);
}

void ScreencopyView::trackWindow(QQuickWindow* window) {
	if (this->trackedWindow) QObject::disconnect(this->trackedWindow, nullptr, this, nullptr);
	this->trackedWindow = window;
	if (window) {
		// Follow the item every frame (animations move previews around in the overview).
		QObject::connect(window, &QQuickWindow::afterAnimating, this, &ScreencopyView::updateThumbnail);
		// Not every change animates this window (a popup fading in through an ancestor's
		// opacity): follow every frame it draws too. Emitted on the render thread.
		QObject::connect(window, &QQuickWindow::frameSwapped, this, &ScreencopyView::updateThumbnail, Qt::QueuedConnection);
		// Popups (the dock's previews) get their native window only when first shown.
		QObject::connect(window, &QWindow::visibleChanged, this, [this]() {
			if (!this->thumbnail) this->registerThumbnail();
			else this->updateThumbnail();
		});
	}
	this->registerThumbnail();
}

void ScreencopyView::registerThumbnail() {
	auto* window = this->trackedWindow.data();
	auto* handle = qobject_cast<WindowHandle*>(this->mSource);
	auto target = window ? qs::win32::hwnd(window) : 0;
	auto source = handle ? static_cast<quintptr>(handle->hwnd()) : 0;

	this->unregisterThumbnail();

	// Known before DWM is involved, so a popup can size itself before it is shown.
	if (source) {
		auto* sourceHwnd = reinterpret_cast<HWND>(source); // NOLINT
		RECT rect {};
		WINDOWPLACEMENT placement {};
		placement.length = sizeof(placement);
		// A minimized window's rect is a tiny caption off-screen: use its restored size.
		auto ok = IsIconic(sourceHwnd) && GetWindowPlacement(sourceHwnd, &placement)
		            ? (rect = placement.rcNormalPosition, true)
		            : GetWindowRect(sourceHwnd, &rect) != 0;
		if (ok) {
			auto size = QSize(rect.right - rect.left, rect.bottom - rect.top);
			if (size.isValid() && size != this->mSourceSize) {
				this->mSourceSize = size;
				emit this->sourceSizeChanged();
				this->updateImplicitSize();
			}
		}
	}

	if (!target || !source) return;

	HTHUMBNAIL thumb = nullptr;
	auto hr = DwmRegisterThumbnail(reinterpret_cast<HWND>(target), reinterpret_cast<HWND>(source), &thumb); // NOLINT
	qCDebug(logCompat) << "thumbnail" << Qt::hex << source << "->" << target << "hr" << static_cast<quint32>(hr);
	if (FAILED(hr)) return;
	this->thumbnail = reinterpret_cast<quintptr>(thumb);
	this->thumbnailTarget = target;
	this->settled = false;

	SIZE size {};
	if (SUCCEEDED(DwmQueryThumbnailSourceSize(thumb, &size))) {
		this->mSourceSize = QSize(size.cx, size.cy);
		emit this->sourceSizeChanged();
		this->updateImplicitSize();
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
	// The native window's visibility: popups are shown by the backend without QWindow knowing.
	auto visible = IsWindowVisible(reinterpret_cast<HWND>(this->thumbnailTarget)) && this->isVisible() // NOLINT
	            && this->width() > 0 && this->height() > 0;
	qreal opacity = 1;
	for (const QQuickItem* item = this; item; item = item->parentItem()) opacity *= item->opacity();

	auto dpr = window->devicePixelRatio();
	auto full = this->mapRectToScene(QRectF(0, 0, this->width(), this->height()));

	// DWM draws the thumbnail over the window, outside Qt's rendering: it ignores the clipping
	// of ancestors with `clip: true` (a card animating its width would show the thumbnail
	// sticking out of it). Crop it to what those ancestors let through.
	auto shown = full;
	for (const QQuickItem* item = this->parentItem(); item; item = item->parentItem()) {
		if (item->clip()) shown &= item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
	}

	// A freshly registered thumbnail is first laid out with the size the item had before (e.g.
	// the previous app's whole row of previews in the dock): DWM would draw the window stretched
	// across it for a frame. Show it from the next frame on, and only while it is not distorted.
	auto undistorted = true;
	if (this->mSourceSize.isValid() && full.height() > 0 && this->mSourceSize.height() > 0) {
		auto ratio = (full.width() / full.height())
		           / (static_cast<qreal>(this->mSourceSize.width()) / this->mSourceSize.height());
		undistorted = ratio > 0.85 && ratio < 1.18;
	}
	auto firstFrame = !this->settled;
	this->settled = true;

	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY;
	props.fVisible = visible && opacity > 0.01 && !shown.isEmpty() && undistorted && !firstFrame;
	props.opacity = static_cast<BYTE>(std::clamp(opacity, 0.0, 1.0) * 255);
	props.rcDestination = RECT {
	    static_cast<LONG>(std::lround(shown.left() * dpr)),
	    static_cast<LONG>(std::lround(shown.top() * dpr)),
	    static_cast<LONG>(std::lround(shown.right() * dpr)),
	    static_cast<LONG>(std::lround(shown.bottom() * dpr)),
	};
	if (shown != full && !shown.isEmpty() && this->mSourceSize.isValid() && full.width() > 0 && full.height() > 0) {
		// The matching part of the source window, so the visible part keeps its scale.
		auto sx = this->mSourceSize.width() / full.width();
		auto sy = this->mSourceSize.height() / full.height();
		props.dwFlags |= DWM_TNP_RECTSOURCE;
		props.rcSource = RECT {
		    static_cast<LONG>(std::lround((shown.left() - full.left()) * sx)),
		    static_cast<LONG>(std::lround((shown.top() - full.top()) * sy)),
		    static_cast<LONG>(std::lround((shown.right() - full.left()) * sx)),
		    static_cast<LONG>(std::lround((shown.bottom() - full.top()) * sy)),
		};
	} else {
		// The whole source again (it may have been cropped before).
		props.dwFlags |= DWM_TNP_RECTSOURCE | DWM_TNP_SOURCECLIENTAREAONLY;
		props.fSourceClientAreaOnly = FALSE;
		props.rcSource = RECT {0, 0, this->mSourceSize.width(), this->mSourceSize.height()};
	}
	auto hr = DwmUpdateThumbnailProperties(reinterpret_cast<HTHUMBNAIL>(this->thumbnail), &props); // NOLINT
	auto rect = QRect(QPoint(props.rcDestination.left, props.rcDestination.top), QPoint(props.rcDestination.right, props.rcDestination.bottom));
	if (rect != this->lastRect || (props.fVisible != 0) != this->lastVisible || props.opacity != this->lastOpacity) {
		this->lastOpacity = props.opacity;
		qCDebug(logCompat) << "thumbnail" << Qt::hex << this->thumbnail << Qt::dec << "rect" << rect << "visible" << props.fVisible << "opacity" << props.opacity << "hr" << Qt::hex << static_cast<quint32>(hr);
		this->lastRect = rect;
		this->lastVisible = props.fVisible != 0;
	}
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
	this->updateImplicitSize();
}

// As on Wayland: the window's size, scaled down to fit the constraint (dock previews size
// themselves from this).
void ScreencopyView::updateImplicitSize() {
	auto size = this->mSourceSize.toSizeF();
	if (size.isEmpty()) return;
	auto constraint = this->mConstraint;
	if (constraint.width() > 0 && constraint.height() > 0) {
		size.scale(constraint, Qt::KeepAspectRatio);
	} else if (constraint.width() > 0) {
		size = QSizeF(constraint.width(), size.height() * constraint.width() / size.width());
	} else if (constraint.height() > 0) {
		size = QSizeF(size.width() * constraint.height() / size.height(), constraint.height());
	}
	this->setImplicitSize(size.width(), size.height());
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
