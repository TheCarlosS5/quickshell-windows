#include "network.hpp"
#include <algorithm>

#include <qdesktopservices.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qurl.h>

#include <windows.h>
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wlanapi.h>
#include <wrl/client.h>
// clang-format on

#include "../../core/logcat.hpp"

namespace qs::win32::network {

QS_LOGGING_CATEGORY(logNetwork, "quickshell.win32.network", QtInfoMsg);

using Microsoft::WRL::ComPtr;

namespace {

constexpr int POLL_MS = 5000;
constexpr int CONNECT_TIMEOUT_MS = 30000;

QString ssidString(const DOT11_SSID& ssid) {
	return QString::fromUtf8(reinterpret_cast<const char*>(ssid.ucSSID), static_cast<qsizetype>(std::min<ULONG>(ssid.uSSIDLength, DOT11_SSID_MAX_LENGTH))); // NOLINT
}

QString securityName(DOT11_AUTH_ALGORITHM auth, bool secured) {
	if (!secured) return {};
	switch (auth) {
	case DOT11_AUTH_ALGO_WPA3_SAE: return "WPA3";
	case DOT11_AUTH_ALGO_RSNA_PSK: return "WPA2";
	case DOT11_AUTH_ALGO_WPA_PSK: return "WPA1";
	case DOT11_AUTH_ALGO_RSNA:
	case DOT11_AUTH_ALGO_WPA:
	case DOT11_AUTH_ALGO_WPA3_ENT_192:
	case DOT11_AUTH_ALGO_WPA3_ENT: return "802.1X";
	case DOT11_AUTH_ALGO_OWE: return "OWE";
	default: return "WEP";
	}
}

QString bssidString(const DOT11_MAC_ADDRESS& mac) {
	QStringList parts;
	for (auto byte: mac) parts.append(QString("%1").arg(byte, 2, 16, QChar('0')).toUpper());
	return parts.join(':');
}

void WINAPI wlanCallback(PWLAN_NOTIFICATION_DATA data, PVOID context) {
	auto* network = static_cast<Network*>(context);
	if (!data || !network) return;

	QString ssid;
	quint32 reason = 0;
	if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM
	    && (data->NotificationCode == wlan_notification_acm_connection_complete
	        || data->NotificationCode == wlan_notification_acm_connection_attempt_fail)
	    && data->pData && data->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
	{
		auto* connection = static_cast<WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
		ssid = ssidString(connection->dot11Ssid);
		reason = connection->wlanReasonCode;
	}

	auto source = static_cast<quint32>(data->NotificationSource);
	auto code = static_cast<quint32>(data->NotificationCode);
	QMetaObject::invokeMethod(
	    network,
	    [network, source, code, ssid, reason]() { network->onWlanNotification(source, code, ssid, reason); },
	    Qt::QueuedConnection
	);
}

} // namespace

Network::Network(QObject* parent): QObject(parent) {
	this->openWlan();

	QObject::connect(&this->poll, &QTimer::timeout, this, &Network::refresh);
	this->poll.start(POLL_MS);

	this->connectTimeout.setSingleShot(true);
	QObject::connect(&this->connectTimeout, &QTimer::timeout, this, [this]() { this->finishConnect(false, false); });

	this->refresh();
}

Network::~Network() {
	if (this->wlan) {
		// Unregistering waits for callbacks in flight, so none can reach a dead object.
		WlanRegisterNotification(this->wlan, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
		WlanCloseHandle(this->wlan, nullptr);
	}
}

void Network::openWlan() {
	DWORD version = 0;
	HANDLE handle = nullptr;
	// Fails when the WLAN service isn't running (machines without Wi-Fi).
	if (WlanOpenHandle(2, nullptr, &version, &handle) != ERROR_SUCCESS) return;
	this->wlan = handle;

	PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
	if (WlanEnumInterfaces(handle, nullptr, &interfaces) == ERROR_SUCCESS) {
		if (interfaces->dwNumberOfItems > 0) {
			const auto& guid = interfaces->InterfaceInfo[0].InterfaceGuid; // NOLINT
			this->interfaceGuid = QByteArray(reinterpret_cast<const char*>(&guid), sizeof(GUID));
		}
		WlanFreeMemory(interfaces);
	}

	WlanRegisterNotification(
	    handle,
	    WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM,
	    TRUE,
	    wlanCallback,
	    this,
	    nullptr,
	    nullptr
	);
	qCInfo(logNetwork) << "Wi-Fi interface:" << (this->interfaceGuid.isEmpty() ? "none" : "found");
}

void Network::refresh() {
	auto before = QVariantList {
	    this->mWifiAvailable,
	    this->mWifiEnabled,
	    this->mWifiStatus,
	    this->mScanning,
	    this->mEthernet,
	    this->mConnectivity,
	    this->mNetworkName,
	};

	this->readWired();
	this->readWifi();
	if (this->mWifiStatus == "connected" && !this->mWifiName.isEmpty()) this->mNetworkName = this->mWifiName;

	auto after = QVariantList {
	    this->mWifiAvailable,
	    this->mWifiEnabled,
	    this->mWifiStatus,
	    this->mScanning,
	    this->mEthernet,
	    this->mConnectivity,
	    this->mNetworkName,
	};
	if (after != before) emit this->changed();
}

void Network::readWired() {
	// Wired: an Ethernet adapter that is up and has a default gateway (skips virtual switches).
	auto ethernet = false;
	ULONG size = 16 * 1024;
	QByteArray buffer(static_cast<qsizetype>(size), Qt::Uninitialized);
	auto flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size); // NOLINT
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(static_cast<qsizetype>(size));
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size); // NOLINT
	}
	if (result == ERROR_SUCCESS) {
		for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next) { // NOLINT
			if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD && adapter->OperStatus == IfOperStatusUp
			    && adapter->FirstGatewayAddress)
			{
				ethernet = true;
				break;
			}
		}
	}
	this->mEthernet = ethernet;

	// Internet reachability and the connected network's name, as Windows sees them.
	ComPtr<INetworkListManager> manager;
	if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)))) return;

	NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
	if (SUCCEEDED(manager->GetConnectivity(&connectivity))) {
		if (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) {
			this->mConnectivity = "full";
		} else if (connectivity != NLM_CONNECTIVITY_DISCONNECTED) {
			this->mConnectivity = "limited";
		} else {
			this->mConnectivity = "none";
		}
	}

	ComPtr<IEnumNetworks> networks;
	this->mNetworkName.clear();
	if (SUCCEEDED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks))) {
		ComPtr<INetwork> network;
		if (networks->Next(1, &network, nullptr) == S_OK) {
			BSTR name = nullptr;
			if (SUCCEEDED(network->GetName(&name)) && name) {
				this->mNetworkName = QString::fromWCharArray(name);
				SysFreeString(name);
			}
		}
	}
}

