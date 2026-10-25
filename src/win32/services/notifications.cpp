#include "notifications.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <qcoreapplication.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qprocess.h>

#include <windows.h>

// clang-format off
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
// clang-format on

#include "../../core/desktopentry.hpp"
#include "../../core/logcat.hpp"

namespace qs::win32::notifications {

QS_LOGGING_CATEGORY(logNotifs, "quickshell.win32.notifications", QtInfoMsg);

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

QString NotificationUrgency::toString(NotificationUrgency::Enum value) {
	switch (value) {
	case Low: return "Low";
	case Critical: return "Critical";
	default: return "Normal";
	}
}

QString NotificationCloseReason::toString(NotificationCloseReason::Enum value) {
	switch (value) {
	case Expired: return "Expired";
	case Dismissed: return "Dismissed";
	case CloseRequested: return "CloseRequested";
	default: return "Unknown";
	}
}

namespace {

constexpr ULONG_PTR COPYDATA_NOTIFICATION = 0x49490010;
// Health report to ii-host (host/src/health.cpp, protocol version 1).
constexpr ULONG_PTR COPYDATA_SERVICE_STATE = 0x49490020;
constexpr int POLL_MS = 1000;
// Each read asks Windows for every toast it keeps and parses each one: ~15 ms of CPU here, several times that on a
// slow PC, every second. With nobody at the keyboard or mouse for a minute (away, or watching a video) nobody sees a
// banner a few seconds late either: then the toasts are read every 5 s (the health report stays every second).
constexpr DWORD IDLE_AFTER_MS = 60000;
constexpr auto IDLE_READ_INTERVAL = std::chrono::milliseconds(5000);

bool userIdle() {
	LASTINPUTINFO info {sizeof(info), 0};
	return GetLastInputInfo(&info) && GetTickCount() - info.dwTime > IDLE_AFTER_MS;
}

QString toQ(const winrt::hstring& s) { return QString::fromWCharArray(s.c_str(), static_cast<qsizetype>(s.size())); }

LRESULT CALLBACK sinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

class Backend: public QObject {
public:
	static Backend* instance() {
		static auto* backend = new Backend(); // NOLINT
		return backend;
	}

	ObjectModel<Notification> tracked {this};
	QList<QPointer<NotificationServerQml>> servers;

	// GUI thread: build the Notification and hand it to the servers.
	void deliver(NotificationData data) {
		QString desktopEntry;
		if (!data.appId.isEmpty()) {
			if (auto* entry = findEntry(data.appId, data.appName)) {
				desktopEntry = entry->mId;
				auto icon = entry->bindableIcon().value();
				if (!icon.isEmpty()) data.appIcon = icon;
			}
		}

		auto* notification = new Notification(this->nextId++, std::move(data), desktopEntry, this);
		// The summary is somebody's message: it does not go into logs (they end up in bug reports).
		// QS_LOG_NOTIFICATION_TEXT=1 puts it back for local debugging.
		if (qEnvironmentVariable("QS_LOG_NOTIFICATION_TEXT") == QStringLiteral("1")) {
			qCInfo(logNotifs) << "Notification" << notification->id() << "from" << notification->appName() << ":"
			                  << notification->summary();
		} else {
			qCInfo(logNotifs) << "Notification" << notification->id() << "from" << notification->appName();
		}

		for (const auto& server: QList(this->servers)) {
			if (server) emit server->notification(notification);
		}

		// Nobody kept it: drop it quietly (the toast stays in Windows' notification center).
		if (!notification->isTracked()) notification->close(static_cast<NotificationCloseReason::Enum>(0));
	}

	void track(Notification* notification) {
		if (!this->tracked.valueList().contains(notification)) this->tracked.insertObject(notification);
	}

	void finish(Notification* notification, NotificationCloseReason::Enum reason) {
		this->tracked.removeObject(notification);
		if (reason != 0) emit notification->closed(reason);
		// Dismissed in the shell: remove it from Windows' notification center too.
		if (reason == NotificationCloseReason::Dismissed && notification->data().toastId != 0) {
			std::scoped_lock lock(this->mutex);
			this->pendingRemovals.push_back(notification->data().toastId);
			this->wake.notify_one();
		}
		notification->deleteLater();
	}

	// The toast went away in Windows (the app withdrew it, or it was cleared there).
	void toastRemoved(quint32 toastId) {
		for (auto* notification: QList(this->tracked.valueList())) {
			if (notification->data().toastId == toastId) notification->close(NotificationCloseReason::CloseRequested);
		}
	}

	static void launch(const QString& appId) {
		QProcess::startDetached("explorer.exe", {"shell:AppsFolder\\" + appId});
	}

private:
	Backend() {
		this->createSink();
		this->readHostHandshake();
		std::thread([this]() { this->pollToasts(); }).detach();
	}

