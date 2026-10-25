#pragma once

// Quickshell.Windows SystemProbe: small read-only questions the shell asks every few seconds, answered in-process
// instead of by starting a helper process each time (plan 1.2: on Windows a process costs far more than on Linux,
// and the antivirus looks at every one).

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtmetamacros.h>

namespace qs::win32::systemprobe {

class SystemProbe: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(SystemProbe);
	QML_SINGLETON;

public:
	explicit SystemProbe(QObject* parent = nullptr): QObject(parent) {}

	/// Is a process with one of these executable names running in this session (case-insensitive, e.g. "wallpaper64.exe")?
	Q_INVOKABLE static bool processRunning(const QStringList& names);
	/// The file's last write time in ms since the epoch, or -1 if there is no such file.
	Q_INVOKABLE static qint64 modifiedMs(const QString& path);
};

} // namespace qs::win32::systemprobe
