#include "bluetooth.hpp"
#include "hardware_async.hpp"
#include <qdesktopservices.h>
#include <qmetaobject.h>
#include <qset.h>
#include <qurl.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace qs::win32::bluetooth {
namespace BT = winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;
using hardware::Delivery;
namespace {
QString toQ(const winrt::hstring& text) { return QString::fromWCharArray(text.c_str()); }
template <typename T> QString enumName(int value) {
	auto e = T::staticMetaObject.enumerator(T::staticMetaObject.indexOfEnumerator("Enum"));
	auto key = e.valueToKey(value); return key ? QString::fromLatin1(key) : QStringLiteral("Unknown");
}
void settings() { QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth"))); }
BluetoothAdapterState::Enum radioState(Radio const& radio) {
	if (radio.State() == RadioState::On) return BluetoothAdapterState::Enabled;
	if (radio.State() == RadioState::Disabled) return BluetoothAdapterState::Blocked;
	return BluetoothAdapterState::Disabled;
}
void stop(DeviceWatcher const& watcher) {
	try {
		if (watcher && (watcher.Status() == DeviceWatcherStatus::Started || watcher.Status() == DeviceWatcherStatus::EnumerationCompleted)) watcher.Stop();
	} catch (const winrt::hresult_error&) {}
}
QString propertyText(DeviceInformation const& info, const wchar_t* key) {
	auto value = info.Properties().TryLookup(key);
	if (value) { auto text = value.try_as<winrt::Windows::Foundation::IReference<winrt::hstring>>(); if (text) return toQ(text.Value()); }
	return {};
}
bool propertyBool(DeviceInformation const& info, const wchar_t* key) {
	auto value = info.Properties().TryLookup(key);
	if (value) { auto boolean = value.try_as<winrt::Windows::Foundation::IReference<bool>>(); if (boolean) return boolean.Value(); }
	return false;
}
}
QString BluetoothAdapterState::toString(Enum value) { return enumName<BluetoothAdapterState>(value); }
QString BluetoothDeviceState::toString(Enum value) { return enumName<BluetoothDeviceState>(value); }
struct BluetoothAdapter::Runtime {
	Radio radio {nullptr}; winrt::event_token stateToken {};
	std::shared_ptr<Delivery> delivery;
	bool changing = false;
};
BluetoothAdapter::BluetoothAdapter(QString id, QObject* parent): QObject(parent), runtime(std::make_shared<Runtime>()), mId(std::move(id)) {
	runtime->delivery = std::make_shared<Delivery>(this);
	discoveryTimeout.setSingleShot(true); discoveryTimeout.setInterval(30000);
	QObject::connect(&discoveryTimeout, &QTimer::timeout, this, [this] { setDiscovering(false); });
}
BluetoothAdapter::~BluetoothAdapter() {
	runtime->delivery->close();
	try { if (runtime->radio) runtime->radio.StateChanged(runtime->stateToken); } catch (const winrt::hresult_error&) {}
}
void BluetoothAdapter::setEnabled(bool value) {
	if (!runtime->radio || runtime->changing || enabled() == value) return;
	auto delivery = runtime->delivery;
	auto failure = [delivery, this](QString message) { delivery->post([this, message] {
		error = message; runtime->changing = false;
		try { mState = radioState(runtime->radio); } catch (const winrt::hresult_error&) { mState = BluetoothAdapterState::Blocked; }
		emit changed();
	}); };
	try {
		// Start the consent operation on the GUI thread, only after an explicit toggle.
		runtime->changing = true; mState = value ? BluetoothAdapterState::Enabling : BluetoothAdapterState::Disabling; emit changed();
		hardware::complete(Radio::RequestAccessAsync(), [delivery, this, value, failure](auto access) {
			delivery->post([this, delivery, value, access, failure] {
				if (access != RadioAccessStatus::Allowed) { failure(QStringLiteral("Radio access denied")); return; }
				try {
					hardware::complete(runtime->radio.SetStateAsync(value ? RadioState::On : RadioState::Off), [delivery, this, failure](auto result) {
						if (result != RadioAccessStatus::Allowed) { failure(QStringLiteral("Radio state rejected")); return; }
						delivery->post([this] { runtime->changing = false; mState = radioState(runtime->radio); error.clear(); emit changed(); });
					}, failure);
				} catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
			});
		}, failure);
	} catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
}
void BluetoothAdapter::setDiscovering(bool value) {
	value = value && enabled();
	if (mDiscovering == value) return;
	mDiscovering = value;
	if (value) discoveryTimeout.start(); else discoveryTimeout.stop();
	emit changed(); emit discoveryRequested(value);
}
void BluetoothAdapter::setDiscoverable(bool value) { if (value) settings(); }
void BluetoothAdapter::setDiscoverableTimeout(quint32 value) { if (value) settings(); }
void BluetoothAdapter::setPairable(bool value) { if (value != enabled()) settings(); }
void BluetoothAdapter::setPairableTimeout(quint32 value) { if (value) settings(); }
struct BluetoothDevice::Runtime {
	DeviceSources sources;
	DeviceInformation info {nullptr};
	BT::BluetoothDevice classic {nullptr}; BT::BluetoothLEDevice le {nullptr};
	BT::GenericAttributeProfile::GattSession session {nullptr};
	QTimer connectionTimeout;
	quint64 connectionGeneration = 0, projectionGeneration = 0;
	winrt::event_token connectionToken {};
	winrt::Windows::Foundation::IAsyncInfo pairingOperation {nullptr};
	DeviceInformationCustomPairing custom {nullptr}; winrt::event_token pairingToken {};
	std::shared_ptr<Delivery> delivery;
	bool opened = false, cancelled = false, lowEnergy = false, observeSuspended = false;
	void revokePairing() { if (custom) { custom.PairingRequested(pairingToken); custom = nullptr; } }
	void releaseConnection() {
		connectionTimeout.stop(); ++connectionGeneration;
		if (session) { session.MaintainConnection(false); session.Close(); session = nullptr; }
		// Release every LE reference owned here; another app may still own its link.
		++projectionGeneration; observeSuspended = true;
		if (le) { le.ConnectionStatusChanged(connectionToken); le.Close(); le = nullptr; opened = false; }
	}
};
BluetoothDevice::BluetoothDevice(QString id, BluetoothAdapter* adapter): QObject(adapter), runtime(std::make_shared<Runtime>()), mId(std::move(id)), mAdapter(adapter) {
	runtime->delivery = std::make_shared<Delivery>(this);
	runtime->connectionTimeout.setSingleShot(true); runtime->connectionTimeout.setInterval(30000);
	QObject::connect(&runtime->connectionTimeout, &QTimer::timeout, this, [this] {
		try { runtime->releaseConnection(); } catch (const winrt::hresult_error&) {}
		mConnecting = false; error = QStringLiteral("Bluetooth LE connection timed out"); emit changed();
	});
}
BluetoothDevice::~BluetoothDevice() {
	runtime->delivery->close();
	try {
		if (runtime->pairingOperation) runtime->pairingOperation.Cancel();
		runtime->revokePairing();
		runtime->releaseConnection();
		if (runtime->classic) { runtime->classic.ConnectionStatusChanged(runtime->connectionToken); runtime->classic.Close(); }
		if (runtime->le) { runtime->le.ConnectionStatusChanged(runtime->connectionToken); runtime->le.Close(); }
	} catch (const winrt::hresult_error&) {}
}
void BluetoothDevice::connect() {
	if (runtime->lowEnergy && paired()) {
		if (mConnecting || mConnected) return;
		mConnecting = true; error.clear(); emit changed(); runtime->connectionTimeout.start();
		const auto generation = ++runtime->connectionGeneration;
		auto delivery = runtime->delivery;
		auto failure = [this, delivery, generation](QString message) { delivery->post([this, generation, message] {
			if (generation != runtime->connectionGeneration) return;
			try { runtime->releaseConnection(); } catch (const winrt::hresult_error&) {}
			mConnecting = false; error = message; emit changed();
		}); };
		runtime->observeSuspended = false;
		if (!runtime->le) ++runtime->projectionGeneration;
		auto begin = [this, delivery, generation, failure](BT::BluetoothLEDevice native) {
			delivery->post([this, delivery, native, generation, failure] {
				if (generation != runtime->connectionGeneration) { if (native) native.Close(); return; }
				if (!native) { failure(QStringLiteral("LE device unavailable")); return; }
				try {
					if (!runtime->le) {
						runtime->le = native; runtime->opened = true;
						runtime->connectionToken = native.ConnectionStatusChanged([delivery, this, generation](auto const& native, auto const&) {
							auto connected = native.ConnectionStatus() == BT::BluetoothConnectionStatus::Connected;
							delivery->post([this, connected, generation] {
								if (generation != runtime->connectionGeneration) return;
								mConnected = connected;
								if (connected) { mConnecting = false; runtime->connectionTimeout.stop(); }
								emit changed();
							});
						});
					}
					// FromIdAsync or another client may have connected before subscription.
					mConnected = native.ConnectionStatus() == BT::BluetoothConnectionStatus::Connected;
					if (mConnected) { mConnecting = false; runtime->connectionTimeout.stop(); }
					emit changed();
					hardware::complete(BT::GenericAttributeProfile::GattSession::FromDeviceIdAsync(native.BluetoothDeviceId()),
						[this, delivery, generation, failure](auto session) { delivery->post([this, session, generation, failure] {
							if (generation != runtime->connectionGeneration) { if (session) session.Close(); return; }
							if (!session || !session.CanMaintainConnection()) { failure(QStringLiteral("LE connection unavailable")); return; }
							try {
								runtime->session = session; session.MaintainConnection(true);
								if (runtime->le) mConnected = runtime->le.ConnectionStatus() == BT::BluetoothConnectionStatus::Connected;
								if (mConnected) { mConnecting = false; runtime->connectionTimeout.stop(); }
								emit changed();
							}
							catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
						}); }, failure);
				} catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
			});
		};
		try {
			if (runtime->le) begin(runtime->le);
			else hardware::complete(BT::BluetoothLEDevice::FromIdAsync(runtime->info.Id()), begin, failure);
		} catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
		return;
	}
	// A Windows Bluetooth object does not connect every profile (audio/HID/RFCOMM).
	// Settings owns those connections. Never fake Connected, or pair implicitly.
	error = QStringLiteral("Use Windows Bluetooth settings to connect this profile"); emit changed(); settings();
}
void BluetoothDevice::disconnect() {
	if (runtime->lowEnergy && (runtime->le || runtime->session || mConnecting)) {
		try { runtime->releaseConnection(); error.clear(); } catch (const winrt::hresult_error& e) { error = toQ(e.message()); }
		mConnecting = false; emit changed(); return;
	}
	error = QStringLiteral("Use Windows Bluetooth settings to disconnect this profile"); emit changed(); settings();
}
void BluetoothDevice::setTrusted(bool value) { if (value != paired()) settings(); }
void BluetoothDevice::setBlocked(bool value) { if (value) settings(); }
void BluetoothDevice::setWakeAllowed(bool value) { if (value) settings(); }
void BluetoothDevice::pair() {
	if (mPairing || mPaired || !runtime->info || runtime->pairingOperation) return;
	auto delivery = runtime->delivery;
	auto failure = [delivery, this](QString message) { delivery->post([this, message] {
		mPairing = false; error = message; runtime->pairingOperation = nullptr; runtime->revokePairing(); emit changed();
		if (!runtime->cancelled) settings();
	}); };
	try {
		mPairing = true; runtime->cancelled = false; error.clear(); emit changed();
		// ConfirmOnly is authorized by this explicit Pair action and still receives
		// Windows' consent dialog. PIN/password/number comparison ceremonies are
		// handled by Settings; never silently accept a code we have not displayed.
		runtime->custom = runtime->info.Pairing().Custom();
		runtime->pairingToken = runtime->custom.PairingRequested([](auto const&, auto const& request) {
			if (request.PairingKind() == DevicePairingKinds::ConfirmOnly) request.Accept();
		});
		auto operation = runtime->custom.PairAsync(DevicePairingKinds::ConfirmOnly); runtime->pairingOperation = operation;
		hardware::complete(operation, [delivery, this](auto result) {
			delivery->post([this, result] {
				mPairing = false; runtime->pairingOperation = nullptr; runtime->revokePairing();
				mPaired = runtime->info.Pairing().IsPaired();
				if (result.Status() != DevicePairingResultStatus::Paired && result.Status() != DevicePairingResultStatus::AlreadyPaired) {
					error = QStringLiteral("Pairing failed (%1)").arg(int(result.Status())); if (!runtime->cancelled) settings();
				}
				emit changed();
			});
		}, failure);
	} catch (const winrt::hresult_error& e) { failure(toQ(e.message())); }
}
void BluetoothDevice::cancelPair() {
	runtime->cancelled = true;
	try { if (runtime->pairingOperation) runtime->pairingOperation.Cancel(); } catch (const winrt::hresult_error&) {}
	mPairing = false; emit changed();
}
void BluetoothDevice::forget() {
	if (!mPaired || !runtime->info || mPairing) return;
	auto delivery = runtime->delivery;
	try {
		runtime->releaseConnection(); mConnecting = false;
		hardware::complete(runtime->info.Pairing().UnpairAsync(), [delivery, this](auto result) {
			delivery->post([this, result] {
				mPaired = runtime->info.Pairing().IsPaired();
				if (result.Status() != DeviceUnpairingResultStatus::Unpaired && result.Status() != DeviceUnpairingResultStatus::AlreadyUnpaired)
					error = QStringLiteral("Unpairing failed (%1)").arg(int(result.Status()));
				emit changed();
			});
		}, [delivery, this](QString message) { delivery->post([this, message] { error = message; emit changed(); }); });
	} catch (const winrt::hresult_error& e) { error = toQ(e.message()); emit changed(); }
}
struct Bluetooth::Runtime {
	std::shared_ptr<Delivery> delivery;
	DeviceWatcher radios {nullptr};
	std::vector<DeviceWatcher> paired, discovery;
	QTimer retry;
	QHash<QString, quint64> radioVersions;
	int pending = 0;
	bool enumerated = false, apartment = false;
	quint64 scanGeneration = 0, pairedGeneration = 0, radioGeneration = 0, watcherSequence = 0;
};
Bluetooth::Bluetooth(QObject* parent, bool observeDevices): QObject(parent), runtime(std::make_shared<Runtime>()), observeDevices(observeDevices) {
	runtime->delivery = std::make_shared<Delivery>(this);
	try { winrt::init_apartment(winrt::apartment_type::single_threaded); runtime->apartment = true; }
	catch (const winrt::hresult_error&) { /* Qt/tests may already have initialized an MTA. */ }
	runtime->retry.setInterval(15000);
	QObject::connect(&runtime->retry, &QTimer::timeout, this, [this] { watchRadios(); });
	watchRadios();
}
void Bluetooth::watchRadios() {
	auto delivery = runtime->delivery;
	const auto generation = ++runtime->radioGeneration;
	stop(runtime->radios); runtime->pending = 0; runtime->enumerated = false;
	try {
		runtime->radios = DeviceInformation::CreateWatcher(Radio::GetDeviceSelector());
		runtime->radios.Added([delivery, this, generation](auto const&, auto const& info) {
			delivery->post([this, info, delivery, generation] {
				if (generation != runtime->radioGeneration) return;
				const auto version = ++runtime->radioVersions[toQ(info.Id())];
				++runtime->pending;
				auto finish = [this, generation] { if (generation != runtime->radioGeneration) return; --runtime->pending; mReady = runtime->enumerated && runtime->pending == 0; emit changed(); };
				try {
					hardware::complete(Radio::FromIdAsync(info.Id()), [delivery, this, info, finish, version, generation](Radio radio) {
						delivery->post([this, info, radio, finish, version, generation] {
							if (generation != runtime->radioGeneration) return;
							bool exists = false;
							for (auto* adapter: mAdapters.valueList()) if (adapter->id() == toQ(info.Id())) exists = true;
							if (!exists && radio && radio.Kind() == RadioKind::Bluetooth && runtime->radioVersions.value(toQ(info.Id())) == version) {
								auto* adapter = new BluetoothAdapter(toQ(info.Id()), this);
								adapter->runtime->radio = radio; adapter->mName = toQ(radio.Name()); adapter->mState = radioState(radio);
								auto target = adapter->runtime->delivery;
								adapter->runtime->stateToken = radio.StateChanged([target, adapter](auto const& radio, auto const&) {
									auto state = radioState(radio);
									target->post([adapter, state] { adapter->mState = state; if (!adapter->enabled()) adapter->setDiscovering(false); emit adapter->changed(); });
								});
								QObject::connect(adapter, &BluetoothAdapter::discoveryRequested, this, [this](bool value) { watchDevices(value); });
								mAdapters.insertObject(adapter);
								if (this->observeDevices && mAdapters.valueList().size() == 1) watchDevices(false);
							}
							finish();
						});
					}, [delivery, finish](QString) { delivery->post(finish); });
				} catch (const winrt::hresult_error&) { finish(); }
			});
		});
		runtime->radios.Updated([delivery, this, generation](auto const&, auto const& info) {
			auto id = toQ(info.Id()); delivery->post([this, id, generation] {
				if (generation != runtime->radioGeneration) return;
				for (auto* adapter: mAdapters.valueList()) if (adapter->id() == id) {
					try { adapter->mName = toQ(adapter->runtime->radio.Name()); adapter->mState = radioState(adapter->runtime->radio); }
					catch (const winrt::hresult_error&) { adapter->mState = BluetoothAdapterState::Blocked; }
					if (!adapter->enabled()) adapter->setDiscovering(false);
					emit adapter->changed();
				}
			});
		});
		runtime->radios.Removed([delivery, this, generation](auto const&, auto const& info) {
			auto id = toQ(info.Id()); delivery->post([this, id, generation] { if (generation != runtime->radioGeneration) return; ++runtime->radioVersions[id]; removeAdapter(id); });
		});
		runtime->radios.EnumerationCompleted([delivery, this, generation](auto const&, auto const&) {
			delivery->post([this, generation] { if (generation != runtime->radioGeneration) return; runtime->enumerated = true; mReady = runtime->pending == 0; emit changed(); });
		});
		runtime->radios.Stopped([delivery, this, generation](auto const&, auto const&) {
			delivery->post([this, generation] { if (generation == runtime->radioGeneration) runtime->retry.start(); });
		});
		runtime->radios.Start(); runtime->retry.stop();
		// Replace fallback identities with the native watcher's IDs. Its callbacks
		// are queued, so they cannot race this reconciliation on the Qt thread.
		const auto previous = mAdapters.valueList();
		for (auto* adapter: previous) removeAdapter(adapter->id());
		error.clear(); mReady = false; emit changed();
	} catch (const winrt::hresult_error&) {
		runtime->retry.start();
		// Some desktop installations have no radio enumeration provider when the
		// adapter is absent. GetRadiosAsync is also a documented read-only query.
		try {
			hardware::complete(Radio::GetRadiosAsync(), [this, delivery, generation](auto radios) {
				delivery->post([this, radios, generation] {
					if (generation != runtime->radioGeneration) return;
					QStringList names, previous;
					for (auto const& radio: radios) if (radio.Kind() == RadioKind::Bluetooth) names.append(toQ(radio.Name()));
					for (auto* adapter: mAdapters.valueList()) previous.append(adapter->name());
					if (names == previous) { error.clear(); mReady = true; emit changed(); return; }
					const auto old = mAdapters.valueList();
					for (auto* adapter: old) removeAdapter(adapter->id());
					for (auto const& radio: radios) if (radio.Kind() == RadioKind::Bluetooth) {
						auto* adapter = new BluetoothAdapter(QStringLiteral("radio-%1").arg(mAdapters.valueList().size()), this);
						adapter->runtime->radio = radio; adapter->mName = toQ(radio.Name()); adapter->mState = radioState(radio);
						auto target = adapter->runtime->delivery;
						adapter->runtime->stateToken = radio.StateChanged([target, adapter](auto const& radio, auto const&) {
							auto state = radioState(radio); target->post([adapter, state] { adapter->mState = state; if (!adapter->enabled()) adapter->setDiscovering(false); emit adapter->changed(); });
						});
						QObject::connect(adapter, &BluetoothAdapter::discoveryRequested, this, [this](bool value) { watchDevices(value); });
						mAdapters.insertObject(adapter);
					}
					if (this->observeDevices && defaultAdapter()) watchDevices(false);
					error.clear(); mReady = true; emit changed();
				});
			}, [this, delivery, generation](QString message) { delivery->post([this, message, generation] { if (generation != runtime->radioGeneration) return; error = message; mReady = true; emit changed(); }); });
		} catch (const winrt::hresult_error& e) { error = toQ(e.message()); mReady = true; }
	}
}
Bluetooth::~Bluetooth() {
	runtime->delivery->close(); runtime->retry.stop(); stop(runtime->radios);
	for (auto const& watcher: runtime->paired) stop(watcher);
	for (auto const& watcher: runtime->discovery) stop(watcher);
	// Destroy adapters/devices and revoke their events before uninitializing COM.
	qDeleteAll(mDevices.valueList()); mDevices.valueList().clear();
	qDeleteAll(mAdapters.valueList()); mAdapters.valueList().clear();
	if (runtime->apartment) winrt::uninit_apartment();
}
void Bluetooth::removeDevice(const QString& id) {
	for (auto* device: mDevices.valueList()) if (device->id() == id) {
		device->adapter()->devices()->removeObject(device); mDevices.removeObject(device); delete device; break;
	}
}
void Bluetooth::removeAdapter(const QString& id) {
	for (auto* adapter: mAdapters.valueList()) if (adapter->id() == id) {
		adapter->setDiscovering(false);
		const auto devices = adapter->devices()->valueList();
		for (auto* device: devices) removeDevice(device->id());
		mAdapters.removeObject(adapter); delete adapter; emit changed();
		if (!defaultAdapter()) {
			++runtime->pairedGeneration;
			for (auto const& watcher: runtime->paired) stop(watcher); runtime->paired.clear();
		} else if (observeDevices) watchDevices(false);
		break;
	}
}
void Bluetooth::watchDevices(bool discovery) {
	if (!observeDevices) return;
	++runtime->scanGeneration;
	for (auto const& watcher: runtime->discovery) stop(watcher); runtime->discovery.clear();
	for (auto* device: mDevices.valueList()) device->runtime->sources.discovered.clear();
	if (!discovery) {
		const auto devices = mDevices.valueList();
		for (auto* device: devices) if (!device->paired() && !device->connected()) removeDevice(device->id());
	}
	if (!defaultAdapter()) return;
	auto delivery = runtime->delivery;
	auto make = [this, delivery](bool le, bool paired) {
		auto selector = le ? BT::BluetoothLEDevice::GetDeviceSelectorFromPairingState(paired) : BT::BluetoothDevice::GetDeviceSelectorFromPairingState(paired);
		auto properties = winrt::single_threaded_vector<winrt::hstring>({L"System.Devices.Aep.DeviceAddress", L"System.Devices.Aep.IsConnected"});
		auto watcher = DeviceInformation::CreateWatcher(selector, properties, DeviceInformationKind::AssociationEndpoint);
		const auto generation = paired ? runtime->pairedGeneration : runtime->scanGeneration;
		const auto source = ++runtime->watcherSequence;
		auto active = [this, paired, generation] { return generation == (paired ? runtime->pairedGeneration : runtime->scanGeneration); };
		auto apply = [this, delivery, le, paired, source, active](DeviceInformation const& info) {
			if (!active()) return;
			auto* adapter = defaultAdapter(); if (!adapter) return;
			auto id = toQ(info.Id()); BluetoothDevice* device = nullptr;
			for (auto* item: mDevices.valueList()) if (item->id() == id) { device = item; break; }
			bool fresh = !device;
			if (fresh) device = new BluetoothDevice(id, adapter);
			device->runtime->sources.add(source, paired);
			device->runtime->lowEnergy = le;
			device->runtime->info = info; device->mName = toQ(info.Name());
			device->mAddress = propertyText(info, L"System.Devices.Aep.DeviceAddress");
			if (device->mName.isEmpty()) device->mName = device->mAddress.isEmpty() ? QStringLiteral("Bluetooth") : device->mAddress;
			device->mPaired = !device->runtime->sources.paired.isEmpty() || info.Pairing().IsPaired(); device->mConnected = propertyBool(info, L"System.Devices.Aep.IsConnected");
			if (fresh) { mDevices.insertObject(device); adapter->devices()->insertObject(device); }
			emit device->changed();
			// Opening the projection only observes ConnectionStatus; no GATT read,
			// MaintainConnection, service discovery or connect attempt is performed.
			if (device->mPaired && !device->runtime->opened && !device->runtime->observeSuspended) {
				device->runtime->opened = true;
				const auto projection = ++device->runtime->projectionGeneration;
				auto target = device->runtime->delivery;
				auto opened = [target, device, projection](auto native) {
					target->post([target, device, native, projection] {
						if (projection != device->runtime->projectionGeneration) { if (native) native.Close(); return; }
						if (!native) return;
						using Native = std::decay_t<decltype(native)>;
						if constexpr (std::is_same_v<Native, BT::BluetoothDevice>) device->runtime->classic = native; else device->runtime->le = native;
						device->mConnected = native.ConnectionStatus() == BT::BluetoothConnectionStatus::Connected;
						device->runtime->connectionToken = native.ConnectionStatusChanged([target, device, projection](auto const& native, auto const&) {
							auto connected = native.ConnectionStatus() == BT::BluetoothConnectionStatus::Connected;
							target->post([device, connected, projection] {
								if (projection != device->runtime->projectionGeneration) return;
								device->mConnected = connected;
								if (connected) { device->mConnecting = false; device->runtime->connectionTimeout.stop(); }
								emit device->changed();
							});
						});
						emit device->changed();
					});
				};
				try {
					if (le) hardware::complete(BT::BluetoothLEDevice::FromIdAsync(info.Id()), opened);
					else hardware::complete(BT::BluetoothDevice::FromIdAsync(info.Id()), opened);
				} catch (const winrt::hresult_error&) {}
			}
		};
		watcher.Added([delivery, apply](auto const&, auto const& info) { delivery->post([apply, info] { apply(info); }); });
		watcher.Updated([delivery, this, apply, source, active](auto const&, auto const& update) { delivery->post([this, apply, update, source, active] {
			if (!active()) return;
			for (auto* device: mDevices.valueList()) if (device->id() == toQ(update.Id()) && device->runtime->sources.contains(source)) {
				device->runtime->info.Update(update); apply(device->runtime->info); break;
			}
		}); });
		watcher.Removed([delivery, this, source, active](auto const&, auto const& info) {
			auto id = toQ(info.Id()); delivery->post([this, id, source, active] {
				if (!active()) return;
				for (auto* device: mDevices.valueList()) if (device->id() == id) {
					device->runtime->sources.remove(source);
					if (!device->runtime->sources.empty()) {
						device->mPaired = !device->runtime->sources.paired.isEmpty(); emit device->changed();
					} else {
						// Allow the opposite query's Added event to arrive during pair/unpair.
						QTimer::singleShot(250, device, [this, id, device] {
							if (device->runtime->sources.empty() && !device->pairing()) removeDevice(id);
						});
					}
					break;
				}
			});
		});
		watcher.Stopped([delivery, this, paired, active](auto const&, auto const&) {
			delivery->post([this, paired, active] {
				if (!paired && active() && defaultAdapter()) defaultAdapter()->setDiscovering(false);
			});
		});
		watcher.Start(); return watcher;
	};
	try {
		if (runtime->paired.empty()) { runtime->paired.push_back(make(false, true)); runtime->paired.push_back(make(true, true)); }
		if (discovery) { runtime->discovery.push_back(make(false, false)); runtime->discovery.push_back(make(true, false)); }
	} catch (const winrt::hresult_error& e) {
		error = toQ(e.message()); emit changed();
		if (defaultAdapter()) defaultAdapter()->setDiscovering(false);
	}
}
}
