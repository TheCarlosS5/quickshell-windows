#pragma once

// Quickshell.Services.Pipewire for Windows, backed by Core Audio:
//  - audio devices (render = sinks, capture = sources) via IMMDeviceEnumerator +
//    IAudioEndpointVolume, with change notifications
//  - application streams via audio sessions (IAudioSessionManager2 + ISimpleAudioVolume)
//  - default device switching via the IPolicyConfig interface audio switchers use
// The QML API matches Quickshell's PipeWire module so shells use it unchanged.

#include <functional>

#include <qflags.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qqmllist.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qvariant.h>

#include "../../core/model.hpp"

namespace qs::win32::audio {

class PwNodeType: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Flag : quint8 {
		Untracked = 0b0,
		Audio = 0b1,
		Video = 0b10,
		Stream = 0b100,
		Source = 0b1000,
		Sink = 0b10000,
		AudioSink = Audio | Sink,
		AudioSource = Audio | Source,
		AudioDuplex = Audio | Sink | Source,
		AudioOutStream = Audio | Sink | Stream,
		AudioInStream = Audio | Source | Stream,
		VideoSource = Video | Source,
		VideoSink = Video | Sink,
	};
	Q_ENUM(Flag);
	Q_DECLARE_FLAGS(Flags, Flag);
	Q_FLAG(Flags);
};

class PwAudioChannel: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	enum Enum : quint8 {
		Unknown = 0,
		Mono = 2,
		FrontLeft = 3,
		FrontRight = 4,
	};
	Q_ENUM(Enum);
};

class PwNodeAudio: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY mutedChanged);
	Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumesChanged);
	Q_PROPERTY(QVector<qs::win32::audio::PwAudioChannel::Enum> channels READ channels NOTIFY channelsChanged);
	Q_PROPERTY(QVector<float> volumes READ volumes WRITE setVolumes NOTIFY volumesChanged);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("PwNodeAudio cannot be created directly");

public:
	using Apply = std::function<void(float volume, bool muted)>;

	explicit PwNodeAudio(Apply apply, QObject* parent = nullptr)
	    : QObject(parent)
	    , apply(std::move(apply)) {}

	[[nodiscard]] bool isMuted() const { return this->mMuted; }
	void setMuted(bool muted);
	[[nodiscard]] float volume() const { return this->mVolume; }
	void setVolume(float volume);
	[[nodiscard]] QVector<PwAudioChannel::Enum> channels() const;
	[[nodiscard]] QVector<float> volumes() const;
	void setVolumes(const QVector<float>& volumes);

	// Called from the backend when Windows reports a change.
	void update(float volume, bool muted, quint32 channelCount);

signals:
	void mutedChanged();
	void volumesChanged();
	void channelsChanged();

private:
	Apply apply;
	float mVolume = 0;
	bool mMuted = false;
	quint32 channelCount = 2;
};

class PwNode: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(quint32 id READ id CONSTANT);
	Q_PROPERTY(QString name READ name CONSTANT);
	Q_PROPERTY(QString description READ description NOTIFY propertiesChanged);
	Q_PROPERTY(QString nickname READ nickname NOTIFY propertiesChanged);
	Q_PROPERTY(bool isSink READ isSink CONSTANT);
	Q_PROPERTY(bool isStream READ isStream CONSTANT);
	Q_PROPERTY(qs::win32::audio::PwNodeType::Flags type READ type CONSTANT);
	Q_PROPERTY(QVariantMap properties READ properties NOTIFY propertiesChanged);
	Q_PROPERTY(qs::win32::audio::PwNodeAudio* audio READ audio CONSTANT);
	Q_PROPERTY(bool ready READ isReady CONSTANT);
	// clang-format on
	QML_ELEMENT;
	QML_UNCREATABLE("PwNodes cannot be created directly");

public:
	PwNode(quint32 id, QString name, PwNodeType::Flags type, PwNodeAudio::Apply apply, QObject* parent);

	[[nodiscard]] quint32 id() const { return this->mId; }
	[[nodiscard]] QString name() const { return this->mName; }
	[[nodiscard]] QString description() const { return this->mDescription; }
	[[nodiscard]] QString nickname() const { return this->mNickname; }
	[[nodiscard]] bool isSink() const { return this->mType.testFlag(PwNodeType::Sink); }
	[[nodiscard]] bool isStream() const { return this->mType.testFlag(PwNodeType::Stream); }
	[[nodiscard]] PwNodeType::Flags type() const { return this->mType; }
	[[nodiscard]] QVariantMap properties() const { return this->mProperties; }
	[[nodiscard]] PwNodeAudio* audio() const { return this->mAudio; }
	[[nodiscard]] static bool isReady() { return true; }

	void setInfo(const QString& description, const QString& nickname, const QVariantMap& properties);

