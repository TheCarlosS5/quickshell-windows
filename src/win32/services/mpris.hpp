#pragma once

// Quickshell.Services.Mpris for Windows, backed by the Global System Media Transport Controls
// (the sessions behind Windows' own media flyout: Spotify, browsers, media players...).
// The QML API matches Quickshell's MPRIS module.

#include <memory>

#include <qdatetime.h>
#include <qhash.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtmetamacros.h>
#include <qvariant.h>

#include "../../core/model.hpp"

namespace qs::win32::mpris {

class MprisPlaybackState: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Stopped = 0,
		Playing = 1,
		Paused = 2,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::mpris::MprisPlaybackState::Enum status);
};

class MprisLoopState: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		None = 0,
		Track = 1,
		Playlist = 2,
	};
	Q_ENUM(Enum);
	Q_INVOKABLE static QString toString(qs::win32::mpris::MprisLoopState::Enum status);
};

// Snapshot of one GSMTC session, built on a worker thread and applied on the GUI thread.
struct PlayerState {
	QString title, artist, album, albumArtist, artUrl;
	MprisPlaybackState::Enum playback = MprisPlaybackState::Stopped;
	MprisLoopState::Enum loop = MprisLoopState::None;
	bool shuffle = false, loopSupported = false, shuffleSupported = false;
	bool canPlay = false, canPause = false, canNext = false, canPrevious = false, canSeek = false;
	qreal position = 0, length = 0, rate = 1;
	QDateTime positionUpdated;
};

