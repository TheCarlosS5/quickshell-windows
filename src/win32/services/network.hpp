#pragma once

// Quickshell.Windows Network: what ii's nmcli-based network service needs, from the Native Wifi
// API (WLAN) and the Network List Manager. Wi-Fi changes arrive as WLAN notifications; wired and
// internet connectivity are re-read on those and every few seconds.

#include <qobject.h>
#include <qqmlintegration.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qvariant.h>

namespace qs::win32::network {

class Network: public QObject {
	Q_OBJECT;
	/// A Wi-Fi adapter exists.
	Q_PROPERTY(bool wifiAvailable READ wifiAvailable NOTIFY changed);
	/// The Wi-Fi radio is on.
	Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY changed);
	/// "connected", "connecting", "disconnected", "disabled" or "limited", like ii's nmcli parsing.
	Q_PROPERTY(QString wifiStatus READ wifiStatus NOTIFY changed);
	Q_PROPERTY(bool wifiScanning READ wifiScanning NOTIFY changed);
	/// A wired adapter is up with a gateway.
	Q_PROPERTY(bool ethernet READ ethernet NOTIFY changed);
	/// "full", "limited" or "none".
	Q_PROPERTY(QString connectivity READ connectivity NOTIFY changed);
	/// Name of the active connection (SSID, or the wired network's name).
	Q_PROPERTY(QString networkName READ networkName NOTIFY changed);
	/// Visible Wi-Fi networks, one per SSID:
	/// {active, strength (0-100), frequency (MHz), ssid, bssid, security, saved}.
	Q_PROPERTY(QVariantList networks READ networks NOTIFY networksChanged);
	QML_NAMED_ELEMENT(Network);
	QML_SINGLETON;

public:
	explicit Network(QObject* parent = nullptr);
	~Network() override;
	Q_DISABLE_COPY_MOVE(Network);

	[[nodiscard]] bool wifiAvailable() const { return this->mWifiAvailable; }
	[[nodiscard]] bool wifiEnabled() const { return this->mWifiEnabled; }
	[[nodiscard]] QString wifiStatus() const { return this->mWifiStatus; }
	[[nodiscard]] bool wifiScanning() const { return this->mScanning; }
	[[nodiscard]] bool ethernet() const { return this->mEthernet; }
	[[nodiscard]] QString connectivity() const { return this->mConnectivity; }
	[[nodiscard]] QString networkName() const { return this->mNetworkName; }
	[[nodiscard]] QVariantList networks() const { return this->mNetworks; }

	Q_INVOKABLE void setWifiEnabled(bool enabled);
	Q_INVOKABLE void scan();
	/// Connects with the saved profile, or an open network directly. Ends in connectFinished();
	/// needsPassword is true when a secured network has no usable saved key.
	Q_INVOKABLE void connectTo(const QString& ssid);
	/// Saves (or replaces) a WPA personal profile with this key and connects.
	Q_INVOKABLE void connectWithPassword(const QString& ssid, const QString& password);
	Q_INVOKABLE void disconnectWifi();
	Q_INVOKABLE void refresh();

	// From the WLAN notification thread, queued to the GUI thread.
	void onWlanNotification(quint32 source, quint32 code, const QString& ssid, quint32 reason);

signals:
	void changed();
	void networksChanged();
	void connectFinished(QString ssid, bool success, bool needsPassword);

private:
	void openWlan();
	void readWifi();
	void readWired();
	void finishConnect(bool success, bool needsPassword);

	void* wlan = nullptr;
	QByteArray interfaceGuid; // GUID of the first Wi-Fi interface
	bool mWifiAvailable = false;
	bool mWifiEnabled = false;
	QString mWifiStatus = "disconnected";
	bool mScanning = false;
	bool mEthernet = false;
	QString mConnectivity = "none";
	QString mNetworkName;
	QString mWifiName;
	QVariantList mNetworks;
	QString pendingSsid;
	QTimer poll;
	QTimer connectTimeout;
};

} // namespace qs::win32::network