signals:
	void propertiesChanged();

private:
	quint32 mId;
	QString mName;
	QString mDescription;
	QString mNickname;
	PwNodeType::Flags mType;
	QVariantMap mProperties;
	PwNodeAudio* mAudio;
};

class AudioBackend;

class Pipewire: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(UntypedObjectModel* nodes READ nodes CONSTANT);
	Q_PROPERTY(UntypedObjectModel* links READ links CONSTANT);
	Q_PROPERTY(UntypedObjectModel* linkGroups READ links CONSTANT);
	Q_PROPERTY(qs::win32::audio::PwNode* defaultAudioSink READ defaultAudioSink NOTIFY defaultAudioSinkChanged);
	Q_PROPERTY(qs::win32::audio::PwNode* defaultAudioSource READ defaultAudioSource NOTIFY defaultAudioSourceChanged);
	Q_PROPERTY(qs::win32::audio::PwNode* preferredDefaultAudioSink READ defaultAudioSink WRITE setPreferredSink NOTIFY defaultAudioSinkChanged);
	Q_PROPERTY(qs::win32::audio::PwNode* preferredDefaultAudioSource READ defaultAudioSource WRITE setPreferredSource NOTIFY defaultAudioSourceChanged);
	Q_PROPERTY(bool ready READ isReady NOTIFY readyChanged);
	// clang-format on
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit Pipewire(QObject* parent = nullptr);

	[[nodiscard]] static UntypedObjectModel* nodes();
	[[nodiscard]] static UntypedObjectModel* links();
	[[nodiscard]] static PwNode* defaultAudioSink();
	[[nodiscard]] static PwNode* defaultAudioSource();
	static void setPreferredSink(PwNode* node);
	static void setPreferredSource(PwNode* node);
	[[nodiscard]] static bool isReady();

signals:
	void defaultAudioSinkChanged();
	void defaultAudioSourceChanged();
	void readyChanged();
};

/// Keeps nodes bound. Core Audio objects need no binding, so this only stores the list.
class PwObjectTracker: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QList<QObject*> objects READ objects WRITE setObjects NOTIFY objectsChanged);
	QML_ELEMENT;

public:
	explicit PwObjectTracker(QObject* parent = nullptr): QObject(parent) {}
	[[nodiscard]] QList<QObject*> objects() const { return this->mObjects; }
	void setObjects(QList<QObject*> objects) {
		this->mObjects = std::move(objects);
		emit this->objectsChanged();
	}

signals:
	void objectsChanged();

private:
	QList<QObject*> mObjects;
};

/// Windows has no audio graph to inspect; link groups are always empty.
class PwNodeLinkTracker: public QObject {
	Q_OBJECT;
	Q_PROPERTY(qs::win32::audio::PwNode* node READ node WRITE setNode NOTIFY nodeChanged);
	Q_PROPERTY(QList<QObject*> linkGroups READ linkGroups NOTIFY linkGroupsChanged);
	QML_ELEMENT;

public:
	explicit PwNodeLinkTracker(QObject* parent = nullptr): QObject(parent) {}
	[[nodiscard]] PwNode* node() const { return this->mNode; }
	void setNode(PwNode* node) {
		this->mNode = node;
		emit this->nodeChanged();
	}
	[[nodiscard]] static QList<QObject*> linkGroups() { return {}; }

signals:
	void nodeChanged();
	void linkGroupsChanged();

private:
	QPointer<PwNode> mNode;
};

// Owns all Core Audio state; lives on the GUI thread.
class AudioBackend: public QObject {
	Q_OBJECT;

public:
	static AudioBackend* instance();

	ObjectModel<PwNode> nodes {this};
	ObjectModel<QObject> links {this};
	QPointer<PwNode> defaultSink;
	QPointer<PwNode> defaultSource;
	bool ready = false;

	void setDefault(PwNode* node);

signals:
	void defaultSinkChanged();
	void defaultSourceChanged();
	void readyChanged();

public slots:
	void scheduleDeviceRescan();
	void onEndpointVolume(const QString& deviceId, float volume, bool muted);

private slots:
	void rescanDevices();
	void rescanSessions();

private:
	explicit AudioBackend(QObject* parent = nullptr);

	struct Device;
	struct Session;
	QHash<QString, Device*> devices;
	QHash<QString, Session*> sessions;
	quint32 nextId = 1;
	QTimer deviceDebounce;
	QTimer sessionPoll;
	void* enumerator = nullptr;      // IMMDeviceEnumerator*
	void* notificationClient = nullptr;
};

} // namespace qs::win32::audio

Q_DECLARE_OPERATORS_FOR_FLAGS(qs::win32::audio::PwNodeType::Flags);
