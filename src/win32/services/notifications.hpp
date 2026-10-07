#pragma once

// Quickshell.Services.Notifications for Windows. Two sources feed the same server:
//  - Windows toasts, read with UserNotificationListener (polled: its change event needs package
//    identity, which an unpackaged shell doesn't have). Dismissing one in the shell also removes
//    it from Windows' notification center.
//  - notify-send (ii-shim), delivered to a message-only window as WM_COPYDATA.
// The QML API matches Quickshell's notification server.

#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

#include "../../core/model.hpp"

namespace qs::win32::notifications {

class NotificationUrgency: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Low = 0,
		Normal = 1,
		Critical = 2,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::notifications::NotificationUrgency::Enum value);
};

class NotificationCloseReason: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Expired = 1,
		Dismissed = 2,
		CloseRequested = 3,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::notifications::NotificationCloseReason::Enum value);
};

class Notification;

class NotificationAction: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QString identifier READ identifier CONSTANT);
	Q_PROPERTY(QString text READ text CONSTANT);
	QML_ELEMENT;
	QML_UNCREATABLE("NotificationActions must be acquired from a Notification");

public:
	NotificationAction(QString identifier, QString text, Notification* notification);

	[[nodiscard]] QString identifier() const { return this->mIdentifier; }
	[[nodiscard]] QString text() const { return this->mText; }

	Q_INVOKABLE void invoke();

private:
	QString mIdentifier;
	QString mText;
	Notification* notification;
};

// What a source knows about a notification; turned into a Notification on the GUI thread.
struct NotificationData {
	quint32 toastId = 0; // 0 when it didn't come from a Windows toast
	QString appId;       // AUMID for toasts
	QString appName, appIcon, summary, body, image;
	NotificationUrgency::Enum urgency = NotificationUrgency::Normal;
	qreal expireTimeout = -1;
	bool transient = false;
};

class Notification: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(quint32 id READ id CONSTANT);
	Q_PROPERTY(bool tracked READ isTracked WRITE setTracked NOTIFY trackedChanged);
	Q_PROPERTY(bool lastGeneration READ falseValue CONSTANT);
	Q_PROPERTY(qreal expireTimeout READ expireTimeout CONSTANT);
	Q_PROPERTY(QString appName READ appName CONSTANT);
	Q_PROPERTY(QString appIcon READ appIcon CONSTANT);
	Q_PROPERTY(QString summary READ summary CONSTANT);
	Q_PROPERTY(QString body READ body CONSTANT);
	Q_PROPERTY(qs::win32::notifications::NotificationUrgency::Enum urgency READ urgency CONSTANT);
	Q_PROPERTY(QList<qs::win32::notifications::NotificationAction*> actions READ actions CONSTANT);
	Q_PROPERTY(bool hasActionIcons READ falseValue CONSTANT);
	Q_PROPERTY(bool resident READ falseValue CONSTANT);
	Q_PROPERTY(bool transient READ transient CONSTANT);
	Q_PROPERTY(QString desktopEntry READ desktopEntry CONSTANT);
	Q_PROPERTY(QString image READ image CONSTANT);
	Q_PROPERTY(bool hasInlineReply READ falseValue CONSTANT);
	Q_PROPERTY(QString inlineReplyPlaceholder READ emptyString CONSTANT);
	Q_PROPERTY(QVariantMap hints READ hints CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("Notifications must be acquired from a NotificationServer");

public:
	Notification(quint32 id, NotificationData data, QString desktopEntry, QObject* parent);

	[[nodiscard]] static bool falseValue() { return false; }
	[[nodiscard]] static QString emptyString() { return {}; }
	[[nodiscard]] quint32 id() const { return this->mId; }
	[[nodiscard]] bool isTracked() const { return this->mTracked; }
	void setTracked(bool tracked);
	[[nodiscard]] qreal expireTimeout() const { return this->d.expireTimeout; }
	[[nodiscard]] QString appName() const { return this->d.appName; }
	[[nodiscard]] QString appIcon() const { return this->d.appIcon; }
	[[nodiscard]] QString summary() const { return this->d.summary; }
	[[nodiscard]] QString body() const { return this->d.body; }
	[[nodiscard]] NotificationUrgency::Enum urgency() const { return this->d.urgency; }
	[[nodiscard]] QList<NotificationAction*> actions() const { return this->mActions; }
	[[nodiscard]] bool transient() const { return this->d.transient; }
	[[nodiscard]] QString desktopEntry() const { return this->mDesktopEntry; }
	[[nodiscard]] QString image() const { return this->d.image; }
	[[nodiscard]] QVariantMap hints() const;
	[[nodiscard]] const NotificationData& data() const { return this->d; }

	Q_INVOKABLE void expire();
	Q_INVOKABLE void dismiss();
	Q_INVOKABLE void sendInlineReply(const QString& /*replyText*/) {}

	void close(NotificationCloseReason::Enum reason);

signals:
	void closed(qs::win32::notifications::NotificationCloseReason::Enum reason);
	void trackedChanged();

private:
	quint32 mId;
	NotificationData d;
	QString mDesktopEntry;
	QList<NotificationAction*> mActions;
	bool mTracked = false;
	bool mClosed = false;
};

class NotificationServerQml: public QObject {
	Q_OBJECT;
	// The capability flags are accepted for compatibility; what Windows can deliver is fixed.
	Q_PROPERTY(bool keepOnReload MEMBER mKeepOnReload NOTIFY flagsChanged);
	Q_PROPERTY(bool persistenceSupported MEMBER m_persistenceSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool bodySupported MEMBER m_bodySupported NOTIFY flagsChanged);
	Q_PROPERTY(bool bodyMarkupSupported MEMBER m_bodyMarkupSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool bodyHyperlinksSupported MEMBER m_bodyHyperlinksSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool bodyImagesSupported MEMBER m_bodyImagesSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool actionsSupported MEMBER m_actionsSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool actionIconsSupported MEMBER m_actionIconsSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool imageSupported MEMBER m_imageSupported NOTIFY flagsChanged);
	Q_PROPERTY(bool inlineReplySupported MEMBER m_inlineReplySupported NOTIFY flagsChanged);
	Q_PROPERTY(QList<QString> extraHints MEMBER mExtraHints NOTIFY flagsChanged);
	Q_PROPERTY(UntypedObjectModel* trackedNotifications READ trackedNotifications CONSTANT);
	QML_NAMED_ELEMENT(NotificationServer);

public:
	explicit NotificationServerQml(QObject* parent = nullptr);

	[[nodiscard]] static UntypedObjectModel* trackedNotifications();

signals:
	void notification(qs::win32::notifications::Notification* notification);
	void flagsChanged();

private:
	bool mKeepOnReload = true;
	bool m_persistenceSupported = false;
	bool m_bodySupported = false;
	bool m_bodyMarkupSupported = false;
	bool m_bodyHyperlinksSupported = false;
	bool m_bodyImagesSupported = false;
	bool m_actionsSupported = false;
	bool m_actionIconsSupported = false;
	bool m_imageSupported = false;
	bool m_inlineReplySupported = false;
	QList<QString> mExtraHints;
};

} // namespace qs::win32::notifications
