#include "mpris.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <qcoreapplication.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
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
#include "../crash.hpp"

// Media sessions (GSMTC) for Quickshell's MPRIS API.
//
// Plan 2026-10-31, A1. The previous version crashed the whole shell (0xC0000005, every 10-40 minutes while scrolling
// X or YouTube in a browser, since v0.1.3): a browser drops a media session at any moment, and then the session's
// getters (GetPlaybackInfo, GetTimelineProperties, Controls...) return S_OK with a null object instead of failing;
// calling through it reads address 0, which no C++ catch can stop. The rules now:
//   1. every object a WinRT getter returns is checked before use;
//   2. one worker thread (MTA) does all the reading; WinRT's event callbacks only mark a session dirty and wake it,
//      so nothing runs twice at once for one session and the thread pool never blocks;
//   3. a burst of events (a new video changes title, state and timeline at once) is read once, 80 ms later;
//   4. a session keeps the key it got when first seen ("Brave", "Brave#2") until it goes; keys never shift;
//   5. event handlers are removed (by token) when their session goes, so a dead session sends nothing;
//   6. nothing waits for ever: asynchronous calls get 2 s (a hung app costs one round, not a thread);
//   7. the cover is read once per track, not on every event, and kept in a bounded cache.

namespace qs::win32::mpris {

QS_LOGGING_CATEGORY(logMpris, "quickshell.win32.mpris", QtInfoMsg);

using namespace winrt::Windows::Media::Control;
using winrt::Windows::Foundation::AsyncStatus;
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

constexpr auto WAIT_ASYNC = std::chrono::seconds(2);
constexpr auto BURST = std::chrono::milliseconds(80);
constexpr qsizetype ART_CACHE_FILES = 200;
constexpr qint64 ART_CACHE_BYTES = 50LL * 1024 * 1024;
constexpr uint32_t ART_MAX_BYTES = 8U * 1024 * 1024;

// An asynchronous WinRT call, waited for at most WAIT_ASYNC; nullptr-equivalent result when it did not finish.
template <typename Operation>
auto resultOf(Operation operation) -> decltype(operation.GetResults()) {
	if (!operation) return {nullptr};
	if (operation.wait_for(WAIT_ASYNC) != AsyncStatus::Completed) {
		try {
			operation.Cancel();
		} catch (...) {}
		return {nullptr};
	}
	return operation.GetResults();
}

struct SessionEntry {
	GlobalSystemMediaTransportControlsSession session {nullptr};
	QString key;
	QString app; // SourceAppUserModelId
	winrt::event_token mediaToken {};
	winrt::event_token playbackToken {};
	winrt::event_token timelineToken {};
	std::atomic<bool> dirty {true};
	QString lastTrack; // title \x1f artist \x1f album of the last cover read
	QString lastArt;
};

// Owns the GSMTC manager and sessions. The worker thread is the only one that creates, changes or drops entries;
// the GUI thread only looks them up (under the mutex) to send play/pause/... commands.
class Backend: public QObject {
public:
	static Backend* instance() {
		static auto* backend = new Backend(); // NOLINT
		return backend;
	}

	ObjectModel<MprisPlayer> players {this};

	GlobalSystemMediaTransportControlsSession session(const QString& key) {
		std::scoped_lock lock(this->mutex);
		for (const auto& entry: this->entries) {
			if (entry->key == key) return entry->session;
		}
		return nullptr;
	}

private:
	Backend() {
		if (crash::disabled("mpris")) {
			qCInfo(logMpris) << "Media sessions disabled by II_DISABLE / II_SAFE_MODE";
			return;
		}
		std::thread([this]() { this->run(); }).detach();
	}

	void wake(bool sessionsChanged) {
		{
			std::scoped_lock lock(this->mutex);
			if (sessionsChanged) this->sessionsDirty = true;
			this->anyDirty = true;
		}
		this->wakeUp.notify_one();
	}

