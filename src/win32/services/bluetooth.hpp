#pragma once
#include <memory>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtimer.h>
#include <qset.h>
#include "../../core/model.hpp"

namespace qs::win32::bluetooth {
// One endpoint can be present in several watchers during a pairing transition.
struct DeviceSources {
	QSet<quint64> paired, discovered;
	void add(quint64 source, bool isPaired) { (isPaired ? paired : discovered).insert(source); }
	void remove(quint64 source) { paired.remove(source); discovered.remove(source); }
	bool contains(quint64 source) const { return paired.contains(source) || discovered.contains(source); }
	bool empty() const { return paired.isEmpty() && discovered.isEmpty(); }
};
class BluetoothAdapterState: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { Disabled, Enabled, Enabling, Disabling, Blocked };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
class BluetoothDeviceState: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { Disconnected, Connected, Disconnecting, Connecting };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
class BluetoothAdapter;
class BluetoothDevice: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_UNCREATABLE("Devices are provided by Bluetooth")
	Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
	Q_PROPERTY(QString deviceName READ deviceName NOTIFY changed)
	Q_PROPERTY(QString address READ address NOTIFY changed)
	Q_PROPERTY(QString icon READ icon NOTIFY changed)
	Q_PROPERTY(bool connected READ connected WRITE setConnected NOTIFY changed)
	Q_PROPERTY(bool paired READ paired NOTIFY changed)
	Q_PROPERTY(bool bonded READ paired NOTIFY changed)
	Q_PROPERTY(bool trusted READ paired WRITE setTrusted NOTIFY changed)
	Q_PROPERTY(bool pairing READ pairing NOTIFY changed)
	Q_PROPERTY(bool blocked READ falseValue WRITE setBlocked NOTIFY changed)
	Q_PROPERTY(bool wakeAllowed READ falseValue WRITE setWakeAllowed NOTIFY changed)
	Q_PROPERTY(bool batteryAvailable READ falseValue CONSTANT)
	Q_PROPERTY(qreal battery READ battery CONSTANT)
	Q_PROPERTY(qs::win32::bluetooth::BluetoothDeviceState::Enum state READ state NOTIFY changed)
	Q_PROPERTY(qs::win32::bluetooth::BluetoothAdapter* adapter READ adapter NOTIFY changed)
	Q_PROPERTY(QString dbusPath READ id CONSTANT)
	Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
	struct Runtime;
	std::shared_ptr<Runtime> runtime;
	explicit BluetoothDevice(QString id, BluetoothAdapter* adapter);
	~BluetoothDevice() override;
	QString id() const { return mId; }
	QString name() const { return alias.isEmpty() ? mName : alias; }
	void setName(const QString& value) { alias = value; emit changed(); }
	QString deviceName() const { return mName; }
	QString address() const { return mAddress; }
	QString icon() const { return mIcon; }
	bool connected() const { return mConnected; }
	void setConnected(bool value) { if (value) connect(); else disconnect(); }
	bool paired() const { return mPaired; }
	bool pairing() const { return mPairing; }
	bool falseValue() const { return false; }
	void setTrusted(bool value);
	void setBlocked(bool value);
	void setWakeAllowed(bool value);
	qreal battery() const { return 0; }
	BluetoothDeviceState::Enum state() const { return mConnected ? BluetoothDeviceState::Connected : mConnecting ? BluetoothDeviceState::Connecting : BluetoothDeviceState::Disconnected; }
	BluetoothAdapter* adapter() const { return mAdapter; }
	QString lastError() const { return error; }
	Q_INVOKABLE void connect();
	Q_INVOKABLE void disconnect();
	Q_INVOKABLE void pair();
	Q_INVOKABLE void cancelPair();
	Q_INVOKABLE void forget();
signals:
	void changed();
private:
	friend class Bluetooth;
	QString mId, mName, mAddress, alias, mIcon = "bluetooth", error;
	BluetoothAdapter* mAdapter;
	bool mConnected = false, mPaired = false, mPairing = false, mConnecting = false;
};
class BluetoothAdapter: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_UNCREATABLE("Adapters are provided by Bluetooth")
	Q_PROPERTY(QString name READ name NOTIFY changed)
	Q_PROPERTY(QString adapterId READ id CONSTANT)
	Q_PROPERTY(QString dbusPath READ id CONSTANT)
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
	Q_PROPERTY(qs::win32::bluetooth::BluetoothAdapterState::Enum state READ state NOTIFY changed)
	Q_PROPERTY(bool discovering READ discovering WRITE setDiscovering NOTIFY changed)
	Q_PROPERTY(bool discoverable READ falseValue WRITE setDiscoverable NOTIFY changed)
	Q_PROPERTY(quint32 discoverableTimeout READ zero WRITE setDiscoverableTimeout NOTIFY changed)
	Q_PROPERTY(bool pairable READ enabled WRITE setPairable NOTIFY changed)
	Q_PROPERTY(quint32 pairableTimeout READ zero WRITE setPairableTimeout NOTIFY changed)
	Q_PROPERTY(UntypedObjectModel* devices READ devices CONSTANT)
	Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
	struct Runtime;
	std::shared_ptr<Runtime> runtime;
	explicit BluetoothAdapter(QString id, QObject* parent);
	~BluetoothAdapter() override;
	QString id() const { return mId; }
	QString name() const { return mName; }
	bool enabled() const { return mState == BluetoothAdapterState::Enabled; }
	BluetoothAdapterState::Enum state() const { return mState; }
	void setEnabled(bool value);
	bool discovering() const { return mDiscovering; }
	void setDiscovering(bool value);
	bool falseValue() const { return false; }
	quint32 zero() const { return 0; }
	void setDiscoverable(bool value);
	void setDiscoverableTimeout(quint32 value);
	void setPairable(bool value);
	void setPairableTimeout(quint32 value);
	ObjectModel<BluetoothDevice>* devices() { return &mDevices; }
	QString lastError() const { return error; }
signals:
	void changed();
	void discoveryRequested(bool enabled);
private:
	friend class Bluetooth;
	QString mId, mName, error;
	BluetoothAdapterState::Enum mState = BluetoothAdapterState::Disabled;
	bool mDiscovering = false;
	QTimer discoveryTimeout;
	ObjectModel<BluetoothDevice> mDevices {this};
};
class Bluetooth: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(qs::win32::bluetooth::BluetoothAdapter* defaultAdapter READ defaultAdapter NOTIFY changed)
	Q_PROPERTY(UntypedObjectModel* adapters READ adapters CONSTANT)
	Q_PROPERTY(UntypedObjectModel* devices READ devices CONSTANT)
	Q_PROPERTY(bool ready READ ready NOTIFY changed)
	Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
	explicit Bluetooth(QObject* parent = nullptr, bool observeDevices = true);
	~Bluetooth() override;
	BluetoothAdapter* defaultAdapter() const { return mAdapters.valueList().isEmpty() ? nullptr : mAdapters.valueList().first(); }
	ObjectModel<BluetoothAdapter>* adapters() { return &mAdapters; }
	ObjectModel<BluetoothDevice>* devices() { return &mDevices; }
	bool ready() const { return mReady; }
	QString lastError() const { return error; }
signals:
	void changed();
private:
	struct Runtime;
	std::shared_ptr<Runtime> runtime;
	ObjectModel<BluetoothAdapter> mAdapters {this};
	ObjectModel<BluetoothDevice> mDevices {this};
	bool mReady = false, observeDevices;
	QString error;
	void watchRadios();
	void watchDevices(bool discovery);
	void removeAdapter(const QString& id);
	void removeDevice(const QString& id);
};
}
