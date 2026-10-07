#include "mpris.hpp"
#include <mutex>
#include <thread>

#include <qcoreapplication.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qprocess.h>
#include <qurl.h>

// clang-format off
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
// clang-format on

#include "../../core/logcat.hpp"

namespace qs::win32::mpris {

QS_LOGGING_CATEGORY(logMpris, "quickshell.win32.mpris", QtInfoMsg);

using namespace winrt::Windows::Media::Control;
using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;

QString MprisPlaybackState::toString(MprisPlaybackState::Enum status) {
	switch (status) {
	case Playing: return "Playing";
	case Paused: return "Paused";
	default: return "Stopped";
	}
}

QString MprisLoopState::toString(MprisLoopState::Enum status) {
	switch (status) {
	case Track: return "Track";
	case Playlist: return "Playlist";
	default: return "None";
	}
}

namespace {

QString toQ(const winrt::hstring& s) { return QString::fromWCharArray(s.c_str(), static_cast<qsizetype>(s.size())); }

// Owns the GSMTC manager and sessions. WinRT calls that block (.get()) run on the worker
// thread or on WinRT's thread pool (event callbacks), never on the GUI (STA) thread.
class Backend: public QObject {
public:
	static Backend* instance() {
		static auto* backend = new Backend(); // NOLINT
		return backend;
	}

	ObjectModel<MprisPlayer> players {this};

	GlobalSystemMediaTransportControlsSession session(const QString& id) {
		std::scoped_lock lock(this->mutex);
		auto it = this->sessions.find(id);
		return it == this->sessions.end() ? nullptr : it.value();
	}

private:
	Backend() {
		std::thread([this]() {
			try {
				winrt::init_apartment(winrt::apartment_type::multi_threaded);
				this->manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				this->manager.SessionsChanged([this](auto&&, auto&&) { this->refreshSessions(); });
				this->refreshSessions();
				qCInfo(logMpris) << "Media sessions ready";
			} catch (const winrt::hresult_error& e) {
				qCWarning(logMpris) << "GSMTC unavailable:" << toQ(e.message());
			}
		}).detach();
	}

	void refreshSessions() {
		QHash<QString, GlobalSystemMediaTransportControlsSession> current;
		try {
			auto list = this->manager.GetSessions();
			QHash<QString, int> seen;
			for (const auto& session: list) {
				auto app = toQ(session.SourceAppUserModelId());
				auto n = seen[app]++;
				current.insert(n == 0 ? app : app + '#' + QString::number(n), session);
			}
		} catch (...) {
			return;
		}

		QStringList added;
		QStringList removed;
		{
			std::scoped_lock lock(this->mutex);
			for (auto it = current.begin(); it != current.end(); ++it) {
				if (this->sessions.contains(it.key())) continue;
				added.append(it.key());
				this->subscribe(it.key(), it.value());
			}
			for (auto it = this->sessions.begin(); it != this->sessions.end(); ++it) {
				if (!current.contains(it.key())) removed.append(it.key());
			}
			this->sessions = current;
		}

		QMetaObject::invokeMethod(this, [this, added, removed]() {
			for (const auto& id: removed) {
				for (auto* player: QList(this->players.valueList())) {
					if (player->sessionId() == id) {
						this->players.removeObject(player);
						player->deleteLater();
					}
				}
			}
			for (const auto& id: added) {
				auto app = id.section('#', 0, 0);
				this->players.insertObject(new MprisPlayer(id, app, this));
			}
		});

		for (const auto& id: added) this->pushState(id);
	}

	void subscribe(const QString& id, const GlobalSystemMediaTransportControlsSession& session) {
		session.MediaPropertiesChanged([this, id](auto&&, auto&&) { this->pushState(id); });
		session.PlaybackInfoChanged([this, id](auto&&, auto&&) { this->pushState(id); });
		session.TimelinePropertiesChanged([this, id](auto&&, auto&&) { this->pushState(id); });
	}

	static QString saveArt(const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& ref) {
		if (!ref) return {};
		try {
			auto stream = ref.OpenReadAsync().get();
			auto size = static_cast<uint32_t>(stream.Size());
			if (size == 0 || size > 32 * 1024 * 1024) return {};
			winrt::Windows::Storage::Streams::DataReader reader(stream);
			reader.LoadAsync(size).get();
			QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
			reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(bytes.data()), size)); // NOLINT

			auto dir = QDir(QDir::tempPath()).filePath("ii-windows/mpris");
			QDir().mkpath(dir);
			auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
			auto path = QDir(dir).filePath(QString::fromLatin1(hash) + ".img");
			if (!QFile::exists(path)) {
				QFile file(path);
				if (file.open(QFile::WriteOnly)) file.write(bytes);
			}
			return QUrl::fromLocalFile(path).toString();
		} catch (...) {
			return {};
		}
	}

