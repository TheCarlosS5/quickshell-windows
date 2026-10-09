#pragma once

// Quickshell.Wayland for Windows: the subset of the Wayland module that shells use, mapped onto
// Windows equivalents so Wayland-targeting configs (illogical-impulse) load unchanged.

#include <qobject.h>
#include <qpointer.h>
#include <qproperty.h>
#include <qquickwindow.h>
#include <qqmlcomponent.h>
#include <qqmlintegration.h>
#include <qquickitem.h>
#include <qrect.h>
#include <qsize.h>
#include <qtimer.h>
#include <qtmetamacros.h>

#include "../../core/model.hpp"
#include "../../core/reload.hpp"
#include "../toplevels.hpp"

namespace qs::win32::compat {

namespace WlrLayer { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	Background = 0,
	Bottom = 1,
	Top = 2,
	Overlay = 3,
};
Q_ENUM_NS(Enum);

} // namespace WlrLayer

namespace WlrKeyboardFocus { // NOLINT
Q_NAMESPACE;
QML_ELEMENT;

enum Enum : quint8 {
	None = 0,
	Exclusive = 1,
	OnDemand = 2,
};
Q_ENUM_NS(Enum);

} // namespace WlrKeyboardFocus

// Windows' application windows, exposed under the Wayland module's `Toplevel` name.
struct ToplevelForeign {
	Q_GADGET;
	QML_FOREIGN(qs::win32::WindowHandle);
	QML_NAMED_ELEMENT(Toplevel);
	QML_UNCREATABLE("Toplevels must be acquired from the ToplevelManager.");
};

/// Attached to a PanelWindow: `WlrLayershell.layer`, `.namespace`, `.keyboardFocus`.
class WlrLayershell: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(qs::win32::compat::WlrLayer::Enum layer READ layer WRITE setLayer NOTIFY layerChanged);
	Q_PROPERTY(QString namespace READ ns WRITE setNamespace NOTIFY namespaceChanged);
	Q_PROPERTY(qs::win32::compat::WlrKeyboardFocus::Enum keyboardFocus READ keyboardFocus WRITE setKeyboardFocus NOTIFY keyboardFocusChanged);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("On Windows WlrLayershell is only available as an attached object.");
	QML_ATTACHED(WlrLayershell);

public:
	explicit WlrLayershell(QObject* target);

	static WlrLayershell* qmlAttachedProperties(QObject* object);

	[[nodiscard]] WlrLayer::Enum layer() const { return this->mLayer; }
	void setLayer(WlrLayer::Enum layer);

	[[nodiscard]] QString ns() const { return this->mNamespace; }
	void setNamespace(QString ns);

	[[nodiscard]] WlrKeyboardFocus::Enum keyboardFocus() const { return this->mFocus; }
	void setKeyboardFocus(WlrKeyboardFocus::Enum focus);

signals:
	void layerChanged();
	void namespaceChanged();
	void keyboardFocusChanged();

private:
	QObject* target;
	WlrLayer::Enum mLayer = WlrLayer::Top;
	QString mNamespace = "quickshell";
	WlrKeyboardFocus::Enum mFocus = WlrKeyboardFocus::None;
};

class ToplevelManagerQml: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	Q_PROPERTY(qs::win32::WindowHandle* activeToplevel READ activeToplevel NOTIFY activeToplevelChanged);
	// clang-format on
	QML_NAMED_ELEMENT(ToplevelManager);
	QML_SINGLETON;

public:
	explicit ToplevelManagerQml(QObject* parent = nullptr);

	[[nodiscard]] static ObjectModel<WindowHandle>* toplevels();
	[[nodiscard]] static WindowHandle* activeToplevel();

signals:
	void activeToplevelChanged();
};

/// Live view of a window, drawn by DWM (DwmRegisterThumbnail) over the item's area.
/// Screens are not supported yet. Note that DWM composites the thumbnail above the
/// window's own content, so items stacked over the view are covered by it.
class ScreencopyView: public QQuickItem {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QObject* captureSource READ captureSource WRITE setCaptureSource NOTIFY captureSourceChanged);
	Q_PROPERTY(bool paintCursor READ paintCursor WRITE setPaintCursor NOTIFY paintCursorChanged);
	Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged);
	Q_PROPERTY(bool hasContent READ hasContent NOTIFY hasContentChanged);
	Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY sourceSizeChanged);
	Q_PROPERTY(QSizeF constraintSize READ constraintSize WRITE setConstraintSize NOTIFY constraintSizeChanged);
	// clang-format on
	QML_ELEMENT;

