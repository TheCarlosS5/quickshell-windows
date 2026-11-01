#pragma once

// Quickshell.Windows PrivacyMonitor (ii-windows plan 2026-10-31, F1/E10): which apps are using the microphone, the
// camera or the location right now. Windows shows this in its own taskbar, which ii hides; ii shows it in its bar.
//
// Source: HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\<capability>, where every
// app (packaged ones by package family name, desktop ones under NonPackaged by path with '#' for '\') keeps
// LastUsedTimeStart / LastUsedTimeStop: in use = started and not stopped. A worker thread waits on
// RegNotifyChangeKeyValue (no polling) and the lists are updated on the GUI thread.

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstringlist.h>
#include <qtmetamacros.h>

namespace qs::win32::privacy {

class PrivacyMonitor: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(PrivacyMonitor);
	QML_SINGLETON;
	/// Friendly names of the apps using each one now.
	Q_PROPERTY(QStringList microphone READ microphone NOTIFY changed);
	Q_PROPERTY(QStringList camera READ camera NOTIFY changed);
	Q_PROPERTY(QStringList location READ location NOTIFY changed);

public:
	explicit PrivacyMonitor(QObject* parent = nullptr);

	[[nodiscard]] QStringList microphone() const { return this->mMicrophone; }
	[[nodiscard]] QStringList camera() const { return this->mCamera; }
	[[nodiscard]] QStringList location() const { return this->mLocation; }

	void apply(const QStringList& microphone, const QStringList& camera, const QStringList& location);

signals:
	void changed();

private:
	QStringList mMicrophone;
	QStringList mCamera;
	QStringList mLocation;
};

} // namespace qs::win32::privacy
