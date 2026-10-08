#pragma once

// Quickshell.Windows UiSound: short interface sounds (panel open/close, notifications...) played
// in-process with PlaySound, so they come without the delay of starting a player process.
// Files are 16-bit PCM WAVs; each is read once per volume and kept in memory (PlaySound plays
// from that memory asynchronously). A new sound replaces the one playing, as UI sounds should.

#include <qbytearray.h>
#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>

namespace qs::win32::uisound {

class UiSound: public QObject {
	Q_OBJECT;
	QML_NAMED_ELEMENT(UiSound);
	QML_SINGLETON;

public:
	explicit UiSound(QObject* parent = nullptr): QObject(parent) {}
	~UiSound() override;
	Q_DISABLE_COPY_MOVE(UiSound);

	/// Plays a WAV file (path or file:// URL) at volume 0..1.
	Q_INVOKABLE void play(const QString& path, qreal volume = 1);
	/// Stops whatever UiSound is playing.
	Q_INVOKABLE void stop();

private:
	QHash<QString, QByteArray> cache; // "<volume>|<path>" -> scaled WAV
};

} // namespace qs::win32::uisound
