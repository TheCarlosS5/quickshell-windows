#pragma once

// Quickshell.Hyprland for Windows. Hyprland itself does not exist here; this provides the
// pieces shells rely on with Windows meaning:
//  - GlobalShortcut: fired by ii-host's keyboard hook through a WM_COPYDATA sink window
//  - HyprlandFocusGrab: "click outside closes the popup", via a mouse hook while active
//  - Hyprland: monitors = screens, workspaces = Windows' virtual desktops (shared by all
//    monitors, as Windows does), dispatch() for common dispatchers

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qvariant.h>

#include "../../core/model.hpp"
#include "../../core/qmlscreen.hpp"
#include "../toplevels.hpp"

class QScreen;

namespace qs::win32::compat {

class GlobalShortcut: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool pressed READ isPressed NOTIFY pressedChanged);
	Q_PROPERTY(QString appid READ appid WRITE setAppid NOTIFY appidChanged);
	Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged);
	Q_PROPERTY(QString description READ description WRITE setDescription NOTIFY descriptionChanged);
	Q_PROPERTY(QString triggerDescription READ triggerDescription WRITE setTriggerDescription NOTIFY triggerDescriptionChanged);
	// clang-format on
	QML_ELEMENT;

public:
	explicit GlobalShortcut(QObject* parent = nullptr);
	~GlobalShortcut() override;
	Q_DISABLE_COPY_MOVE(GlobalShortcut);

	[[nodiscard]] bool isPressed() const { return this->mPressed; }
	[[nodiscard]] QString appid() const { return this->mAppid; }
	void setAppid(QString appid);
	[[nodiscard]] QString name() const { return this->mName; }
	void setName(QString name);
	[[nodiscard]] QString description() const { return this->mDescription; }
	void setDescription(QString description);
	[[nodiscard]] QString triggerDescription() const { return this->mTriggerDescription; }
	void setTriggerDescription(QString description);

	void trigger(bool pressed);

	// Delivers a shortcut by name to every matching GlobalShortcut. Returns how many matched.
	static int dispatch(const QString& name, bool pressed);

signals:
	void pressed();
	void released();
	void pressedChanged();
	void appidChanged();
	void nameChanged();
	void descriptionChanged();
	void triggerDescriptionChanged();

private:
	bool mPressed = false;
	QString mAppid = "quickshell";
	QString mName;
	QString mDescription;
	QString mTriggerDescription;
};

class HyprlandFocusGrab: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged);
	Q_PROPERTY(QList<QObject*> windows READ windows WRITE setWindows NOTIFY windowsChanged);
	QML_ELEMENT;

public:
	explicit HyprlandFocusGrab(QObject* parent = nullptr): QObject(parent) {}
	~HyprlandFocusGrab() override;
	Q_DISABLE_COPY_MOVE(HyprlandFocusGrab);

	[[nodiscard]] bool isActive() const { return this->mActive; }
	void setActive(bool active);
	[[nodiscard]] QList<QObject*> windows() const { return this->mWindows; }
	void setWindows(QList<QObject*> windows);

	// True if the native window under a click belongs to this grab.
	[[nodiscard]] bool owns(quintptr hwnd) const;
	void clearFromOutsideClick();

signals:
	void activeChanged();
	void windowsChanged();
	void cleared();

private:
	bool mActive = false;
	QList<QObject*> mWindows;
};

class HyprlandWorkspace;

class HyprlandMonitor: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(qint32 id READ id CONSTANT);
	Q_PROPERTY(QString name READ name CONSTANT);
	Q_PROPERTY(QString description READ description CONSTANT);
	Q_PROPERTY(qint32 x READ x NOTIFY geometryChanged);
	Q_PROPERTY(qint32 y READ y NOTIFY geometryChanged);
	Q_PROPERTY(qint32 width READ width NOTIFY geometryChanged);
	Q_PROPERTY(qint32 height READ height NOTIFY geometryChanged);
	Q_PROPERTY(qreal scale READ scale NOTIFY geometryChanged);
	Q_PROPERTY(bool focused READ focused NOTIFY focusedChanged);
	Q_PROPERTY(qs::win32::compat::HyprlandWorkspace* activeWorkspace READ activeWorkspace NOTIFY activeWorkspaceChanged);
	Q_PROPERTY(QVariantMap lastIpcObject READ lastIpcObject NOTIFY lastIpcObjectChanged);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("HyprlandMonitors must be retrieved from the Hyprland object.");

public:
	HyprlandMonitor(QScreen* screen, qint32 id, QObject* parent);

	[[nodiscard]] qint32 id() const { return this->mId; }
	[[nodiscard]] QString name() const;
	[[nodiscard]] QString description() const;
	[[nodiscard]] qint32 x() const;
	[[nodiscard]] qint32 y() const;
	[[nodiscard]] qint32 width() const;
	[[nodiscard]] qint32 height() const;
	[[nodiscard]] qreal scale() const;
	[[nodiscard]] bool focused() const { return this->mFocused; }
	[[nodiscard]] HyprlandWorkspace* activeWorkspace() const;
	[[nodiscard]] QVariantMap lastIpcObject() const;
	[[nodiscard]] QScreen* screen() const { return this->mScreen; }

	void setFocused(bool focused);