	void run() {
		crash::setThreadSubsystem("mpris");
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			this->manager = resultOf(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
			if (!this->manager) {
				qCWarning(logMpris) << "GSMTC unavailable: the session manager did not answer";
				return;
			}
			this->manager.SessionsChanged([this](auto&&, auto&&) { this->wake(true); });
			qCInfo(logMpris) << "Media sessions ready";
		} catch (const winrt::hresult_error& e) {
			qCWarning(logMpris) << "GSMTC unavailable:" << toQ(e.message());
			return;
		}
		this->trimArtCache();

		for (;;) {
			bool refresh = false;
			{
				std::unique_lock lock(this->mutex);
				this->wakeUp.wait(lock, [this]() { return this->sessionsDirty || this->anyDirty; });
			}
			std::this_thread::sleep_for(BURST);
			{
				std::scoped_lock lock(this->mutex);
				refresh = this->sessionsDirty;
				this->sessionsDirty = false;
				this->anyDirty = false;
			}
			if (refresh) this->refreshSessions();
			for (const auto& entry: std::vector(this->entries)) { // a copy: only this thread changes the list
				if (!entry->dirty.exchange(false)) continue;
				if (auto state = this->readState(*entry)) this->post(entry->key, *state);
			}
		}
	}

	void subscribe(const std::shared_ptr<SessionEntry>& entry) {
		std::weak_ptr<SessionEntry> weak = entry;
		auto mark = [this, weak](auto&&, auto&&) {
			if (auto e = weak.lock()) {
				e->dirty = true;
				this->wake(false);
			}
		};
		try {
			entry->mediaToken = entry->session.MediaPropertiesChanged(mark);
			entry->playbackToken = entry->session.PlaybackInfoChanged(mark);
			entry->timelineToken = entry->session.TimelinePropertiesChanged(mark);
		} catch (const winrt::hresult_error& e) {
			qCInfo(logMpris) << "Session" << entry->key << "events unavailable:" << Qt::hex << static_cast<quint32>(e.code());
		}
	}

	static void unsubscribe(SessionEntry& entry) {
		if (!entry.session) return;
		try {
			if (entry.mediaToken) entry.session.MediaPropertiesChanged(entry.mediaToken);
			if (entry.playbackToken) entry.session.PlaybackInfoChanged(entry.playbackToken);
			if (entry.timelineToken) entry.session.TimelinePropertiesChanged(entry.timelineToken);
		} catch (...) {} // a session that is already gone has nothing left to remove
		entry.mediaToken = entry.playbackToken = entry.timelineToken = {};
	}

	QString newKey(const QString& app) {
		auto taken = [this](const QString& key) {
			return std::any_of(this->entries.begin(), this->entries.end(), [&](const auto& e) { return e->key == key; });
		};
		if (!taken(app)) return app;
		auto& serial = this->serials[app];
		if (serial < 2) serial = 2;
		while (taken(app + '#' + QString::number(serial))) ++serial;
		return app + '#' + QString::number(serial++);
	}

	// Worker thread: pair what Windows lists now with the entries we have. Matched by object identity; a session
	// Windows hands back as a new object (if it ever does) takes over an unmatched entry of the same app, so its key
	// and player stay.
	void refreshSessions() {
		std::vector<std::pair<GlobalSystemMediaTransportControlsSession, QString>> current;
		try {
			auto list = this->manager.GetSessions();
			if (!list) return;
			for (const auto& session: list) {
				if (!session) continue;
				current.emplace_back(session, toQ(session.SourceAppUserModelId()));
			}
		} catch (const winrt::hresult_error& e) {
			qCInfo(logMpris) << "Sessions not readable:" << Qt::hex << static_cast<quint32>(e.code());
			return;
		}

		std::vector<bool> entryMatched(this->entries.size(), false);
		std::vector<bool> sessionMatched(current.size(), false);
		for (size_t s = 0; s < current.size(); ++s) {
			for (size_t e = 0; e < this->entries.size(); ++e) {
				if (!entryMatched[e] && this->entries[e]->session == current[s].first) {
					entryMatched[e] = sessionMatched[s] = true;
					break;
				}
			}
		}

		QList<QPair<QString, QString>> added; // key, app
		QStringList removed;
		std::vector<std::shared_ptr<SessionEntry>> next;
		for (size_t e = 0; e < this->entries.size(); ++e) {
			if (entryMatched[e]) next.push_back(this->entries[e]);
		}
		for (size_t s = 0; s < current.size(); ++s) {
			if (sessionMatched[s]) continue;
			std::shared_ptr<SessionEntry> entry;
			for (size_t e = 0; e < this->entries.size(); ++e) {
				if (!entryMatched[e] && this->entries[e]->app == current[s].second) {
					entryMatched[e] = true;
					entry = this->entries[e];
					unsubscribe(*entry);
					break;
				}
			}
			if (!entry) {
				entry = std::make_shared<SessionEntry>();
				entry->app = current[s].second;
				entry->key = this->newKey(entry->app);
				added.append({entry->key, entry->app});
			}
			entry->session = current[s].first;
			entry->dirty = true;
			this->subscribe(entry);
			next.push_back(entry);
		}
		for (size_t e = 0; e < this->entries.size(); ++e) {
			if (entryMatched[e]) continue;
			unsubscribe(*this->entries[e]);
			removed.append(this->entries[e]->key);
		}
		{
			std::scoped_lock lock(this->mutex);
			this->entries = std::move(next);
		}
		if (!added.isEmpty() || !removed.isEmpty()) {
			qCInfo(logMpris) << "Sessions:" << this->entries.size() << "added" << added.size() << "removed" << removed;
			QMetaObject::invokeMethod(this, [this, added, removed]() { this->applySessions(added, removed); });
		}
	}

