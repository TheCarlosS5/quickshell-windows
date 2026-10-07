#include "pipewire.hpp"
#include <algorithm>
#include <atomic>

#include <qcoreapplication.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qset.h>

#include <windows.h>
// clang-format off
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
// clang-format on

#include "../../core/logcat.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::win32::audio {

QS_LOGGING_CATEGORY(logAudio, "quickshell.win32.audio", QtInfoMsg);

namespace {

// Undocumented but stable since Windows 7; used by every audio switcher (EarTrumpet,
// SoundSwitch, AudioDeviceCmdlets) because Windows has no public "set default device" API.
// Only SetDefaultEndpoint is called; the other slots just keep the vtable layout.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig: public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
	virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

QString fromWide(const wchar_t* str) { return str ? QString::fromWCharArray(str) : QString(); }

QString deviceString(IMMDevice* device, const PROPERTYKEY& key) {
	ComPtr<IPropertyStore> store;
	if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
	PROPVARIANT value;
	PropVariantInit(&value);
	QString result;
	if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR) result = fromWide(value.pwszVal);
	PropVariantClear(&value);
	return result;
}

QString processPath(DWORD pid) {
	auto* proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) return {};
	wchar_t buf[MAX_PATH * 2];
	DWORD size = MAX_PATH * 2;
	QString path;
	if (QueryFullProcessImageNameW(proc, 0, buf, &size)) path = QString::fromWCharArray(buf, size);
	CloseHandle(proc);
	return path;
}

// "Spotify.exe" -> "Spotify" from its version resource, else the file name.
QString processDisplayName(const QString& path) {
	if (path.isEmpty()) return {};
	auto wide = path.toStdWString();
	DWORD handle = 0;
	auto size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
	if (size > 0) {
		QByteArray data(static_cast<qsizetype>(size), 0);
		if (GetFileVersionInfoW(wide.c_str(), 0, size, data.data())) {
			struct Translation {
				WORD lang;
				WORD codepage;
			}* translations = nullptr;
			UINT len = 0;
			if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &len) && len >= sizeof(Translation)) {
				wchar_t key[64];
				swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].lang, translations[0].codepage);
				wchar_t* value = nullptr;
				UINT valueLen = 0;
				if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &valueLen) && valueLen > 1) {
					return QString::fromWCharArray(value).trimmed();
				}
			}
		}
	}
	return QFileInfo(path).completeBaseName();
}

class VolumeCallback: public IAudioEndpointVolumeCallback {
public:
	explicit VolumeCallback(QString deviceId): deviceId(std::move(deviceId)) {}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override {
		auto refs = --this->refs;
		if (refs == 0) delete this;
		return refs;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
			*out = static_cast<IAudioEndpointVolumeCallback*>(this);
			this->AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

	// Called on a Core Audio thread: hop to the GUI thread.
	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
		auto volume = data->fMasterVolume;
		auto muted = data->bMuted != FALSE;
		auto id = this->deviceId;
		QMetaObject::invokeMethod(
		    AudioBackend::instance(),
		    [id, volume, muted]() { AudioBackend::instance()->onEndpointVolume(id, volume, muted); },
		    Qt::QueuedConnection
		);
		return S_OK;
	}

private:
	std::atomic<ULONG> refs = 1;
	QString deviceId;
};

class DeviceNotifications: public IMMNotificationClient {
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
	ULONG STDMETHODCALLTYPE Release() override {
		auto refs = --this->refs;
		if (refs == 0) delete this;
		return refs;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
			*out = static_cast<IMMNotificationClient*>(this);
			this->AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return rescan(); }
	HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return rescan(); }
	HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return rescan(); }
	HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return rescan(); }
	HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
	static HRESULT rescan() {
		QMetaObject::invokeMethod(AudioBackend::instance(), &AudioBackend::scheduleDeviceRescan, Qt::QueuedConnection);
		return S_OK;
	}

	std::atomic<ULONG> refs = 1;
};

} // namespace

// PwNodeAudio

void PwNodeAudio::setMuted(bool muted) {
	if (muted == this->mMuted) return;
	this->mMuted = muted;
	this->apply(this->mVolume, muted);
	emit this->mutedChanged();
}

