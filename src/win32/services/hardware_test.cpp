// QCoreApplication only: no QGuiApplication, QML window, consent, setters or scans.
#include "battery.hpp"
#include "bluetooth.hpp"
#include <qcoreapplication.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtimer.h>
#include <qqml.h>
#include <qqmlengine.h>
#include <qqmlcomponent.h>
#include <cstdio>
#include <cmath>
#include <windows.h>
#include <winrt/base.h>

int main(int argc, char** argv) {
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

	QCoreApplication app(argc, argv);
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
	if (app.arguments() != QStringList {app.arguments().first(), "--read-only"}) return 2;
	using namespace qs::win32::power;
	int failures = 0;
	auto check = [&](bool condition) { if (!condition) ++failures; };
	auto absent = normalizeBattery(false, 100, 50000, 50000, 55000, 0, UPowerDeviceState::FullyCharged);
	check(!absent.present && absent.ready && absent.energy == 0 && absent.percentage == 0);
	auto discharge = normalizeBattery(true, 50, 25000, 50000, 62500, -10000, UPowerDeviceState::Discharging);
	check(discharge.percentage == .5 && discharge.energy == 25 && discharge.capacity == 50 && discharge.rate == -10 && discharge.health == .8 && discharge.empty == 9000 && discharge.full == 0);
	auto charging = normalizeBattery(true, 50, 25000, 50000, 62500, 10000, UPowerDeviceState::Charging);
	check(charging.full == 9000 && charging.empty == 0 && !charging.onBattery);
	auto unknown = normalizeBattery(true, -1, -1, -1, -1, 0, UPowerDeviceState::Unknown);
	check(unknown.percentage == 1 && !unknown.healthSupported && std::isfinite(unknown.percentage));
	auto full = normalizeBattery(true, 100, 50000, 50000, 55000, 0, UPowerDeviceState::PendingCharge);
	check(full.state == UPowerDeviceState::FullyCharged);
	auto empty = normalizeBattery(true, 0, 0, 50000, 55000, -10000, UPowerDeviceState::Discharging);
	check(empty.state == UPowerDeviceState::Empty && empty.onBattery);
	auto clamped = normalizeBattery(true, 255, 60000, 50000, 40000, 0, UPowerDeviceState::Unknown);
	check(clamped.percentage == 1 && clamped.health == 1);
	qs::win32::bluetooth::DeviceSources membership;
	membership.add(1, false); membership.add(2, true); membership.remove(1);
	check(!membership.empty() && membership.paired.contains(2)); // paired Added before discovered Removed
	membership.add(3, false); membership.remove(2);
	check(!membership.empty() && membership.discovered.contains(3)); // unpair in the opposite order
	membership.add(4, true); membership.discovered.clear();
	check(!membership.empty() && membership.paired.contains(4)); // stop discovery retains paired endpoints
	membership.remove(4); check(membership.empty());
	if (failures) { std::fprintf(stderr, "hardware state failures=%d\n", failures); return 1; }
	UPower battery;
	qs::win32::bluetooth::Bluetooth bluetooth(nullptr, false);
	UPowerDeviceState stateEnums;
	PowerProfile profileEnums;
	PowerProfiles profiles;
	// Import the public API into a QtObject only. No QtQuick/Window is imported,
	// and the singleton instance disables device enumeration/consent/discovery.
	qmlRegisterUncreatableType<UntypedObjectModel>("HardwareTest", 1, 0, "ObjectModel", "Provided by services");
	qmlRegisterUncreatableType<UPowerDevice>("Quickshell.Services.UPower", 0, 1, "UPowerDevice", "Provided by UPower");
	qmlRegisterSingletonInstance("Quickshell.Services.UPower", 0, 1, "UPower", &battery);
	qmlRegisterSingletonInstance("Quickshell.Services.UPower", 0, 1, "UPowerDeviceState", &stateEnums);
	qmlRegisterSingletonInstance("Quickshell.Services.UPower", 0, 1, "PowerProfile", &profileEnums);
	qmlRegisterSingletonInstance("Quickshell.Services.UPower", 0, 1, "PowerProfiles", &profiles);
	qmlRegisterUncreatableType<qs::win32::bluetooth::BluetoothAdapter>("Quickshell.Bluetooth", 0, 1, "BluetoothAdapter", "Provided by Bluetooth");
	qmlRegisterUncreatableType<qs::win32::bluetooth::BluetoothDevice>("Quickshell.Bluetooth", 0, 1, "BluetoothDevice", "Provided by Bluetooth");
	qmlRegisterSingletonInstance("Quickshell.Bluetooth", 0, 1, "Bluetooth", &bluetooth);
	QQmlEngine engine;
	QQmlComponent component(&engine);
	component.setData(R"(
import QtQml
import Quickshell.Services.UPower
import Quickshell.Bluetooth
QtObject {
    property bool batteryVisible: UPower.displayDevice.isLaptopBattery
    property real percentage: UPower.displayDevice.percentage
    property int batteries: UPower.devices.values.length
    property int adapters: Bluetooth.adapters.values.length
    property BluetoothAdapter adapter: Bluetooth.defaultAdapter
    property bool onBattery: UPower.onBattery
    property bool performance: PowerProfiles.hasPerformanceProfile
    property string stateName: UPowerDeviceState.toString(UPowerDeviceState.Charging)
    property int balanced: PowerProfile.Balanced
}
)", QUrl());
	std::unique_ptr<QObject> qml(component.create());
	if (!qml || !component.errors().isEmpty() || qml->property("stateName").toString() != "Charging" || qml->property("balanced").toInt() != 1) {
		for (const auto& error: component.errors()) std::fprintf(stderr, "%s\n", error.toString().toUtf8().constData());
		return 1;
	}
	QTimer timeout; timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &app, [&] { std::fputs("hardware query timed out\n", stderr); app.exit(1); });
	timeout.start(10000);
	QTimer done;
	QObject::connect(&done, &QTimer::timeout, &app, [&] {
		if (!battery.displayDevice()->ready() || !bluetooth.ready()) return;
		auto* device = battery.displayDevice();
		QJsonObject power {{"available", device->isLaptopBattery()}, {"isLaptopBattery", device->isLaptopBattery()},
			{"onBattery", battery.onBattery()}, {"percentage", device->percentage()}, {"ready", device->ready()},
			{"state", UPowerDeviceState::toString(device->state())}};
		QJsonObject radios {{"adapters", int(bluetooth.adapters()->valueList().size())},
			{"defaultAdapter", bluetooth.defaultAdapter() ? QJsonValue(bluetooth.defaultAdapter()->name()) : QJsonValue(QJsonValue::Null)},
			{"ready", bluetooth.ready()}, {"error", bluetooth.lastError()}};
		auto json = QJsonDocument(QJsonObject {{"battery", power}, {"bluetooth", radios}, {"bluetoothMembershipChecks", 4}, {"normalizationChecks", 7}, {"qmlApi", true}}).toJson(QJsonDocument::Compact);
		std::printf("%s\n", json.constData()); app.exit(bluetooth.lastError().isEmpty() ? 0 : 1);
	});
	done.start(50);
	return app.exec();
}