	// Runs on WinRT's thread pool.
	void pushState(const QString& id) {
		auto session = this->session(id);
		if (!session) return;

		PlayerState state;
		try {
			auto props = session.TryGetMediaPropertiesAsync().get();
			if (props) {
				state.title = toQ(props.Title());
				state.artist = toQ(props.Artist());
				state.album = toQ(props.AlbumTitle());
				state.albumArtist = toQ(props.AlbumArtist());
				state.artUrl = saveArt(props.Thumbnail());
			}
			qCDebug(logMpris) << "Session" << id << "title" << state.title << "artist" << state.artist;
		} catch (const winrt::hresult_error& e) {
			qCWarning(logMpris) << "Media properties unavailable for" << id << toQ(e.message());
		} catch (...) {}

		try {
			auto info = session.GetPlaybackInfo();
			switch (info.PlaybackStatus()) {
			case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
				state.playback = MprisPlaybackState::Playing;
				break;
			case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
				state.playback = MprisPlaybackState::Paused;
				break;
			default: state.playback = MprisPlaybackState::Stopped; break;
			}
			auto controls = info.Controls();
			state.canPlay = controls.IsPlayEnabled();
			state.canPause = controls.IsPauseEnabled() || controls.IsPlayPauseToggleEnabled();
			state.canNext = controls.IsNextEnabled();
			state.canPrevious = controls.IsPreviousEnabled();
			state.canSeek = controls.IsPlaybackPositionEnabled();
			state.shuffleSupported = controls.IsShuffleEnabled();
			state.loopSupported = controls.IsRepeatEnabled();
			if (auto shuffle = info.IsShuffleActive()) state.shuffle = shuffle.Value();
			if (auto repeat = info.AutoRepeatMode()) {
				auto mode = repeat.Value();
				state.loop = mode == MediaPlaybackAutoRepeatMode::Track  ? MprisLoopState::Track
				           : mode == MediaPlaybackAutoRepeatMode::List ? MprisLoopState::Playlist
				                                                       : MprisLoopState::None;
			}
			if (auto rate = info.PlaybackRate()) state.rate = rate.Value();
		} catch (...) {}

		try {
			auto timeline = session.GetTimelineProperties();
			auto seconds = [](winrt::Windows::Foundation::TimeSpan span) {
				return std::chrono::duration<double>(span).count();
			};
			state.length = seconds(timeline.EndTime() - timeline.StartTime());
			auto position = seconds(timeline.Position() - timeline.StartTime());
			// Apps report the position at LastUpdatedTime; bring it to now while playing.
			auto age = std::chrono::duration<double>(winrt::clock::now() - timeline.LastUpdatedTime()).count();
			if (state.playback == MprisPlaybackState::Playing && age > 0 && age < 24 * 3600) {
				position += age * state.rate;
			}
			if (state.length > 0) position = std::min(position, state.length);
			state.position = std::max(0.0, position);
			state.positionUpdated = QDateTime::currentDateTimeUtc();
		} catch (...) {}

		QMetaObject::invokeMethod(this, [this, id, state]() {
			for (auto* player: this->players.valueList()) {
				if (player->sessionId() == id) player->apply(state);
			}
		});
	}

	std::mutex mutex;
	GlobalSystemMediaTransportControlsSessionManager manager {nullptr};
	QHash<QString, GlobalSystemMediaTransportControlsSession> sessions;
};

// "Spotify.exe" -> "Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" -> "ZuneMusic"
QString identityFor(const QString& aumid) {
	try {
		auto info = winrt::Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(winrt::hstring(aumid.toStdWString()));
		if (info) {
			auto name = toQ(info.DisplayInfo().DisplayName());
			if (!name.isEmpty()) return name;
		}
	} catch (...) {}

	auto name = aumid.section('!', -1).section('\\', -1);
	if (name.endsWith(".exe", Qt::CaseInsensitive)) name.chop(4);
	name = name.section('.', -1);
	if (!name.isEmpty()) name[0] = name[0].toUpper();
	return name.isEmpty() ? aumid : name;
}

// Fire-and-forget: never block the GUI thread on WinRT async operations.
template <typename F>
void withSession(const QString& id, F&& action) {
	auto session = Backend::instance()->session(id);
	if (!session) return;
	try {
		action(session);
	} catch (...) {}
}

} // namespace

// MprisPlayer

static quint32 NEXT_UNIQUE_ID = 1; // NOLINT

MprisPlayer::MprisPlayer(QString sessionId, QString appId, QObject* parent)
    : QObject(parent)
    , mSessionId(std::move(sessionId))
    , mIdentity(identityFor(appId)) {
	this->mDesktopEntry = appId.section('!', 0, 0).section('\\', -1);
	if (this->mDesktopEntry.endsWith(".exe", Qt::CaseInsensitive)) this->mDesktopEntry.chop(4);
	this->mDesktopEntry = this->mDesktopEntry.toLower();
}