void PwNodeAudio::setVolume(float volume) {
	volume = std::clamp(volume, 0.0F, 1.0F); // Windows volume scalars stop at 100 %
	if (qFuzzyCompare(volume, this->mVolume)) return;
	this->mVolume = volume;
	this->apply(volume, this->mMuted);
	emit this->volumesChanged();
}

QVector<PwAudioChannel::Enum> PwNodeAudio::channels() const {
	if (this->channelCount == 1) return {PwAudioChannel::Mono};
	return {PwAudioChannel::FrontLeft, PwAudioChannel::FrontRight};
}

QVector<float> PwNodeAudio::volumes() const {
	return QVector<float>(this->channelCount == 1 ? 1 : 2, this->mVolume);
}

void PwNodeAudio::setVolumes(const QVector<float>& volumes) {
	if (volumes.isEmpty()) return;
	auto sum = 0.0F;
	for (auto v: volumes) sum += v;
	this->setVolume(sum / static_cast<float>(volumes.size()));
}

void PwNodeAudio::update(float volume, bool muted, quint32 channelCount) {
	if (channelCount != this->channelCount && channelCount > 0) {
		this->channelCount = channelCount;
		emit this->channelsChanged();
	}
	if (!qFuzzyCompare(volume + 1, this->mVolume + 1)) {
		this->mVolume = volume;
		emit this->volumesChanged();
	}
	if (muted != this->mMuted) {
		this->mMuted = muted;
		emit this->mutedChanged();
	}
}

// PwNode

PwNode::PwNode(quint32 id, QString name, PwNodeType::Flags type, PwNodeAudio::Apply apply, QObject* parent)
    : QObject(parent)
    , mId(id)
    , mName(std::move(name))
    , mType(type)
    , mAudio(new PwNodeAudio(std::move(apply), this)) {}

void PwNode::setInfo(const QString& description, const QString& nickname, const QVariantMap& properties) {
	if (description == this->mDescription && nickname == this->mNickname && properties == this->mProperties) {
		return;
	}
	this->mDescription = description;
	this->mNickname = nickname;
	this->mProperties = properties;
	emit this->propertiesChanged();
}

// Pipewire (QML singleton)

Pipewire::Pipewire(QObject* parent): QObject(parent) {
	auto* backend = AudioBackend::instance();
	QObject::connect(backend, &AudioBackend::defaultSinkChanged, this, &Pipewire::defaultAudioSinkChanged);
	QObject::connect(backend, &AudioBackend::defaultSourceChanged, this, &Pipewire::defaultAudioSourceChanged);
	QObject::connect(backend, &AudioBackend::readyChanged, this, &Pipewire::readyChanged);
}

UntypedObjectModel* Pipewire::nodes() { return &AudioBackend::instance()->nodes; }
UntypedObjectModel* Pipewire::links() { return &AudioBackend::instance()->links; }
PwNode* Pipewire::defaultAudioSink() { return AudioBackend::instance()->defaultSink; }
PwNode* Pipewire::defaultAudioSource() { return AudioBackend::instance()->defaultSource; }
void Pipewire::setPreferredSink(PwNode* node) { AudioBackend::instance()->setDefault(node); }
void Pipewire::setPreferredSource(PwNode* node) { AudioBackend::instance()->setDefault(node); }
bool Pipewire::isReady() { return AudioBackend::instance()->ready; }

// AudioBackend

struct AudioBackend::Device {
	QString id;
	bool capture = false;
	ComPtr<IMMDevice> device;
	ComPtr<IAudioEndpointVolume> volume;
	VolumeCallback* callback = nullptr;
	PwNode* node = nullptr;
};

struct AudioBackend::Session {
	ComPtr<ISimpleAudioVolume> volume;
	PwNode* node = nullptr;
	bool seen = false;
};

AudioBackend* AudioBackend::instance() {
	static auto* backend = new AudioBackend(); // NOLINT
	return backend;
}

