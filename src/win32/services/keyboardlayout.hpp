#pragma once

// Quickshell.Windows KeyboardLayouts: the input languages installed in Windows and the one the
// app in front is using (Windows keeps a layout per app thread), for ii's layout indicator.
// Codes are locale names ("es-CO", "en-US"); ii shows the language part.

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::win32::keyboardlayout {

class KeyboardLayouts: public QObject {
	Q_OBJECT;
	/// Every installed input language, as locale names.
	Q_PROPERTY(QStringList codes READ codes NOTIFY changed);
	/// The layout of the app in front.
	Q_PROPERTY(QString current READ current NOTIFY changed);
	/// Its display name ("Español (Colombia)").
	Q_PROPERTY(QString currentName READ currentName NOTIFY changed);
	QML_NAMED_ELEMENT(KeyboardLayouts);
	QML_SINGLETON;

public:
	explicit KeyboardLayouts(QObject* parent = nullptr);

	[[nodiscard]] QStringList codes() const { return this->mCodes; }
	[[nodiscard]] QString current() const { return this->mCurrent; }
	[[nodiscard]] QString currentName() const { return this->mCurrentName; }

signals:
	void changed();

private:
	void poll();

	QStringList mCodes;
	QString mCurrent;
	QString mCurrentName;
	QTimer timer;
};

} // namespace qs::win32::keyboardlayout
