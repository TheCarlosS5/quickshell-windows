#include "battery.hpp"
#include "hardware_async.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <qmetaobject.h>
#include <windows.h>
#include <powrprof.h>
#include <powersetting.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.System.Power.h>

namespace qs::win32::power {
using namespace winrt::Windows::Devices::Power;
using namespace winrt::Windows::Devices::Enumeration;
using winrt::Windows::System::Power::BatteryStatus;
using hardware::Delivery;
namespace {
template <typename T> QString enumName(int value) {
	auto e = T::staticMetaObject.enumerator(T::staticMetaObject.indexOfEnumerator("Enum"));
	auto name = e.valueToKey(value);
	return name ? QString::fromLatin1(name) : QStringLiteral("Unknown");
}
QString toQ(const winrt::hstring& text) { return QString::fromWCharArray(text.c_str()); }
int number(winrt::Windows::Foundation::IReference<int> const& value) { return value ? value.Value() : -1; }
class PowerEvents {
	DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters {};
	std::vector<HPOWERNOTIFY> handles;
	std::function<void()> function;
	static ULONG CALLBACK callback(PVOID context, ULONG, PVOID) {
		static_cast<PowerEvents*>(context)->function(); return ERROR_SUCCESS;
	}
public:
	PowerEvents(std::function<void()> f, std::initializer_list<const GUID*> guids): function(std::move(f)) {
		parameters.Callback = callback; parameters.Context = this;
		for (auto guid: guids) {
			HPOWERNOTIFY handle = nullptr;
			if (!PowerSettingRegisterNotification(guid, DEVICE_NOTIFY_CALLBACK, &parameters, &handle)) handles.push_back(handle);
		}
	}
	~PowerEvents() { for (auto handle: handles) PowerSettingUnregisterNotification(handle); }
};
BatterySnapshot report(Battery const& battery, bool aggregate) {
	SYSTEM_POWER_STATUS status {};
	const bool systemKnown = GetSystemPowerStatus(&status) && status.BatteryFlag != 255;
	BatterySnapshot result;
	result.ready = true;
	try {
		if (!battery) throw winrt::hresult_error(E_NOTIMPL);
		auto r = battery.GetReport();
		bool present = r.Status() != BatteryStatus::NotPresent;
		if (aggregate && systemKnown) present = !(status.BatteryFlag & 128);
		auto state = UPowerDeviceState::Unknown;
		if (r.Status() == BatteryStatus::Charging) state = UPowerDeviceState::Charging;
		else if (r.Status() == BatteryStatus::Discharging) state = UPowerDeviceState::Discharging;
		else if (r.Status() == BatteryStatus::Idle) state = UPowerDeviceState::PendingCharge;
		result = normalizeBattery(present, aggregate && systemKnown && status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1,
			number(r.RemainingCapacityInMilliwattHours()), number(r.FullChargeCapacityInMilliwattHours()),
			number(r.DesignCapacityInMilliwattHours()), r.ChargeRateInMilliwatts() ? r.ChargeRateInMilliwatts().Value() : 0, state,
			aggregate && systemKnown && status.BatteryLifeTime != DWORD(-1) ? status.BatteryLifeTime : 0);
	} catch (const winrt::hresult_error&) {
		if (aggregate && systemKnown) {
			bool present = !(status.BatteryFlag & 128);
			auto state = status.BatteryFlag & 8 ? UPowerDeviceState::Charging
				: status.ACLineStatus == 0 ? UPowerDeviceState::Discharging : UPowerDeviceState::PendingCharge;
			result = normalizeBattery(present, status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1,
				-1, -1, -1, 0, state, status.BatteryLifeTime == DWORD(-1) ? 0 : status.BatteryLifeTime);
		}
	}
	if (aggregate && systemKnown && status.ACLineStatus != 255) result.onBattery = result.present && status.ACLineStatus == 0;
	return result;
}
const GUID* scheme(PowerProfile::Enum value) {
	switch (value) {
	case PowerProfile::PowerSaver: return &GUID_MAX_POWER_SAVINGS;
	case PowerProfile::Performance: return &GUID_MIN_POWER_SAVINGS;
	default: return &GUID_TYPICAL_POWER_SAVINGS;
	}
}
bool schemeExists(const GUID& wanted) {
	for (DWORD index = 0;; ++index) {
		GUID value {}; DWORD size = sizeof(value);
		if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, reinterpret_cast<BYTE*>(&value), &size)) return false;
		if (value == wanted) return true;
	}
}
}
QString UPowerDeviceState::toString(Enum value) { return enumName<UPowerDeviceState>(value); }
QString UPowerDeviceType::toString(Enum value) { return enumName<UPowerDeviceType>(value); }
QString PowerProfile::toString(Enum value) { return enumName<PowerProfile>(value); }
QString PerformanceDegradationReason::toString(Enum value) { return enumName<PerformanceDegradationReason>(value); }
BatterySnapshot normalizeBattery(bool present, int percent, int remaining, int capacity, int design,
	int rate, UPowerDeviceState::Enum state, qreal timeToEmpty) {
	BatterySnapshot value;
	value.ready = true; value.present = present;
	if (!present) return value;
	value.energy = std::max(remaining, 0) / 1000.0;
	value.capacity = std::max(capacity, 0) / 1000.0;
	// An unknown percentage must never trigger ii's automatic critical-battery suspend.
	value.percentage = remaining >= 0 && capacity > 0 ? std::clamp(qreal(remaining) / capacity, 0.0, 1.0)
		: percent >= 0 && percent <= 100 ? percent / 100.0 : 1.0;
	value.healthSupported = design > 0 && capacity > 0;
	value.health = value.healthSupported ? std::clamp(qreal(capacity) / design, 0.0, 1.0) : 0;
	value.rate = rate / 1000.0;
	value.state = state;
	const bool knownPercentage = (remaining >= 0 && capacity > 0) || (percent >= 0 && percent <= 100);
	if (knownPercentage && value.percentage == 0 && state == UPowerDeviceState::Discharging) value.state = UPowerDeviceState::Empty;
	if (knownPercentage && value.percentage == 1 && state == UPowerDeviceState::PendingCharge) value.state = UPowerDeviceState::FullyCharged;
	value.onBattery = state == UPowerDeviceState::Discharging;
	if (value.onBattery) value.empty = timeToEmpty > 0 ? timeToEmpty : rate < 0 && remaining >= 0 ? qreal(remaining) / -qreal(rate) * 3600 : 0;
	if (state == UPowerDeviceState::Charging && rate > 0 && capacity >= 0 && remaining >= 0)
		value.full = std::max(capacity - remaining, 0) / qreal(rate) * 3600;
	return value;
}
QString UPowerDevice::iconName() const {
	if (!snapshot.present) return {};
	auto level = snapshot.percentage >= .95 ? "full" : snapshot.percentage >= .6 ? "good" : snapshot.percentage >= .3 ? "low" : "caution";
	return QStringLiteral("battery-") + level + (snapshot.state == UPowerDeviceState::Charging ? "-charging-symbolic" : "-symbolic");
}
struct UPower::Runtime: std::enable_shared_from_this<Runtime> {
	struct Entry { Battery battery {nullptr}; winrt::event_token token {}; };
	UPower* owner;
	std::shared_ptr<Delivery> delivery;
	std::atomic_bool stopped = false;
	std::mutex mutex;
	std::mutex lifecycle;
	Battery aggregate {nullptr}; winrt::event_token aggregateToken {};
	DeviceWatcher watcher {nullptr};
	QHash<QString, Entry> entries;
	QHash<QString, quint64> versions;
	std::unique_ptr<PowerEvents> events;
	explicit Runtime(UPower* owner): owner(owner), delivery(std::make_shared<Delivery>(owner)) {}
	void deliver(QString id, BatterySnapshot snapshot, quint64 version = 0) {
		auto weak = weak_from_this();
		delivery->post([weak, id, snapshot, version] {
			if (auto self = weak.lock(); self && !self->stopped) {
				if (!id.isEmpty()) {
					std::scoped_lock lock(self->mutex);
					if (self->versions.value(id) != version) return;
				}
				self->owner->update(id, snapshot);
			}
		});
	}
	void publish(Battery const& battery, QString id, quint64 version = 0) {
		auto snapshot = report(battery, id.isEmpty());
		snapshot.path = id.isEmpty() ? QStringLiteral("Windows.AggregateBattery") : id;
		deliver(id, snapshot, version);
	}
	void add(DeviceInformation const& information) {
		const auto id = toQ(information.Id());
		quint64 version;
		{ std::scoped_lock lock(mutex); if (entries.contains(id)) return; version = ++versions[id]; }
		auto weak = weak_from_this();
		hardware::complete(Battery::FromIdAsync(information.Id()), [weak, id, version, name = toQ(information.Name())](Battery battery) {
			if (auto self = weak.lock(); self && !self->stopped && battery) {
				std::scoped_lock lock(self->mutex);
				if (self->stopped || self->versions.value(id) != version || self->entries.contains(id)) return;
				auto token = battery.ReportUpdated([weak, id, version](auto const& battery, auto const&) {
					if (auto self = weak.lock(); self && !self->stopped) self->publish(battery, id, version);
				});
				self->entries.insert(id, {battery, token});
				auto snapshot = report(battery, false); snapshot.path = id; snapshot.model = name;
				self->deliver(id, snapshot, version);
			}
		});
	}
	void start() {
		std::scoped_lock lock(lifecycle);
		if (stopped) return;
		auto weak = weak_from_this();
		try {
			aggregate = Battery::AggregateBattery();
			aggregateToken = aggregate.ReportUpdated([weak](auto const& battery, auto const&) {
				if (auto self = weak.lock(); self && !self->stopped) self->publish(battery, {});
			});
		} catch (const winrt::hresult_error&) { /* GetSystemPowerStatus fallback. */ }
		publish(aggregate, {});
		events = std::make_unique<PowerEvents>([weak] {
			if (auto self = weak.lock(); self && !self->stopped) self->publish(self->aggregate, {});
		}, std::initializer_list<const GUID*> {&GUID_ACDC_POWER_SOURCE, &GUID_BATTERY_PERCENTAGE_REMAINING});
		try {
			auto selector = Battery::GetDeviceSelector();
			watcher = DeviceInformation::CreateWatcher(selector);
			watcher.Added([weak](auto const&, auto const& info) { if (auto self = weak.lock(); self && !self->stopped) self->add(info); });
			watcher.Updated([weak](auto const&, auto const& info) {
				if (auto self = weak.lock(); self && !self->stopped) {
					const auto id = toQ(info.Id()); Entry entry; quint64 version;
					{ std::scoped_lock lock(self->mutex); entry = self->entries.value(id); version = self->versions.value(id); }
					if (entry.battery) self->publish(entry.battery, id, version);
				}
			});
			watcher.Removed([weak](auto const&, auto const& info) {
				if (auto self = weak.lock(); self && !self->stopped) {
					auto id = toQ(info.Id()); Entry old; quint64 version;
					{ std::scoped_lock lock(self->mutex); version = ++self->versions[id]; old = self->entries.take(id); }
					if (old.battery) old.battery.ReportUpdated(old.token);
					BatterySnapshot empty; empty.ready = true; self->deliver(id, empty, version);
				}
			});
			watcher.Start();
		} catch (const winrt::hresult_error&) {}
	}
	void stop() {
		stopped = true; delivery->close();
		std::scoped_lock lifeLock(lifecycle);
		events.reset();
		try {
			if (watcher && (watcher.Status() == DeviceWatcherStatus::Started || watcher.Status() == DeviceWatcherStatus::EnumerationCompleted)) watcher.Stop();
			if (aggregate) aggregate.ReportUpdated(aggregateToken);
			std::scoped_lock lock(mutex);
			for (auto const& entry: entries) entry.battery.ReportUpdated(entry.token);
			entries.clear();
		} catch (const winrt::hresult_error&) {}
	}
};
UPower::UPower(QObject* parent): QObject(parent), runtime(std::make_shared<Runtime>(this)) {
	// No blocking enumeration or WinRT waits on Qt's GUI thread.
	std::thread([state = runtime] { winrt::init_apartment(); if (!state->stopped) state->start(); }).detach();
}
UPower::~UPower() { runtime->stop(); }
void UPower::update(const QString& id, const BatterySnapshot& snapshot) {
	if (id.isEmpty()) { display.apply(snapshot); emit onBatteryChanged(); return; }
	auto it = std::find_if(model.valueList().begin(), model.valueList().end(), [&](auto* device) { return device->nativePath() == id; });
	if (!snapshot.present) {
		if (it != model.valueList().end()) { auto* device = *it; model.removeObject(device); device->deleteLater(); }
	} else {
		auto* device = it == model.valueList().end() ? new UPowerDevice(this) : *it;
		auto value = snapshot;
		if (value.model.isEmpty()) value.model = device->model();
		device->apply(value);
		if (it == model.valueList().end()) model.insertObject(device);
	}
}
struct PowerProfiles::Runtime {
	std::shared_ptr<Delivery> delivery;
	std::unique_ptr<PowerEvents> events;
};
PowerProfiles::PowerProfiles(QObject* parent): QObject(parent), runtime(std::make_shared<Runtime>()) {
	runtime->delivery = std::make_shared<Delivery>(this);
	auto weak = std::weak_ptr<Delivery>(runtime->delivery);
	runtime->events = std::make_unique<PowerEvents>([weak, this] { if (auto target = weak.lock()) target->post([this] { refresh(); }); },
		std::initializer_list<const GUID*> {&GUID_POWERSCHEME_PERSONALITY, &GUID_ACTIVE_POWERSCHEME});
	refresh();
}
PowerProfiles::~PowerProfiles() { runtime->delivery->close(); runtime->events.reset(); }
void PowerProfiles::refresh() {
	GUID* active = nullptr;
	valid = !PowerGetActiveScheme(nullptr, &active) && active;
	custom = valid && *active != GUID_MAX_POWER_SAVINGS && *active != GUID_MIN_POWER_SAVINGS && *active != GUID_TYPICAL_POWER_SAVINGS;
	current = valid && *active == GUID_MAX_POWER_SAVINGS ? PowerProfile::PowerSaver : valid && *active == GUID_MIN_POWER_SAVINGS ? PowerProfile::Performance : PowerProfile::Balanced;
	LocalFree(active); performance = schemeExists(GUID_MIN_POWER_SAVINGS); emit changed();
}
void PowerProfiles::setProfile(PowerProfile::Enum value) {
	if (value < PowerProfile::PowerSaver || value > PowerProfile::Performance) return;
	const auto* guid = scheme(value);
	if (schemeExists(*guid)) PowerSetActiveScheme(nullptr, guid);
	refresh();
}
}