AudioBackend::AudioBackend(QObject* parent): QObject(parent) {
	IMMDeviceEnumerator* enumerator = nullptr;
	auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
	if (FAILED(hr)) {
		qCWarning(logAudio) << "Core Audio unavailable" << Qt::hex << hr;
		return;
	}
	this->enumerator = enumerator;

	auto* client = new DeviceNotifications();
	enumerator->RegisterEndpointNotificationCallback(client);
	this->notificationClient = client;

	this->deviceDebounce.setSingleShot(true);
	this->deviceDebounce.setInterval(150);
	QObject::connect(&this->deviceDebounce, &QTimer::timeout, this, &AudioBackend::rescanDevices);

	// Sessions come and go with playback; polling keeps it simple and cheap.
	this->sessionPoll.setInterval(1500);
	QObject::connect(&this->sessionPoll, &QTimer::timeout, this, &AudioBackend::rescanSessions);
	this->sessionPoll.start();

	this->rescanDevices();
	this->rescanSessions();
	this->ready = true;
	emit this->readyChanged();
}

void AudioBackend::scheduleDeviceRescan() {
	if (!this->deviceDebounce.isActive()) this->deviceDebounce.start();
}

void AudioBackend::rescanDevices() {
	auto* enumerator = static_cast<IMMDeviceEnumerator*>(this->enumerator);
	if (!enumerator) return;

	QSet<QString> present;

	for (auto flow: {eRender, eCapture}) {
		ComPtr<IMMDeviceCollection> collection;
		if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) continue;
		UINT count = 0;
		collection->GetCount(&count);

		for (UINT i = 0; i < count; ++i) {
			ComPtr<IMMDevice> device;
			if (FAILED(collection->Item(i, &device))) continue;
			LPWSTR rawId = nullptr;
			device->GetId(&rawId);
			auto id = fromWide(rawId);
			CoTaskMemFree(rawId);
			present.insert(id);

			auto name = deviceString(device.Get(), PKEY_Device_FriendlyName);
			auto capture = flow == eCapture;

			auto* entry = this->devices.value(id);
			if (!entry) {
				entry = new Device();
				entry->id = id;
				entry->capture = capture;
				entry->device = device;
				device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &entry->volume);

				auto volumeIface = entry->volume;
				entry->node = new PwNode(
				    this->nextId++,
				    id,
				    capture ? PwNodeType::AudioSource : PwNodeType::AudioSink,
				    [volumeIface](float volume, bool muted) {
					    if (!volumeIface) return;
					    volumeIface->SetMasterVolumeLevelScalar(volume, nullptr);
					    volumeIface->SetMute(muted, nullptr);
				    },
				    this
				);

				if (entry->volume) {
					entry->callback = new VolumeCallback(id);
					entry->volume->RegisterControlChangeNotify(entry->callback);
				}

				this->devices.insert(id, entry);
				this->nodes.insertObject(entry->node);
				qCInfo(logAudio) << (capture ? "Source" : "Sink") << "added:" << name;
			}

			entry->node->setInfo(
			    name,
			    name,
			    {
			        {"node.name", id},
			        {"node.description", name},
			        {"device.description", deviceString(device.Get(), PKEY_Device_DeviceDesc)},
			        {"media.class", capture ? "Audio/Source" : "Audio/Sink"},
			    }
			);

			if (entry->volume) {
				float volume = 0;
				BOOL muted = FALSE;
				UINT channels = 2;
				entry->volume->GetMasterVolumeLevelScalar(&volume);
				entry->volume->GetMute(&muted);
				entry->volume->GetChannelCount(&channels);
				entry->node->audio()->update(volume, muted != FALSE, channels);
			}
		}
	}

	for (auto it = this->devices.begin(); it != this->devices.end();) {
		auto* entry = it.value();
		if (present.contains(it.key())) {
			++it;
			continue;
		}
		if (entry->volume && entry->callback) entry->volume->UnregisterControlChangeNotify(entry->callback);
		if (entry->callback) entry->callback->Release();
		this->nodes.removeObject(entry->node);
		entry->node->deleteLater();
		delete entry;
		it = this->devices.erase(it);
	}

	auto defaultFor = [&](EDataFlow flow) -> PwNode* {
		ComPtr<IMMDevice> device;
		if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) return nullptr;
		LPWSTR rawId = nullptr;
		device->GetId(&rawId);
		auto id = fromWide(rawId);
		CoTaskMemFree(rawId);
		auto* entry = this->devices.value(id);
		return entry ? entry->node : nullptr;
	};

	if (auto* sink = defaultFor(eRender); sink != this->defaultSink) {
		this->defaultSink = sink;
		emit this->defaultSinkChanged();
	}
	if (auto* source = defaultFor(eCapture); source != this->defaultSource) {
		this->defaultSource = source;
		emit this->defaultSourceChanged();
	}
}