void Network::readWifi() {
	auto available = this->wlan && !this->interfaceGuid.isEmpty();
	this->mWifiAvailable = available;
	if (!available) {
		this->mWifiEnabled = false;
		this->mWifiStatus = "disabled";
		this->mWifiName.clear();
		if (!this->mNetworks.isEmpty()) {
			this->mNetworks.clear();
			emit this->networksChanged();
		}
		return;
	}

	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT

	// Radio: on when every radio is on in both software and hardware.
	DWORD size = 0;
	PVOID data = nullptr;
	auto enabled = true;
	if (WlanQueryInterface(this->wlan, guid, wlan_intf_opcode_radio_state, nullptr, &size, &data, nullptr) == ERROR_SUCCESS) {
		auto* radio = static_cast<WLAN_RADIO_STATE*>(data);
		for (DWORD i = 0; i < radio->dwNumberOfPhys; ++i) {
			const auto& phy = radio->PhyRadioState[i]; // NOLINT
			if (phy.dot11SoftwareRadioState != dot11_radio_state_on || phy.dot11HardwareRadioState != dot11_radio_state_on) {
				enabled = false;
			}
		}
		WlanFreeMemory(data);
	}
	this->mWifiEnabled = enabled;

	auto state = wlan_interface_state_disconnected;
	if (WlanQueryInterface(this->wlan, guid, wlan_intf_opcode_interface_state, nullptr, &size, &data, nullptr) == ERROR_SUCCESS) {
		state = *static_cast<WLAN_INTERFACE_STATE*>(data);
		WlanFreeMemory(data);
	}

	this->mWifiName.clear();
	if (state == wlan_interface_state_connected
	    && WlanQueryInterface(this->wlan, guid, wlan_intf_opcode_current_connection, nullptr, &size, &data, nullptr) == ERROR_SUCCESS)
	{
		auto* connection = static_cast<WLAN_CONNECTION_ATTRIBUTES*>(data);
		this->mWifiName = ssidString(connection->wlanAssociationAttributes.dot11Ssid);
		WlanFreeMemory(data);
	}

	if (!enabled) {
		this->mWifiStatus = "disabled";
	} else if (state == wlan_interface_state_connected) {
		this->mWifiStatus = this->mConnectivity == "full" || this->mEthernet ? "connected" : "limited";
	} else if (state == wlan_interface_state_associating || state == wlan_interface_state_discovering
	           || state == wlan_interface_state_authenticating)
	{
		this->mWifiStatus = "connecting";
	} else {
		this->mWifiStatus = "disconnected";
	}

	// Strongest access point per SSID, for BSSID and frequency.
	QHash<QString, WLAN_BSS_ENTRY> bestBss;
	PWLAN_BSS_LIST bssList = nullptr;
	if (WlanGetNetworkBssList(this->wlan, guid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bssList) == ERROR_SUCCESS) {
		for (DWORD i = 0; i < bssList->dwNumberOfItems; ++i) {
			const auto& entry = bssList->wlanBssEntries[i]; // NOLINT
			auto ssid = ssidString(entry.dot11Ssid);
			auto it = bestBss.find(ssid);
			if (it == bestBss.end() || entry.uLinkQuality > it->uLinkQuality) bestBss.insert(ssid, entry);
		}
		WlanFreeMemory(bssList);
	}

	// One entry per SSID: the connected one, else one with a saved profile, else the strongest.
	QHash<QString, QVariantMap> bySsid;
	QStringList order;
	PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
	if (WlanGetAvailableNetworkList(this->wlan, guid, 0, nullptr, &list) == ERROR_SUCCESS) {
		for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
			const auto& entry = list->Network[i]; // NOLINT
			auto ssid = ssidString(entry.dot11Ssid);
			if (ssid.isEmpty()) continue;

			auto active = (entry.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
			auto profile = (entry.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0
			                 ? QString::fromWCharArray(entry.strProfileName)
			                 : QString();
			auto rank = [](const QVariantMap& n) {
				return (n.value("active").toBool() ? 1000 : 0) + (n.value("saved").toBool() ? 500 : 0)
				     + n.value("strength").toInt();
			};

			QVariantMap network {
			    {"ssid", ssid},
			    {"active", active},
			    {"strength", static_cast<int>(entry.wlanSignalQuality)},
			    {"security", securityName(entry.dot11DefaultAuthAlgorithm, entry.bSecurityEnabled)},
			    {"saved", !profile.isEmpty()},
			    {"profile", profile},
			    {"auth", static_cast<int>(entry.dot11DefaultAuthAlgorithm)},
			    {"frequency", 0},
			    {"bssid", QString()},
			};
			if (auto bss = bestBss.find(ssid); bss != bestBss.end()) {
				network["frequency"] = static_cast<int>(bss->ulChCenterFrequency / 1000);
				network["bssid"] = bssidString(bss->dot11Bssid);
			}

			auto existing = bySsid.find(ssid);
			if (existing == bySsid.end()) {
				order.append(ssid);
				bySsid.insert(ssid, network);
			} else if (rank(network) > rank(*existing)) {
				*existing = network;
			}
		}
		WlanFreeMemory(list);
	}

	QVariantList networks;
	for (const auto& ssid: order) networks.append(bySsid.value(ssid));
	if (networks != this->mNetworks) {
		this->mNetworks = networks;
		emit this->networksChanged();
	}
}

void Network::onWlanNotification(quint32 source, quint32 code, const QString& ssid, quint32 reason) {
	if (source == WLAN_NOTIFICATION_SOURCE_ACM) {
		switch (code) {
		case wlan_notification_acm_scan_complete:
		case wlan_notification_acm_scan_fail: this->mScanning = false; break;
		case wlan_notification_acm_connection_complete:
			if (!this->pendingSsid.isEmpty() && ssid == this->pendingSsid) {
				this->finishConnect(reason == WLAN_REASON_CODE_SUCCESS, reason != WLAN_REASON_CODE_SUCCESS);
			}
			break;
		case wlan_notification_acm_connection_attempt_fail:
			if (!this->pendingSsid.isEmpty() && ssid == this->pendingSsid) this->finishConnect(false, true);
			break;
		case wlan_notification_acm_interface_arrival:
		case wlan_notification_acm_interface_removal:
			// Adapter plugged in or out: start over.
			if (this->wlan) {
				WlanRegisterNotification(this->wlan, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
				WlanCloseHandle(this->wlan, nullptr);
				this->wlan = nullptr;
				this->interfaceGuid.clear();
			}
			this->openWlan();
			break;
		default: break;
		}
	}
	this->refresh();
}

void Network::setWifiEnabled(bool enabled) {
	if (!this->wlan || this->interfaceGuid.isEmpty()) return;
	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT

	WLAN_PHY_RADIO_STATE state {};
	state.dwPhyIndex = 0;
	state.dot11SoftwareRadioState = enabled ? dot11_radio_state_on : dot11_radio_state_off;
	auto result = WlanSetInterface(this->wlan, guid, wlan_intf_opcode_radio_state, sizeof(state), &state, nullptr);
	if (result != ERROR_SUCCESS) {
		qCWarning(logNetwork) << "Changing the Wi-Fi radio failed (" << result << "), opening Settings";
		QDesktopServices::openUrl(QUrl("ms-settings:network-wifi"));
	}
	this->refresh();
}

void Network::scan() {
	if (!this->wlan || this->interfaceGuid.isEmpty()) return;
	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT
	if (WlanScan(this->wlan, guid, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
		this->mScanning = true;
		emit this->changed();
		// Drivers report completion within 4 s; don't stay "scanning" if one never does.
		QTimer::singleShot(8000, this, [this]() {
			if (!this->mScanning) return;
			this->mScanning = false;
			this->refresh();
		});
	}
}

void Network::connectTo(const QString& ssid) {
	if (!this->wlan || this->interfaceGuid.isEmpty()) {
		emit this->connectFinished(ssid, false, false);
		return;
	}
	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT

	QVariantMap network;
	for (const auto& entry: this->mNetworks) {
		if (entry.toMap().value("ssid") == ssid) network = entry.toMap();
	}

	auto profile = network.value("profile").toString();
	if (profile.isEmpty()) {
		if (!network.value("security").toString().isEmpty()) {
			emit this->connectFinished(ssid, false, true);
			return;
		}
		// Open network: save a profile for it like Windows does.
		auto escaped = ssid.toHtmlEscaped();
		auto xml = QString(
		               "<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
		               "<name>%1</name><SSIDConfig><SSID><name>%1</name></SSID></SSIDConfig>"
		               "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
		               "<MSM><security><authEncryption><authentication>open</authentication>"
		               "<encryption>none</encryption><useOneX>false</useOneX></authEncryption></security></MSM>"
		               "</WLANProfile>"
		)
		               .arg(escaped);
		DWORD reason = 0;
		auto result = WlanSetProfile(this->wlan, guid, 0, reinterpret_cast<LPCWSTR>(xml.utf16()), nullptr, TRUE, nullptr, &reason);
		if (result == ERROR_ACCESS_DENIED) {
			result = WlanSetProfile(this->wlan, guid, WLAN_PROFILE_USER, reinterpret_cast<LPCWSTR>(xml.utf16()), nullptr, TRUE, nullptr, &reason);
		}
		if (result != ERROR_SUCCESS) {
			emit this->connectFinished(ssid, false, false);
			return;
		}
		profile = ssid;
	}

	WLAN_CONNECTION_PARAMETERS params {};
	params.wlanConnectionMode = wlan_connection_mode_profile;
	auto profileName = profile.toStdWString();
	params.strProfile = profileName.c_str();
	params.dot11BssType = dot11_BSS_type_infrastructure;

	this->pendingSsid = ssid;
	this->connectTimeout.start(CONNECT_TIMEOUT_MS);
	if (WlanConnect(this->wlan, guid, &params, nullptr) != ERROR_SUCCESS) this->finishConnect(false, false);
}

void Network::connectWithPassword(const QString& ssid, const QString& password) {
	if (!this->wlan || this->interfaceGuid.isEmpty()) return;
	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT

	auto auth = DOT11_AUTH_ALGO_RSNA_PSK;
	for (const auto& entry: this->mNetworks) {
		auto map = entry.toMap();
		if (map.value("ssid") == ssid) auth = static_cast<DOT11_AUTH_ALGORITHM>(map.value("auth").toInt());
	}
	QString authentication = "WPA2PSK";
	QString encryption = "AES";
	if (auth == DOT11_AUTH_ALGO_WPA3_SAE) authentication = "WPA3SAE";
	else if (auth == DOT11_AUTH_ALGO_WPA_PSK) authentication = "WPAPSK";

	auto xml = QString(
	               "<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
	               "<name>%1</name><SSIDConfig><SSID><name>%1</name></SSID></SSIDConfig>"
	               "<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>"
	               "<MSM><security><authEncryption><authentication>%2</authentication>"
	               "<encryption>%3</encryption><useOneX>false</useOneX></authEncryption>"
	               "<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
	               "<keyMaterial>%4</keyMaterial></sharedKey></security></MSM>"
	               "</WLANProfile>"
	)
	               .arg(ssid.toHtmlEscaped(), authentication, encryption, password.toHtmlEscaped());

	// Windows keeps the key in its own (encrypted) profile store; nothing is written here.
	DWORD reason = 0;
	auto result = WlanSetProfile(this->wlan, guid, 0, reinterpret_cast<LPCWSTR>(xml.utf16()), nullptr, TRUE, nullptr, &reason);
	if (result == ERROR_ACCESS_DENIED) {
		result = WlanSetProfile(this->wlan, guid, WLAN_PROFILE_USER, reinterpret_cast<LPCWSTR>(xml.utf16()), nullptr, TRUE, nullptr, &reason);
	}
	xml.fill(QChar(' '));
	if (result != ERROR_SUCCESS) {
		qCWarning(logNetwork) << "Saving the Wi-Fi profile failed:" << result << "reason" << reason;
		emit this->connectFinished(ssid, false, true);
		return;
	}

	this->readWifi();
	this->connectTo(ssid);
}

void Network::disconnectWifi() {
	if (!this->wlan || this->interfaceGuid.isEmpty()) return;
	auto* guid = reinterpret_cast<const GUID*>(this->interfaceGuid.constData()); // NOLINT
	WlanDisconnect(this->wlan, guid, nullptr);
	this->refresh();
}

void Network::finishConnect(bool success, bool needsPassword) {
	if (this->pendingSsid.isEmpty()) return;
	auto ssid = this->pendingSsid;
	this->pendingSsid.clear();
	this->connectTimeout.stop();
	qCInfo(logNetwork) << "Connecting to" << ssid << (success ? "succeeded" : "failed");
	emit this->connectFinished(ssid, success, needsPassword);
	this->refresh();
}

} // namespace qs::win32::network