	// ii-host starts the shell with II_HOST_HWND (its window), II_INSTANCE_ID (an id for this
	// launch) and II_PROTOCOL_VERSION. Without them (quickshell run by hand) nothing is reported
	// and the host, if any, simply never switches Windows' banners off.
	void readHostHandshake() {
		const auto host = qEnvironmentVariable("II_HOST_HWND").toULongLong();
		this->instanceId = qEnvironmentVariable("II_INSTANCE_ID");
		if (host != 0 && !this->instanceId.isEmpty() && qEnvironmentVariable("II_PROTOCOL_VERSION") == QStringLiteral("1")) {
			this->hostHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(host)); // NOLINT
		}
	}

	// Worker thread. What this listener really does, for the host to act on: the host switches
	// Windows' banners off only on a fresh "available / granted / subscribed", so a denied or
	// revoked permission, a failing poll, or this thread hanging all end with Windows' own banners
	// back. A heartbeat proves this loop is alive, not that the whole shell is ready.
	void reportHealth(const char* state, const char* permission, bool subscribed) {
		if (this->hostHwnd == nullptr) return;
		DWORD session = 0;
		ProcessIdToSessionId(GetCurrentProcessId(), &session);
		const auto json = QStringLiteral(
		                      "{\"protocolVersion\":1,\"instanceId\":\"%1\",\"sessionId\":%2,\"requestId\":\"%3\","
		                      "\"messageType\":\"service-state\",\"service\":\"notifications\",\"state\":\"%4\","
		                      "\"permission\":\"%5\",\"subscribed\":%6,\"observedAtMonotonicMs\":%7,\"error\":null}"
		)
		                      .arg(this->instanceId)
		                      .arg(session)
		                      .arg(++this->requestCounter)
		                      .arg(QLatin1String(state), QLatin1String(permission))
		                      .arg(subscribed ? QStringLiteral("true") : QStringLiteral("false"))
		                      .arg(static_cast<qulonglong>(GetTickCount64()));
		auto wide = json.toStdWString();
		COPYDATASTRUCT data {};
		data.dwData = COPYDATA_SERVICE_STATE;
		data.cbData = static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t));
		data.lpData = wide.data();
		DWORD_PTR result = 0;
		// A short timeout: a host that is busy or gone must never stall the poll loop.
		SendMessageTimeoutW(
		    this->hostHwnd,
		    WM_COPYDATA,
		    reinterpret_cast<WPARAM>(this->sinkHwnd), // NOLINT
		    reinterpret_cast<LPARAM>(&data),          // NOLINT
		    SMTO_ABORTIFHUNG,
		    250,
		    &result
		);
	}

	static DesktopEntry* findEntry(const QString& appId, const QString& appName) {
		auto* manager = DesktopEntryManager::instance();
		if (auto* entry = manager->byId(appId)) return entry;
		// Desktop apps without their own AUMID report a path ("{FOLDERID}\dir\app.exe"); the app
		// index names those by executable.
		if (appId.contains('\\')) {
			auto exe = QFileInfo(QString(appId).replace('\\', '/')).completeBaseName();
			if (auto* entry = manager->byId(exe)) return entry;
		}
		return appName.isEmpty() ? nullptr : manager->heuristicLookup(appName);
	}

	void createSink() {
		WNDCLASSW wc {};
		wc.lpfnWndProc = sinkWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"IiShellNotificationSink";
		RegisterClassW(&wc);
		// Hidden top-level (not message-only) so FindWindow in ii-shim finds it.
		auto* sink = this->sinkHwnd = CreateWindowExW(
		    WS_EX_TOOLWINDOW,
		    wc.lpszClassName,
		    L"ii shell notifications",
		    WS_POPUP,
		    0,
		    0,
		    0,
		    0,
		    nullptr,
		    nullptr,
		    wc.hInstance,
		    nullptr
		);
		ChangeWindowMessageFilterEx(sink, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
	}

	// Worker thread (MTA): poll the toast list, report new and removed toasts, apply removals.
	// The permission is re-read on every turn (a cheap query that never prompts), so a revocation
	// is noticed within a second, and a grant given later makes the listener start by itself.
	void pollToasts() {
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			auto listener = UserNotificationListener::Current();
			if (listener.GetAccessStatus() == UserNotificationListenerAccessStatus::Unspecified) {
				this->reportHealth("permission-required", "unknown", false);
				listener.RequestAccessAsync().get(); // asks the user, once
			}

			std::set<quint32> known;
			auto first = true;
			auto lastRead = std::chrono::steady_clock::time_point();
			auto lastReadOk = true;
			auto announcedDenied = false;
			auto announcedReading = false;
			while (true) {
				const auto access = listener.GetAccessStatus();
				if (access != UserNotificationListenerAccessStatus::Allowed) {
					if (!announcedDenied) {
						qCWarning(logNotifs) << "Windows has not granted access to notifications"
						                     << "(Settings > Privacy > Notifications); Windows keeps showing its own banners"
						                     << "and the shell shows only its own notifications until it is granted";
						announcedDenied = true;
						announcedReading = false;
					}
					this->reportHealth(
					    "permission-required",
					    access == UserNotificationListenerAccessStatus::Denied ? "denied" : "unknown",
					    false
					);
					known.clear();
					first = true; // what is there when access returns is history, not news
					std::unique_lock lock(this->mutex);
					this->wake.wait_for(lock, std::chrono::milliseconds(POLL_MS));
					continue;
				}
				announcedDenied = false;
				if (!announcedReading) {
					qCInfo(logNotifs) << "Reading Windows notifications";
					announcedReading = true;
				}

				std::vector<quint32> removals;
				{
					std::scoped_lock lock(this->mutex);
					removals.swap(this->pendingRemovals);
				}
				if (!first && removals.empty() && userIdle()
				    && std::chrono::steady_clock::now() - lastRead < IDLE_READ_INTERVAL)
				{
					this->reportHealth(lastReadOk ? "available" : "error", "granted", lastReadOk);
					std::unique_lock lock(this->mutex);
					this->wake.wait_for(lock, std::chrono::milliseconds(POLL_MS), [this]() {
						return !this->pendingRemovals.empty();
					});
					continue;
				}
				lastRead = std::chrono::steady_clock::now();
				for (auto id: removals) {
					try {
						listener.RemoveNotification(id);
					} catch (...) {}
				}

				std::set<quint32> current;
				auto pollOk = true;
				try {
					auto list = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
					for (const auto& toast: list) {
						auto id = toast.Id();
						current.insert(id);
						// Toasts already there when the shell starts are history, not news.
						if (first || known.contains(id)) continue;
						auto data = readToast(toast);
						QMetaObject::invokeMethod(this, [this, data]() { this->deliver(data); });
					}
				} catch (const winrt::hresult_error& e) {
					pollOk = false;
					qCWarning(logNotifs) << "Reading notifications failed:" << toQ(e.message());
				}

				// Reported after the read, so a loop that is stuck in it stops reporting.
				this->reportHealth(pollOk ? "available" : "error", "granted", pollOk);
				lastReadOk = pollOk;

				for (auto id: known) {
					if (!current.contains(id)) QMetaObject::invokeMethod(this, [this, id]() { this->toastRemoved(id); });
				}
				known = std::move(current);
				first = false;

				std::unique_lock lock(this->mutex);
				this->wake.wait_for(lock, std::chrono::milliseconds(POLL_MS), [this]() {
					return !this->pendingRemovals.empty();
				});
			}
		} catch (const winrt::hresult_error& e) {
			qCWarning(logNotifs) << "Windows notifications unavailable:" << toQ(e.message());
			this->reportHealth("error", "unknown", false);
		}
	}

	static NotificationData readToast(const UserNotification& toast) {
		NotificationData data;
		data.toastId = toast.Id();
		try {
			// WinRT getters may hand back null references; calling through one crashes.
			if (auto app = toast.AppInfo()) {
				data.appId = toQ(app.AppUserModelId());
				if (auto info = app.DisplayInfo()) {
					data.appName = toQ(info.DisplayName());
					data.appIcon = saveLogo(info, data.appId);
				}
			}
		} catch (...) {}

		try {
			auto notification = toast.Notification();
			auto visual = notification ? notification.Visual() : nullptr;
			auto binding = visual ? visual.GetBinding(KnownNotificationBindings::ToastGeneric()) : nullptr;
			if (binding) {
				QStringList texts;
				for (const auto& text: binding.GetTextElements()) texts.append(toQ(text.Text()));
				if (!texts.isEmpty()) data.summary = texts.takeFirst();
				// The shell renders bodies as markup; toast text is plain.
				data.body = texts.join('\n').toHtmlEscaped();
			}
		} catch (...) {}

		if (data.summary.isEmpty()) data.summary = data.appName;
		return data;
	}

	// The app's logo from Windows, cached as a PNG; used when the app index has no icon for it.
	static QString saveLogo(const winrt::Windows::ApplicationModel::AppDisplayInfo& info, const QString& appId) {
		auto dir = QDir(QDir::tempPath()).filePath("ii-windows/notification-icons");
		auto hash = QCryptographicHash::hash(appId.toUtf8(), QCryptographicHash::Sha1).toHex();
		auto path = QDir(dir).filePath(QString::fromLatin1(hash) + ".png");
		if (QFile::exists(path)) return path;

		try {
			// Null for apps without a logo (a null WinRT reference crashes instead of throwing).
			auto logo = info.GetLogo(winrt::Windows::Foundation::Size(64, 64));
			if (!logo) return {};
			auto stream = logo.OpenReadAsync().get();
			auto size = static_cast<uint32_t>(stream.Size());
			if (size == 0 || size > 4 * 1024 * 1024) return {};
			winrt::Windows::Storage::Streams::DataReader reader(stream);
			reader.LoadAsync(size).get();
			QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
			reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(bytes.data()), size)); // NOLINT
			QDir().mkpath(dir);
			QFile file(path);
			if (!file.open(QFile::WriteOnly)) return {};
			file.write(bytes);
			return path;
		} catch (...) {
			return {};
		}
	}

	quint32 nextId = 1;
	HWND sinkHwnd = nullptr;
	HWND hostHwnd = nullptr;
	QString instanceId;
	std::atomic<quint64> requestCounter = 0;
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<quint32> pendingRemovals;
};

