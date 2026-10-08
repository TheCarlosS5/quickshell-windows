#pragma once

#include <memory>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qvariant.h>
#include "../../core/model.hpp"

namespace qs::win32::power {
class UPowerDeviceState: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { Unknown, Charging, Discharging, Empty, FullyCharged, PendingCharge, PendingDischarge };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
class UPowerDeviceType: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { Unknown, LinePower, Battery, Ups, Monitor, Mouse, Keyboard, Pda, Phone,
		MediaPlayer, Tablet, Computer, GamingInput, Pen, Touchpad, Modem, Network, Headset,
		Speakers, Headphones, Video, OtherAudio, RemoteControl, Printer, Scanner, Camera,
		Wearable, Toy, BluetoothGeneric };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
struct BatterySnapshot {
	bool present = false, ready = false, onBattery = false, healthSupported = false;
	qreal percentage = 0, energy = 0, capacity = 0, rate = 0, health = 0, empty = 0, full = 0;
	UPowerDeviceState::Enum state = UPowerDeviceState::Unknown;
	QString path, model;
};
// Pure normalization also used by the console tests; all energies/rates are in mWh/mW.
BatterySnapshot normalizeBattery(bool present, int percent, int remaining, int capacity,
	int design, int rate, UPowerDeviceState::Enum state, qreal timeToEmpty = 0);
class UPowerDevice: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_UNCREATABLE("Devices are provided by UPower")
	Q_PROPERTY(qs::win32::power::UPowerDeviceType::Enum type READ type NOTIFY changed)
	Q_PROPERTY(bool powerSupply READ isLaptopBattery NOTIFY changed)
	Q_PROPERTY(bool isLaptopBattery READ isLaptopBattery NOTIFY changed)
	Q_PROPERTY(bool isPresent READ isLaptopBattery NOTIFY changed)
	Q_PROPERTY(bool ready READ ready NOTIFY changed)
	Q_PROPERTY(qreal percentage READ percentage NOTIFY changed)
	Q_PROPERTY(qs::win32::power::UPowerDeviceState::Enum state READ state NOTIFY changed)
	Q_PROPERTY(qreal energy READ energy NOTIFY changed)
	Q_PROPERTY(qreal energyCapacity READ energyCapacity NOTIFY changed)
	Q_PROPERTY(qreal changeRate READ changeRate NOTIFY changed)
	Q_PROPERTY(qreal healthPercentage READ healthPercentage NOTIFY changed)
	Q_PROPERTY(bool healthSupported READ healthSupported NOTIFY changed)
	Q_PROPERTY(qreal timeToEmpty READ timeToEmpty NOTIFY changed)
	Q_PROPERTY(qreal timeToFull READ timeToFull NOTIFY changed)
	Q_PROPERTY(QString iconName READ iconName NOTIFY changed)
	Q_PROPERTY(QString nativePath READ nativePath NOTIFY changed)
	Q_PROPERTY(QString model READ model NOTIFY changed)
public:
	using QObject::QObject;
	BatterySnapshot snapshot;
	void apply(BatterySnapshot value) { snapshot = std::move(value); emit changed(); }
	UPowerDeviceType::Enum type() const { return snapshot.present ? UPowerDeviceType::Battery : UPowerDeviceType::Unknown; }
	bool isLaptopBattery() const { return snapshot.present; }
	bool ready() const { return snapshot.ready; }
	qreal percentage() const { return snapshot.percentage; }
	UPowerDeviceState::Enum state() const { return snapshot.state; }
	qreal energy() const { return snapshot.energy; }
	qreal energyCapacity() const { return snapshot.capacity; }
	qreal changeRate() const { return snapshot.rate; }
	qreal healthPercentage() const { return snapshot.health; }
	bool healthSupported() const { return snapshot.healthSupported; }
	qreal timeToEmpty() const { return snapshot.empty; }
	qreal timeToFull() const { return snapshot.full; }
	QString iconName() const;
	QString nativePath() const { return snapshot.path; }
	QString model() const { return snapshot.model; }
signals:
	void changed();
};
class UPower: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(qs::win32::power::UPowerDevice* displayDevice READ displayDevice CONSTANT)
	Q_PROPERTY(UntypedObjectModel* devices READ devices CONSTANT)
	Q_PROPERTY(bool onBattery READ onBattery NOTIFY onBatteryChanged)
public:
	explicit UPower(QObject* parent = nullptr);
	~UPower() override;
	UPowerDevice* displayDevice() { return &display; }
	ObjectModel<UPowerDevice>* devices() { return &model; }
	bool onBattery() const { return display.snapshot.onBattery; }
signals:
	void onBatteryChanged();
private:
	struct Runtime;
	std::shared_ptr<Runtime> runtime;
	UPowerDevice display {this};
	ObjectModel<UPowerDevice> model {this};
	void update(const QString& id, const BatterySnapshot& snapshot);
};
class PowerProfile: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { PowerSaver, Balanced, Performance };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
class PerformanceDegradationReason: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
public:
	enum Enum { None, LapDetected, HighTemperature };
	Q_ENUM(Enum)
	Q_INVOKABLE static QString toString(Enum value);
};
class PowerProfiles: public QObject {
	Q_OBJECT
	QML_ELEMENT
	QML_SINGLETON
	Q_PROPERTY(qs::win32::power::PowerProfile::Enum profile READ profile WRITE setProfile NOTIFY changed)
	Q_PROPERTY(bool hasPerformanceProfile READ hasPerformanceProfile NOTIFY changed)
	Q_PROPERTY(qs::win32::power::PerformanceDegradationReason::Enum degradationReason READ degradationReason CONSTANT)
	Q_PROPERTY(QVariantList holds READ holds CONSTANT)
	Q_PROPERTY(bool available READ available NOTIFY changed)
	Q_PROPERTY(bool customProfile READ customProfile NOTIFY changed)
public:
	explicit PowerProfiles(QObject* parent = nullptr);
	~PowerProfiles() override;
	PowerProfile::Enum profile() const { return current; }
	void setProfile(PowerProfile::Enum profile);
	bool hasPerformanceProfile() const { return performance; }
	bool available() const { return valid; }
	bool customProfile() const { return custom; }
	PerformanceDegradationReason::Enum degradationReason() const { return PerformanceDegradationReason::None; }
	QVariantList holds() const { return {}; }
signals:
	void changed();
private:
	void refresh();
	struct Runtime;
	std::shared_ptr<Runtime> runtime;
	PowerProfile::Enum current = PowerProfile::Balanced;
	bool performance = false, valid = false, custom = false;
};
}