	// GUI thread.
	void applySessions(const QList<QPair<QString, QString>>& added, const QStringList& removed) {
		for (const auto& key: removed) {
			for (auto* player: QList(this->players.valueList())) {
				if (player->sessionId() == key) {
					this->players.removeObject(player);
					player->deleteLater();
				}
			}
		}
		for (const auto& [key, app]: added) {
			auto exists = std::any_of(this->players.valueList().begin(), this->players.valueList().end(), [&](auto* p) {
				return p->sessionId() == key;
			});
			if (!exists) this->players.insertObject(new MprisPlayer(key, app, this));
		}
	}

	void post(const QString& key, const PlayerState& state) {
		QMetaObject::invokeMethod(this, [this, key, state]() {
			for (auto* player: this->players.valueList()) {
				if (player->sessionId() == key) player->apply(state);
			}
		});
	}

	static QString artDir() {
		auto base = qEnvironmentVariable("II_DATA_DIR");
		if (base.isEmpty()) base = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("ii-windows");
		return QDir(base).filePath("cache/media-art");
	}

	// Newest ART_CACHE_FILES covers, at most ART_CACHE_BYTES in all.
	void trimArtCache() {
		auto files = QDir(artDir()).entryInfoList({"*.img"}, QDir::Files, QDir::Time);
		qint64 total = 0;
		for (qsizetype i = 0; i < files.size(); ++i) {
			total += files[i].size();
			if (i >= ART_CACHE_FILES || total > ART_CACHE_BYTES) QFile::remove(files[i].filePath());
		}
		this->savedSinceTrim = 0;
	}

	QString saveArt(const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& ref) {
		if (!ref) return {};
		try {
			auto stream = resultOf(ref.OpenReadAsync());
			if (!stream) return {};
			auto size = static_cast<uint32_t>(std::min<uint64_t>(stream.Size(), UINT32_MAX));
			if (size == 0 || size > ART_MAX_BYTES) return {};
			winrt::Windows::Storage::Streams::DataReader reader(stream);
			auto load = reader.LoadAsync(size);
			if (!load || load.wait_for(WAIT_ASYNC) != AsyncStatus::Completed) return {};
			auto loaded = load.GetResults();
			if (loaded == 0) return {};
			QByteArray bytes(static_cast<qsizetype>(loaded), Qt::Uninitialized);
			reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(bytes.data()), loaded)); // NOLINT