// notify-send from ii-shim: "app\x1fsummary\x1fbody\x1ficon\x1furgency[\x1ftimeout\x1ftransient]".
LRESULT CALLBACK sinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_COPYDATA) {
		auto* copy = reinterpret_cast<COPYDATASTRUCT*>(lParam); // NOLINT
		if (copy->dwData == COPYDATA_NOTIFICATION && copy->cbData >= sizeof(wchar_t)) {
			auto payload = QString::fromWCharArray(
			    static_cast<const wchar_t*>(copy->lpData),
			    static_cast<qsizetype>(copy->cbData / sizeof(wchar_t)) - 1
			);
			auto fields = payload.split(QChar(0x1f));
			while (fields.size() < 7) fields.append(QString());

			NotificationData data;
			data.appName = fields[0];
			data.summary = fields[1];
			data.body = fields[2];
			data.appIcon = fields[3];
			auto urgency = fields[4].toLower();
			data.urgency = urgency == "critical" ? NotificationUrgency::Critical
			             : urgency == "low"      ? NotificationUrgency::Low
			                                     : NotificationUrgency::Normal;
			auto ok = false;
			auto timeout = fields[5].toDouble(&ok);
			if (ok) data.expireTimeout = timeout;
			data.transient = fields[6] == "1";

			// Deliver after returning so the sender is never blocked on QML.
			QMetaObject::invokeMethod(
			    Backend::instance(),
			    [data]() { Backend::instance()->deliver(data); },
			    Qt::QueuedConnection
			);
			return TRUE;
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// NotificationAction

NotificationAction::NotificationAction(QString identifier, QString text, Notification* notification)
    : QObject(notification)
    , mIdentifier(std::move(identifier))
    , mText(std::move(text))
    , notification(notification) {}

void NotificationAction::invoke() {
	// Toast buttons can't be pressed from outside; the one action a toast gets opens its app.
	if (this->mIdentifier == "default" && !this->notification->data().appId.isEmpty()) {
		Backend::launch(this->notification->data().appId);
	}
	this->notification->close(NotificationCloseReason::Dismissed);
}

// Notification

Notification::Notification(quint32 id, NotificationData data, QString desktopEntry, QObject* parent)
    : QObject(parent)
    , mId(id)
    , d(std::move(data))
    , mDesktopEntry(std::move(desktopEntry)) {
	if (this->d.toastId != 0 && !this->d.appId.isEmpty()) {
		this->mActions.append(new NotificationAction("default", QCoreApplication::translate("notifications", "Open"), this));
	}
}

QVariantMap Notification::hints() const {
	QVariantMap hints {
	    {"urgency", static_cast<int>(this->d.urgency)},
	    {"transient", this->d.transient},
	};
	if (!this->mDesktopEntry.isEmpty()) hints.insert("desktop-entry", this->mDesktopEntry);
	return hints;
}

void Notification::setTracked(bool tracked) {
	if (!tracked) {
		this->close(NotificationCloseReason::Dismissed);
		return;
	}
	if (this->mTracked || this->mClosed) return;
	this->mTracked = true;
	Backend::instance()->track(this);
	emit this->trackedChanged();
}

void Notification::expire() { this->close(NotificationCloseReason::Expired); }
void Notification::dismiss() { this->close(NotificationCloseReason::Dismissed); }

void Notification::close(NotificationCloseReason::Enum reason) {
	if (this->mClosed) return;
	this->mClosed = true;
	if (this->mTracked) {
		this->mTracked = false;
		emit this->trackedChanged();
	}
	Backend::instance()->finish(this, reason);
}

// NotificationServerQml

NotificationServerQml::NotificationServerQml(QObject* parent): QObject(parent) {
	auto& servers = Backend::instance()->servers;
	servers.removeIf([](const QPointer<NotificationServerQml>& server) { return server.isNull(); });
	servers.append(this);
}

UntypedObjectModel* NotificationServerQml::trackedNotifications() { return &Backend::instance()->tracked; }

} // namespace qs::win32::notifications