void AudioBackend::onEndpointVolume(const QString& deviceId, float volume, bool muted) {
	if (auto* entry = this->devices.value(deviceId)) {
		UINT channels = 2;
		if (entry->volume) entry->volume->GetChannelCount(&channels);
		entry->node->audio()->update(volume, muted, channels);
	}
}

void AudioBackend::rescanSessions() {
	for (auto* session: this->sessions) session->seen = false;

	for (auto* device: this->devices) {
		ComPtr<IAudioSessionManager2> manager;
		if (FAILED(device->device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager))) continue;
		ComPtr<IAudioSessionEnumerator> sessionList;
		if (FAILED(manager->GetSessionEnumerator(&sessionList))) continue;
		int count = 0;
		sessionList->GetCount(&count);

		for (int i = 0; i < count; ++i) {
			ComPtr<IAudioSessionControl> control;
			if (FAILED(sessionList->GetSession(i, &control))) continue;
			ComPtr<IAudioSessionControl2> control2;
			if (FAILED(control.As(&control2))) continue;
			if (control2->IsSystemSoundsSession() == S_OK) continue;

			// PipeWire streams exist while they play; match that with active sessions.
			AudioSessionState state = AudioSessionStateInactive;
			control2->GetState(&state);
			if (state != AudioSessionStateActive) continue;

			LPWSTR rawKey = nullptr;
			control2->GetSessionInstanceIdentifier(&rawKey);
			auto key = fromWide(rawKey);
			CoTaskMemFree(rawKey);
			if (key.isEmpty()) continue;

			auto* session = this->sessions.value(key);
			if (!session) {
				DWORD pid = 0;
				control2->GetProcessId(&pid);
				auto path = processPath(pid);
				LPWSTR rawName = nullptr;
				control2->GetDisplayName(&rawName);
				auto displayName = fromWide(rawName);
				CoTaskMemFree(rawName);
				// Display names are often empty or "@resource" references.
				if (displayName.isEmpty() || displayName.startsWith('@')) displayName = processDisplayName(path);
				auto binary = QFileInfo(path).fileName();

				session = new Session();
				control.As(&session->volume);
				auto volumeIface = session->volume;
				session->node = new PwNode(
				    this->nextId++,
				    binary,
				    device->capture ? PwNodeType::AudioInStream : PwNodeType::AudioOutStream,
				    [volumeIface](float volume, bool muted) {
					    if (!volumeIface) return;
					    volumeIface->SetMasterVolume(volume, nullptr);
					    volumeIface->SetMute(muted, nullptr);
				    },
				    this
				);
				session->node->setInfo(
				    displayName,
				    displayName,
				    {
				        {"application.name", displayName},
				        {"application.process.binary", binary},
				        {"application.process.id", static_cast<qint64>(pid)},
				        {"application.icon-name", QFileInfo(path).completeBaseName().toLower()},
				        {"media.name", QString()},
				        {"node.name", binary},
				        {"media.class", device->capture ? "Stream/Input/Audio" : "Stream/Output/Audio"},
				    }
				);
				this->sessions.insert(key, session);
				this->nodes.insertObject(session->node);
			}

			session->seen = true;
			if (session->volume) {
				float volume = 0;
				BOOL muted = FALSE;
				session->volume->GetMasterVolume(&volume);
				session->volume->GetMute(&muted);
				session->node->audio()->update(volume, muted != FALSE, 2);
			}
		}
	}

	for (auto it = this->sessions.begin(); it != this->sessions.end();) {
		if (it.value()->seen) {
			++it;
			continue;
		}
		this->nodes.removeObject(it.value()->node);
		it.value()->node->deleteLater();
		delete it.value();
		it = this->sessions.erase(it);
	}
}

void AudioBackend::setDefault(PwNode* node) {
	if (!node || node->isStream()) return;
	ComPtr<IPolicyConfig> policy;
	if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)))) {
		qCWarning(logAudio) << "Cannot change the default device: IPolicyConfig unavailable";
		return;
	}
	auto id = node->name().toStdWString();
	for (auto role: {eConsole, eMultimedia, eCommunications}) policy->SetDefaultEndpoint(id.c_str(), role);
	qCInfo(logAudio) << "Default device set to" << node->description();
}

} // namespace qs::win32::audio