			auto dir = artDir();
			QDir().mkpath(dir);
			auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
			auto path = QDir(dir).filePath(QString::fromLatin1(hash) + ".img");
			if (!QFile::exists(path)) {
				QFile file(path);
				if (file.open(QFile::WriteOnly)) file.write(bytes);
				if (++this->savedSinceTrim >= 50) this->trimArtCache();
			}
			return QUrl::fromLocalFile(path).toString();
		} catch (const winrt::hresult_error&) {
			return {};
		}
	}

	// Worker thread. std::nullopt = this session cannot be read right now (it is closing): nothing is sent, and the
	// SessionsChanged that follows removes it.
	std::optional<PlayerState> readState(SessionEntry& e) {
		PlayerState state;
		try {
			auto session = e.session;
			if (!session) return std::nullopt;

			if (auto props = resultOf(session.TryGetMediaPropertiesAsync())) {
				state.title = toQ(props.Title());
				state.artist = toQ(props.Artist());
				state.album = toQ(props.AlbumTitle());
				state.albumArtist = toQ(props.AlbumArtist());
				auto track = state.title + QChar(0x1f) + state.artist + QChar(0x1f) + state.album;
				if (track != e.lastTrack || e.lastArt.isEmpty()) {
					auto art = this->saveArt(props.Thumbnail());
					// A cover that is not ready yet is asked again on the next event of the same track.
					if (!art.isEmpty() || track != e.lastTrack) e.lastArt = art;
					e.lastTrack = track;
				}
				state.artUrl = e.lastArt;
			}

			auto info = session.GetPlaybackInfo();
			if (!info) return std::nullopt;
			switch (info.PlaybackStatus()) {
			case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: state.playback = MprisPlaybackState::Playing; break;
			case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: state.playback = MprisPlaybackState::Paused; break;
			default: state.playback = MprisPlaybackState::Stopped; break;
			}
			if (auto controls = info.Controls()) {
				state.canPlay = controls.IsPlayEnabled();
				state.canPause = controls.IsPauseEnabled() || controls.IsPlayPauseToggleEnabled();
				state.canNext = controls.IsNextEnabled();
				state.canPrevious = controls.IsPreviousEnabled();
				state.canSeek = controls.IsPlaybackPositionEnabled();
				state.shuffleSupported = controls.IsShuffleEnabled();
				state.loopSupported = controls.IsRepeatEnabled();
			}
			if (auto shuffle = info.IsShuffleActive()) state.shuffle = shuffle.Value();
			if (auto repeat = info.AutoRepeatMode()) {
				auto mode = repeat.Value();
				state.loop = mode == MediaPlaybackAutoRepeatMode::Track  ? MprisLoopState::Track
				           : mode == MediaPlaybackAutoRepeatMode::List ? MprisLoopState::Playlist
				                                                       : MprisLoopState::None;
			}
			if (auto rate = info.PlaybackRate()) state.rate = rate.Value();

			if (auto timeline = session.GetTimelineProperties()) {
				auto seconds = [](winrt::Windows::Foundation::TimeSpan span) {
					return std::chrono::duration<double>(span).count();
				};
				auto start = timeline.StartTime();
				state.length = std::max(0.0, seconds(timeline.EndTime() - start));
				auto position = seconds(timeline.Position() - start);
				// Apps report the position at LastUpdatedTime; bring it to now while playing.
				auto age = std::chrono::duration<double>(winrt::clock::now() - timeline.LastUpdatedTime()).count();
				if (state.playback == MprisPlaybackState::Playing && age > 0 && age < 24 * 3600) position += age * state.rate;
				if (state.length > 0) position = std::min(position, state.length);
				state.position = std::max(0.0, position);
				state.positionUpdated = QDateTime::currentDateTimeUtc();
			}
		} catch (const winrt::hresult_error& error) {
			qCInfo(logMpris) << "Session" << e.key << "not readable:" << Qt::hex << static_cast<quint32>(error.code());
			return std::nullopt;
		}
		qCDebug(logMpris) << "Session" << e.key << "state" << state.playback;
		return state;
	}

	std::mutex mutex;
	std::condition_variable wakeUp;
	bool sessionsDirty = true;
	bool anyDirty = true;
	GlobalSystemMediaTransportControlsSessionManager manager {nullptr};
	std::vector<std::shared_ptr<SessionEntry>> entries; // written only by the worker, under the mutex
	QHash<QString, int> serials;                         // worker only
	int savedSinceTrim = 0;                              // worker only
};

// "Spotify.exe" -> "Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" -> "ZuneMusic"
QString identityFor(const QString& aumid) {
	try {
		auto info = winrt::Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(winrt::hstring(aumid.toStdWString()));
		if (info) {
			if (auto display = info.DisplayInfo()) {
				auto name = toQ(display.DisplayName());
				if (!name.isEmpty()) return name;
			}
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
	} catch (const winrt::hresult_error& e) {
		qCInfo(logMpris) << "Command for" << id << "failed:" << Qt::hex << static_cast<quint32>(e.code());
	}
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