class MprisPlayer: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool canControl READ canControl NOTIFY stateChanged);
	Q_PROPERTY(bool canPlay READ canPlay NOTIFY stateChanged);
	Q_PROPERTY(bool canPause READ canPause NOTIFY stateChanged);
	Q_PROPERTY(bool canTogglePlaying READ canTogglePlaying NOTIFY stateChanged);
	Q_PROPERTY(bool canSeek READ canSeek NOTIFY stateChanged);
	Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY stateChanged);
	Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY stateChanged);
	Q_PROPERTY(bool canQuit READ falseValue CONSTANT);
	Q_PROPERTY(bool canRaise READ trueValue CONSTANT);
	Q_PROPERTY(bool canSetFullscreen READ falseValue CONSTANT);
	Q_PROPERTY(QString identity READ identity CONSTANT);
	Q_PROPERTY(QString desktopEntry READ desktopEntry CONSTANT);
	Q_PROPERTY(QString dbusName READ dbusName CONSTANT);
	Q_PROPERTY(qreal position READ position WRITE setPosition NOTIFY positionChanged);
	Q_PROPERTY(bool positionSupported READ trueValue CONSTANT);
	Q_PROPERTY(qreal length READ length NOTIFY lengthChanged);
	Q_PROPERTY(bool lengthSupported READ lengthSupported NOTIFY lengthChanged);
	Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged);
	Q_PROPERTY(bool volumeSupported READ falseValue CONSTANT);
	Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY metadataChanged);
	Q_PROPERTY(quint32 uniqueId READ uniqueId NOTIFY uniqueIdChanged);
	Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY trackTitleChanged);
	Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY trackArtistChanged);
	Q_PROPERTY(QString trackArtists READ trackArtist NOTIFY trackArtistChanged);
	Q_PROPERTY(QString trackAlbum READ trackAlbum NOTIFY trackAlbumChanged);
	Q_PROPERTY(QString trackAlbumArtist READ trackAlbumArtist NOTIFY trackAlbumArtistChanged);
	Q_PROPERTY(QString trackArtUrl READ trackArtUrl NOTIFY trackArtUrlChanged);
	Q_PROPERTY(qs::win32::mpris::MprisPlaybackState::Enum playbackState READ playbackState WRITE setPlaybackState NOTIFY playbackStateChanged);
	Q_PROPERTY(bool isPlaying READ isPlaying WRITE setPlaying NOTIFY isPlayingChanged);
	Q_PROPERTY(qs::win32::mpris::MprisLoopState::Enum loopState READ loopState WRITE setLoopState NOTIFY loopStateChanged);
	Q_PROPERTY(bool loopSupported READ loopSupported NOTIFY stateChanged);
	Q_PROPERTY(qreal rate READ rate WRITE setRate NOTIFY rateChanged);
	Q_PROPERTY(qreal minRate READ minRate CONSTANT);
	Q_PROPERTY(qreal maxRate READ maxRate CONSTANT);
	Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged);
	Q_PROPERTY(bool shuffleSupported READ shuffleSupported NOTIFY stateChanged);
	Q_PROPERTY(bool fullscreen READ falseValue WRITE setFullscreen NOTIFY fullscreenChanged);
	Q_PROPERTY(QList<QString> supportedUriSchemes READ emptyList CONSTANT);
	Q_PROPERTY(QList<QString> supportedMimeTypes READ emptyList CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("MprisPlayers can only be acquired from Mpris");

public:
	MprisPlayer(QString sessionId, QString appId, QObject* parent);

	[[nodiscard]] QString sessionId() const { return this->mSessionId; }

	[[nodiscard]] static bool trueValue() { return true; }
	[[nodiscard]] static bool falseValue() { return false; }
	[[nodiscard]] static QList<QString> emptyList() { return {}; }
	[[nodiscard]] bool canControl() const { return true; }
	[[nodiscard]] bool canPlay() const { return this->s.canPlay; }
	[[nodiscard]] bool canPause() const { return this->s.canPause; }
	[[nodiscard]] bool canTogglePlaying() const { return this->s.canPlay || this->s.canPause; }
	[[nodiscard]] bool canSeek() const { return this->s.canSeek; }
	[[nodiscard]] bool canGoNext() const { return this->s.canNext; }
	[[nodiscard]] bool canGoPrevious() const { return this->s.canPrevious; }
	[[nodiscard]] QString identity() const { return this->mIdentity; }
	[[nodiscard]] QString desktopEntry() const { return this->mDesktopEntry; }
	[[nodiscard]] QString dbusName() const { return "org.mpris.MediaPlayer2." + this->mDesktopEntry; }
	[[nodiscard]] qreal position() const;
	void setPosition(qreal position);
	[[nodiscard]] qreal length() const { return this->s.length; }
	[[nodiscard]] bool lengthSupported() const { return this->s.length > 0; }
	[[nodiscard]] static qreal volume() { return 1; }
	void setVolume(qreal /*volume*/) {}
	[[nodiscard]] QVariantMap metadata() const;
	[[nodiscard]] quint32 uniqueId() const { return this->mUniqueId; }
	[[nodiscard]] QString trackTitle() const { return this->s.title; }
	[[nodiscard]] QString trackArtist() const { return this->s.artist; }
	[[nodiscard]] QString trackAlbum() const { return this->s.album; }
	[[nodiscard]] QString trackAlbumArtist() const { return this->s.albumArtist; }
	[[nodiscard]] QString trackArtUrl() const { return this->s.artUrl; }
	[[nodiscard]] MprisPlaybackState::Enum playbackState() const { return this->s.playback; }
	void setPlaybackState(MprisPlaybackState::Enum state);
	[[nodiscard]] bool isPlaying() const { return this->s.playback == MprisPlaybackState::Playing; }
	void setPlaying(bool playing);
	[[nodiscard]] MprisLoopState::Enum loopState() const { return this->s.loop; }
	void setLoopState(MprisLoopState::Enum loopState);
	[[nodiscard]] bool loopSupported() const { return this->s.loopSupported; }
	[[nodiscard]] qreal rate() const { return this->s.rate; }
	void setRate(qreal rate);
	[[nodiscard]] static qreal minRate() { return 0.25; }
	[[nodiscard]] static qreal maxRate() { return 4; }
	[[nodiscard]] bool shuffle() const { return this->s.shuffle; }
	void setShuffle(bool shuffle);
	[[nodiscard]] bool shuffleSupported() const { return this->s.shuffleSupported; }
	void setFullscreen(bool /*fullscreen*/) {}

	Q_INVOKABLE void raise();
	Q_INVOKABLE void quit() {}
	Q_INVOKABLE void openUri(const QString& /*uri*/) {}
	Q_INVOKABLE void next();
	Q_INVOKABLE void previous();
	Q_INVOKABLE void seek(qreal offset);
	Q_INVOKABLE void play();
	Q_INVOKABLE void pause();
	Q_INVOKABLE void stop();
	Q_INVOKABLE void togglePlaying();

	void apply(const PlayerState& state);

signals:
	void stateChanged();
	void positionChanged();
	void lengthChanged();
	void volumeChanged();
	void metadataChanged();
	void uniqueIdChanged();
	void trackTitleChanged();
	void trackArtistChanged();
	void trackAlbumChanged();
	void trackAlbumArtistChanged();
	void trackArtUrlChanged();
	void playbackStateChanged();
	void isPlayingChanged();
	void loopStateChanged();
	void rateChanged();
	void shuffleChanged();
	void fullscreenChanged();
	void trackChanged();
	void postTrackChanged();

private:
	QString mSessionId;
	QString mIdentity;
	QString mDesktopEntry;
	quint32 mUniqueId = 0;
	PlayerState s;
};

class MprisQml: public QObject {
	Q_OBJECT;
	Q_PROPERTY(UntypedObjectModel* players READ players CONSTANT);
	QML_NAMED_ELEMENT(Mpris);
	QML_SINGLETON;

public:
	explicit MprisQml(QObject* parent = nullptr);
	[[nodiscard]] static UntypedObjectModel* players();
};

} // namespace qs::win32::mpris