signals:
	void geometryChanged();
	void focusedChanged();
	void activeWorkspaceChanged();
	void lastIpcObjectChanged();

private:
	QPointer<QScreen> mScreen;
	qint32 mId;
	bool mFocused = false;
};

class HyprlandWorkspace: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(qint32 id READ id CONSTANT);
	Q_PROPERTY(QString name READ name NOTIFY nameChanged);
	Q_PROPERTY(bool active READ active NOTIFY activeChanged);
	Q_PROPERTY(bool focused READ focused NOTIFY activeChanged);
	Q_PROPERTY(bool urgent READ urgent CONSTANT);
	Q_PROPERTY(qs::win32::compat::HyprlandMonitor* monitor READ monitor NOTIFY monitorChanged);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	Q_PROPERTY(QVariantMap lastIpcObject READ lastIpcObject CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("HyprlandWorkspaces must be retrieved from the Hyprland object.");

public:
	HyprlandWorkspace(qint32 id, QObject* parent): QObject(parent), mId(id) {}

	[[nodiscard]] qint32 id() const { return this->mId; }
	[[nodiscard]] QString name() const;
	[[nodiscard]] bool active() const;
	[[nodiscard]] bool focused() const { return this->active(); }
	[[nodiscard]] bool urgent() const { return false; }
	[[nodiscard]] HyprlandMonitor* monitor() const;
	[[nodiscard]] static UntypedObjectModel* toplevels();
	[[nodiscard]] QVariantMap lastIpcObject() const;

	// Switches to this desktop.
	Q_INVOKABLE void activate();

signals:
	void monitorChanged();
	void nameChanged();
	void activeChanged();

private:
	qint32 mId;
};

class HyprlandIpcQml: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QString requestSocketPath READ empty CONSTANT);
	Q_PROPERTY(QString eventSocketPath READ empty CONSTANT);
	Q_PROPERTY(qs::win32::compat::HyprlandMonitor* focusedMonitor READ focusedMonitor NOTIFY focusedMonitorChanged);
	Q_PROPERTY(qs::win32::compat::HyprlandWorkspace* focusedWorkspace READ focusedWorkspace NOTIFY focusedWorkspaceChanged);
	Q_PROPERTY(qs::win32::WindowHandle* activeToplevel READ activeToplevel NOTIFY activeToplevelChanged);
	Q_PROPERTY(UntypedObjectModel* monitors READ monitors CONSTANT);
	Q_PROPERTY(UntypedObjectModel* workspaces READ workspaces CONSTANT);
	Q_PROPERTY(UntypedObjectModel* toplevels READ toplevels CONSTANT);
	// clang-format on
	QML_NAMED_ELEMENT(Hyprland);
	QML_SINGLETON;

public:
	explicit HyprlandIpcQml(QObject* parent = nullptr);

	static HyprlandIpcQml* instance();

	[[nodiscard]] static QString empty() { return {}; }
	[[nodiscard]] HyprlandMonitor* focusedMonitor() const { return this->mFocused; }
	[[nodiscard]] HyprlandWorkspace* focusedWorkspace() const { return this->workspace; }
	[[nodiscard]] static WindowHandle* activeToplevel() { return WindowTracker::instance()->active(); }
	[[nodiscard]] UntypedObjectModel* monitors() { return &this->mMonitors; }
	[[nodiscard]] UntypedObjectModel* workspaces() { return &this->mWorkspaces; }
	[[nodiscard]] static UntypedObjectModel* toplevels() { return WindowTracker::instance()->windows(); }

	Q_INVOKABLE void dispatch(const QString& request);
	Q_INVOKABLE qs::win32::compat::HyprlandMonitor* monitorFor(QuickshellScreenInfo* screen);
	Q_INVOKABLE void refreshMonitors() {}
	Q_INVOKABLE void refreshWorkspaces() {}
	Q_INVOKABLE void refreshToplevels() { WindowTracker::instance()->scheduleRefresh(); }

	[[nodiscard]] HyprlandMonitor* monitorForScreen(QScreen* screen) const;

signals:
	void rawEvent(QObject* event);
	void focusedMonitorChanged();
	void focusedWorkspaceChanged();
	void activeToplevelChanged();

private slots:
	void syncScreens();
	void updateFocusedMonitor();
	void syncWorkspaces();

private:
	// Hyprland's `workspace` argument: "3", "r+1", "e-1", "+1"... -> desktop index, or -1.
	[[nodiscard]] static qsizetype workspaceIndex(QString spec);
	static void focusWorkspace(const QString& spec);

	ObjectModel<HyprlandMonitor> mMonitors {this};
	ObjectModel<HyprlandWorkspace> mWorkspaces {this};
	HyprlandWorkspace* workspace = nullptr;
	HyprlandMonitor* mFocused = nullptr;
	QHash<QScreen*, HyprlandMonitor*> byScreen;
	qint32 nextMonitorId = 0;
	QTimer focusPoll;
};

} // namespace qs::win32::compat
