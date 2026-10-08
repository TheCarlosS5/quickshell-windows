#pragma once

// Quickshell.Services.SystemTray for Windows: the notification area icons (Shell_NotifyIcon).
//
// A hidden "Shell_TrayWnd" window, owned by a dedicated thread, sits above Explorer's in the
// z-order so Shell_NotifyIcon (which looks the class up) reaches it first. Every message is
// passed on to Explorer's own tray unchanged, so Explorer keeps its icons and the shell can go
// away at any time without apps losing anything. Clicks on the shell's icons are delivered to
// the apps with the same callback messages Explorer sends.

#include <qimage.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

#include "../../core/model.hpp"

namespace qs::win32::tray {

class Status: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Passive = 0,
		Active = 1,
		NeedsAttention = 2,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::tray::Status::Enum status);
};

class Category: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Hardware = 0,
		SystemServices = 1,
		ApplicationStatus = 2,
		Communications = 3,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::tray::Category::Enum category);
};

// One Shell_NotifyIcon call, decoded on the tray thread.
struct IconUpdate {
	enum Kind : quint8 { Upsert, Remove } kind = Upsert;
	QString key;        // GUID, or "<hwnd>:<uID>"
	quint64 hwnd = 0;   // owner window that receives the callback messages
	quint32 uid = 0;
	quint32 pid = 0;
	quint32 version = 0; // NOTIFYICON_VERSION (4 = Vista+ callbacks)
	quint32 callbackMessage = 0;
	bool setCallback = false;
	bool setIcon = false;
	QString iconUrl;
	bool setTip = false;
	QString tip;
	bool setHidden = false;
	bool hidden = false;
	QString exe; // owner executable path
	QString guid; // the icon's GUID (NIF_GUID), if it has one
};

class SystemTrayItem: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QString id READ id CONSTANT);
	Q_PROPERTY(QString title READ title CONSTANT);
	Q_PROPERTY(qs::win32::tray::Status::Enum status READ status CONSTANT);
	Q_PROPERTY(qs::win32::tray::Category::Enum category READ category CONSTANT);
	Q_PROPERTY(QString icon READ icon NOTIFY iconChanged);
	Q_PROPERTY(QString tooltipTitle READ tooltipTitle NOTIFY tooltipChanged);
	Q_PROPERTY(QString tooltipDescription READ tooltipDescription NOTIFY tooltipChanged);
	/// Windows apps draw their own menus: display() asks the app for it.
	Q_PROPERTY(bool hasMenu READ falseValue CONSTANT);
	Q_PROPERTY(QObject* menu READ nullMenu CONSTANT);
	Q_PROPERTY(bool onlyMenu READ falseValue CONSTANT);
	/// Windows-only: the user (or Windows) shows this icon on the taskbar rather than in the
	/// hidden icons behind the arrow (Settings > Personalization > Taskbar > Other system tray
	/// icons). Shells can put the others in their overflow menu.
	Q_PROPERTY(bool promoted READ promoted NOTIFY promotedChanged);
	QML_ELEMENT;
	QML_UNCREATABLE("SystemTrayItems can only be acquired from SystemTray");

public:
	SystemTrayItem(QString key, QString id, QString title, QObject* parent);

	[[nodiscard]] static bool falseValue() { return false; }
	[[nodiscard]] static QObject* nullMenu() { return nullptr; }
	[[nodiscard]] QString key() const { return this->mKey; }
	[[nodiscard]] QString id() const { return this->mId; }
	[[nodiscard]] QString title() const { return this->mTitle; }
	[[nodiscard]] static Status::Enum status() { return Status::Active; }
	[[nodiscard]] static Category::Enum category() { return Category::ApplicationStatus; }
	[[nodiscard]] QString icon() const { return this->mIcon; }
	[[nodiscard]] QString tooltipTitle() const { return this->mTip.section('\n', 0, 0); }
	[[nodiscard]] QString tooltipDescription() const { return this->mTip.section('\n', 1); }
	[[nodiscard]] bool isHidden() const { return this->mHidden; }
	[[nodiscard]] bool promoted() const { return this->mPromoted; }
	[[nodiscard]] QString exe() const { return this->mExe; }
	[[nodiscard]] QString guid() const { return this->mGuid; }
	[[nodiscard]] quint32 iconUid() const { return this->uid; }
	void setPromoted(bool promoted);

	/// Left click.
	Q_INVOKABLE void activate();
	/// Middle click: a double click for apps that only open on one.
	Q_INVOKABLE void secondaryActivate();
	Q_INVOKABLE void scroll(qint32 /*delta*/, bool /*horizontal*/) const {}
	/// Right click: the app shows its own context menu at the cursor.
	Q_INVOKABLE void display(QObject* parentWindow, qint32 relativeX, qint32 relativeY);

	void apply(const IconUpdate& update);

signals:
	void iconChanged();
	void tooltipChanged();
	void promotedChanged();

private:
	void send(quint32 mouseMessage) const;

	QString mKey;
	QString mId;
	QString mTitle;
	QString mIcon;
	QString mTip;
	bool mHidden = false;
	bool mPromoted = false;
	QString mExe;
	QString mGuid;
	quint64 hwnd = 0;
	quint32 uid = 0;
	quint32 pid = 0;
	quint32 version = 0;
	quint32 callbackMessage = 0;
};

class SystemTray: public QObject {
	Q_OBJECT;
	Q_PROPERTY(UntypedObjectModel* items READ items CONSTANT);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit SystemTray(QObject* parent = nullptr);
	[[nodiscard]] static UntypedObjectModel* items();
};

} // namespace qs::win32::tray