qreal MprisPlayer::position() const {
	if (this->s.playback != MprisPlaybackState::Playing || !this->s.positionUpdated.isValid()) {
		return this->s.position;
	}
	auto elapsed = static_cast<double>(this->s.positionUpdated.msecsTo(QDateTime::currentDateTimeUtc())) / 1000.0;
	auto position = this->s.position + elapsed * this->s.rate;
	return this->s.length > 0 ? std::min(position, this->s.length) : position;
}

QVariantMap MprisPlayer::metadata() const {
	return {
	    {"xesam:title", this->s.title},
	    {"xesam:artist", QStringList {this->s.artist}},
	    {"xesam:album", this->s.album},
	    {"xesam:albumArtist", QStringList {this->s.albumArtist}},
	    {"mpris:artUrl", this->s.artUrl},
	    {"mpris:length", static_cast<qlonglong>(this->s.length * 1e6)},
	};
}

void MprisPlayer::apply(const PlayerState& state) {
	auto old = this->s;
	this->s = state;

	auto trackChanged = old.title != state.title || old.artist != state.artist || old.album != state.album;
	if (trackChanged) {
		this->mUniqueId = NEXT_UNIQUE_ID++;
		emit this->trackChanged();
		emit this->uniqueIdChanged();
	}
	if (old.title != state.title) emit this->trackTitleChanged();
	if (old.artist != state.artist) emit this->trackArtistChanged();
	if (old.album != state.album) emit this->trackAlbumChanged();
	if (old.albumArtist != state.albumArtist) emit this->trackAlbumArtistChanged();
	if (old.artUrl != state.artUrl) emit this->trackArtUrlChanged();
	if (old.length != state.length) emit this->lengthChanged();
	if (old.playback != state.playback) {
		emit this->playbackStateChanged();
		emit this->isPlayingChanged();
	}
	if (old.loop != state.loop) emit this->loopStateChanged();
	if (old.shuffle != state.shuffle) emit this->shuffleChanged();
	if (old.rate != state.rate) emit this->rateChanged();
	emit this->positionChanged();
	emit this->metadataChanged();
	emit this->stateChanged();
	if (trackChanged) emit this->postTrackChanged();
}

void MprisPlayer::setPosition(qreal position) {
	withSession(this->mSessionId, [&](auto& session) {
		session.TryChangePlaybackPositionAsync(static_cast<int64_t>(position * 1e7));
	});
	this->s.position = position;
	this->s.positionUpdated = QDateTime::currentDateTimeUtc();
	emit this->positionChanged();
}

void MprisPlayer::seek(qreal offset) { this->setPosition(this->position() + offset); }

void MprisPlayer::setPlaybackState(MprisPlaybackState::Enum state) {
	switch (state) {
	case MprisPlaybackState::Playing: this->play(); break;
	case MprisPlaybackState::Paused: this->pause(); break;
	default: this->stop(); break;
	}
}

void MprisPlayer::setPlaying(bool playing) {
	if (playing) this->play();
	else this->pause();
}

void MprisPlayer::setLoopState(MprisLoopState::Enum loopState) {
	auto mode = loopState == MprisLoopState::Track  ? MediaPlaybackAutoRepeatMode::Track
	          : loopState == MprisLoopState::Playlist ? MediaPlaybackAutoRepeatMode::List
	                                                  : MediaPlaybackAutoRepeatMode::None;
	withSession(this->mSessionId, [&](auto& session) { session.TryChangeAutoRepeatModeAsync(mode); });
}

void MprisPlayer::setRate(qreal rate) {
	withSession(this->mSessionId, [&](auto& session) { session.TryChangePlaybackRateAsync(rate); });
}

void MprisPlayer::setShuffle(bool shuffle) {
	withSession(this->mSessionId, [&](auto& session) { session.TryChangeShuffleActiveAsync(shuffle); });
}

void MprisPlayer::raise() {
	// Bring the player's window forward through the shell's app activation.
	auto aumid = this->mSessionId.section('#', 0, 0);
	QProcess::startDetached("explorer.exe", {"shell:AppsFolder\\" + aumid});
}

void MprisPlayer::next() {
	withSession(this->mSessionId, [](auto& session) { session.TrySkipNextAsync(); });
}
void MprisPlayer::previous() {
	withSession(this->mSessionId, [](auto& session) { session.TrySkipPreviousAsync(); });
}
void MprisPlayer::play() {
	withSession(this->mSessionId, [](auto& session) { session.TryPlayAsync(); });
}
void MprisPlayer::pause() {
	withSession(this->mSessionId, [](auto& session) { session.TryPauseAsync(); });
}
void MprisPlayer::stop() {
	withSession(this->mSessionId, [](auto& session) { session.TryStopAsync(); });
}
void MprisPlayer::togglePlaying() {
	withSession(this->mSessionId, [](auto& session) { session.TryTogglePlayPauseAsync(); });
}

// Mpris

MprisQml::MprisQml(QObject* parent): QObject(parent) { Backend::instance(); }

UntypedObjectModel* MprisQml::players() { return &Backend::instance()->players; }

} // namespace qs::win32::mpris