public:
	explicit ScreencopyView(QQuickItem* parent = nullptr);
	~ScreencopyView() override;
	Q_DISABLE_COPY_MOVE(ScreencopyView);

	[[nodiscard]] QObject* captureSource() const { return this->mSource; }
	void setCaptureSource(QObject* source);
	[[nodiscard]] bool paintCursor() const { return this->mPaintCursor; }
	void setPaintCursor(bool paint);
	[[nodiscard]] bool live() const { return this->mLive; }
	void setLive(bool live);
	[[nodiscard]] bool hasContent() const { return this->thumbnail != 0; }
	[[nodiscard]] QSize sourceSize() const { return this->mSourceSize; }
	[[nodiscard]] QSizeF constraintSize() const { return this->mConstraint; }
	void setConstraintSize(QSizeF size);

	Q_INVOKABLE void captureFrame() {}

signals:
	void captureSourceChanged();
	void paintCursorChanged();
	void liveChanged();
	void hasContentChanged();
	void sourceSizeChanged();
	void constraintSizeChanged();

protected:
	void itemChange(ItemChange change, const ItemChangeData& data) override;

private slots:
	void updateThumbnail();

private:
	void registerThumbnail();
	void unregisterThumbnail();
	void updateImplicitSize();
	void trackWindow(QQuickWindow* window);

	QObject* mSource = nullptr;
	bool mPaintCursor = false;
	bool mLive = false;
	QSizeF mConstraint;
	QSize mSourceSize;
	quintptr thumbnail = 0;
	quintptr thumbnailTarget = 0;
	QPointer<QQuickWindow> trackedWindow;
	QTimer poll;
	QRect lastRect;
	bool settled = false; // laid out at least once since the thumbnail was registered
	bool lastVisible = false;
	quint8 lastOpacity = 0;
};

/// Keeps the display and system awake while enabled (SetThreadExecutionState).
class IdleInhibitor: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(QObject* window READ window WRITE setWindow NOTIFY windowChanged);
	QML_ELEMENT;

public:
	explicit IdleInhibitor(QObject* parent = nullptr): QObject(parent) {}
	~IdleInhibitor() override;
	Q_DISABLE_COPY_MOVE(IdleInhibitor);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);
	[[nodiscard]] QObject* window() const { return this->mWindow; }
	void setWindow(QObject* window);

signals:
	void enabledChanged();
	void windowChanged();

private:
	void apply();

	bool mEnabled = false;
	QObject* mWindow = nullptr;
	static int activeCount; // NOLINT
};

/// Session lock. Windows' own lock screen is the only secure one, so locking calls
/// LockWorkStation(); the QML lock surface is never shown. A lock the shell asks for gives its
/// lock animation a moment before Windows locks. windowsLocked() fires when Windows locks by
/// itself (Win+L, idle, lid) so the shell can show its locked state underneath, and
/// windowsUnlocked() when the user unlocks so the shell can leave it (with its animation).
class WlSessionLock: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool locked READ isLocked WRITE setLocked NOTIFY lockStateChanged);
	Q_PROPERTY(bool secure READ isSecure NOTIFY secureStateChanged);
	Q_PROPERTY(QQmlComponent* surface READ surface WRITE setSurface NOTIFY surfaceComponentChanged);
	Q_CLASSINFO("DefaultProperty", "surface");
	QML_ELEMENT;

public:
	explicit WlSessionLock(QObject* parent = nullptr);

	[[nodiscard]] bool isLocked() const { return this->mLocked; }
	void setLocked(bool locked);
	[[nodiscard]] bool isSecure() const { return this->mLocked; }
	[[nodiscard]] QQmlComponent* surface() const { return this->mSurface; }
	void setSurface(QQmlComponent* surface);

	Q_INVOKABLE void unlock() { this->setLocked(false); }

signals:
	void lockStateChanged();
	void secureStateChanged();
	void surfaceComponentChanged();
	void windowsUnlocked();
	void windowsLocked();

private:
	bool mLocked = false;
	bool sessionLocked = false; // Windows' lock screen is up
	QQmlComponent* mSurface = nullptr;
};

class WlSessionLockSurface: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged);
	Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged);
	Q_PROPERTY(QQmlListProperty<QObject> data READ data);
	Q_CLASSINFO("DefaultProperty", "data");
	QML_ELEMENT;

public:
	explicit WlSessionLockSurface(QObject* parent = nullptr): QObject(parent) {}

	[[nodiscard]] bool isVisible() const { return false; }
	[[nodiscard]] QColor color() const { return this->mColor; }
	void setColor(QColor color) {
		this->mColor = color;
		emit this->colorChanged();
	}
	QQmlListProperty<QObject> data();

signals:
	void visibleChanged();
	void colorChanged();

private:
	QColor mColor;
	QList<QObject*> mData;
};

} // namespace qs::win32::compat
