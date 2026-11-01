#pragma once

// Quickshell.Windows SingleInstance (ii-windows plan 2026-10-31, C5): one window of a kind (Configuración) instead of
// one more process each time it is asked for.
//
//   if (!SingleInstance.claim("ii-settings", page)) Qt.quit();   // another one is open: it got `page` and came forward
//   Connections { target: SingleInstance; function onActivated(argument) { ... } }
//
// The first process owns the mutex Local\qs-single-<name> and a hidden window that receives the others' argument.

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::win32::singleinstance {

class SingleInstance: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(SingleInstance);
	QML_SINGLETON;

public:
	explicit SingleInstance(QObject* parent = nullptr): QObject(parent) {}

	/// True if this process is the first of `name` (it keeps it from now on). False if another one already is: the
	/// argument was handed to it, and it was allowed to come to the front.
	Q_INVOKABLE bool claim(const QString& name, const QString& argument = QString());

	void receive(const QString& argument) { emit this->activated(argument); }

signals:
	/// Another process of the same name was started with this argument (it has quit by now).
	void activated(QString argument);
};

} // namespace qs::win32::singleinstance
