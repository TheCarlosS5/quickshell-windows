#pragma once

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtmetamacros.h>

#include "../core/model.hpp"
#include "../core/qmlscreen.hpp"

namespace qs::win32 {

// One application window as Alt+Tab would list it. Windows keeps managing it; we only read
// its state and forward explicit user actions (activate, minimize, close).
class WindowHandle: public QObject {
	Q_OBJECT;
	// clang-format off
	Q_PROPERTY(QString appId READ appId NOTIFY appIdChanged);
	Q_PROPERTY(QString title READ title NOTIFY titleChanged);
	Q_PROPERTY(QObject* parent READ parentToplevel CONSTANT);
	Q_PROPERTY(bool activated READ activated NOTIFY activatedChanged);
	Q_PROPERTY(QList<QuickshellScreenInfo*> screens READ screens NOTIFY screensChanged);
	Q_PROPERTY(bool maximized READ maximized WRITE setMaximized NOTIFY maximizedChanged);
	Q_PROPERTY(bool minimized READ minimized WRITE setMinimized NOTIFY minimizedChanged);
	Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged);
	/// Hyprland-style identity (`toplevel.HyprlandToplevel.address`) for configs that pair
	/// toplevels with `hyprctl clients` data.
	Q_PROPERTY(QObject* HyprlandToplevel READ hyprlandToplevel CONSTANT);
	/// Windows extensions
	Q_PROPERTY(qulonglong hwnd READ hwnd CONSTANT);
	Q_PROPERTY(QString executable READ executable CONSTANT);
	Q_PROPERTY(qint64 pid READ pid CONSTANT);
	Q_PROPERTY(QRect geometry READ geometry NOTIFY geometryChanged);
	/// Index of the virtual desktop the window is on (-1: unknown or on all of them).
	Q_PROPERTY(qint32 desktop READ desktop NOTIFY desktopChanged);
	/// The window asked for attention (it flashed its taskbar button) and has not been brought up since.
	Q_PROPERTY(bool urgent READ urgent NOTIFY urgentChanged);
	// clang-format on

public:
	explicit WindowHandle(quintptr hwnd, QObject* parent = nullptr);

	[[nodiscard]] QString appId() const { return this->mAppId; }
	[[nodiscard]] QString title() const { return this->mTitle; }
	[[nodiscard]] QObject* parentToplevel() const { return nullptr; }
	[[nodiscard]] bool activated() const { return this->mActivated; }
	[[nodiscard]] QList<QuickshellScreenInfo*> screens() const { return this->mScreens; }
	[[nodiscard]] bool maximized() const { return this->mMaximized; }
	[[nodiscard]] bool minimized() const { return this->mMinimized; }
	[[nodiscard]] bool fullscreen() const { return this->mFullscreen; }
	[[nodiscard]] qulonglong hwnd() const { return this->mHwnd; }
	[[nodiscard]] QString executable() const { return this->mExecutable; }
	[[nodiscard]] qint64 pid() const { return this->mPid; }
	[[nodiscard]] QRect geometry() const { return this->mGeometry; }
	[[nodiscard]] qint32 desktop() const { return this->mDesktop; }
	[[nodiscard]] bool urgent() const { return this->mUrgent; }
	void setUrgent(bool urgent) {
		if (urgent == this->mUrgent) return;
		this->mUrgent = urgent;
		emit this->urgentChanged();
	}
	[[nodiscard]] QObject* hyprlandToplevel() const { return this->mHyprland; }
	[[nodiscard]] QString address() const { return QString::number(this->mHwnd, 16); }

	void setMaximized(bool maximized);
	void setMinimized(bool minimized);
	void setFullscreen(bool fullscreen);

	Q_INVOKABLE void activate();
	Q_INVOKABLE void close();
	Q_INVOKABLE void fullscreenOn(QuickshellScreenInfo* screen);
	Q_INVOKABLE void setRectangle(QObject* window, QRect rect);
	Q_INVOKABLE void unsetRectangle();

	// What to read again (plan 2026-10-31, D1: one window's change re-reads only that window, and only what changed).
	enum Part : quint8 {
		Title = 1,
		State = 2,   // minimized, maximized, geometry, fullscreen, screen
		Desktop = 4, // virtual desktop: a call to Explorer, so only when desktops or the window's cloaking change
		AppId = 8,   // AppUserModelID: a property store read, rarely changes
		Activated = 16,
		All = 31,
	};
	// Re-reads parts of the window's state (it is still an app window: the tracker checked).
	void update(quint8 parts, quintptr foreground);

signals:
	void appIdChanged();
	void titleChanged();
	void activatedChanged();
	void screensChanged();
	void maximizedChanged();
	void minimizedChanged();
	void fullscreenChanged();
	void geometryChanged();
	void desktopChanged();
	void urgentChanged();
	void closed();

private:
	quintptr mHwnd;
	qint64 mPid = 0;
	QString mExecutable;
	QString mPackagedId; // the process' AppUserModelID: it never changes, asked once
	bool mPackagedIdKnown = false;
	QString mAppId;
	QString mTitle;
	bool mActivated = false;
	bool mMaximized = false;
	bool mMinimized = false;
	bool mFullscreen = false;
	QRect mGeometry;
	qint32 mDesktop = 0;
	bool mUrgent = false;
	QList<QuickshellScreenInfo*> mScreens;
	QObject* mHyprland = nullptr;
};

class HyprlandToplevelInfo: public QObject {
	Q_OBJECT;
	Q_PROPERTY(QString address READ address CONSTANT);
	Q_PROPERTY(QString title READ title NOTIFY titleChanged);
	Q_PROPERTY(bool activated READ activated NOTIFY activatedChanged);

public:
	explicit HyprlandToplevelInfo(WindowHandle* handle);
	[[nodiscard]] QString address() const { return this->handle->address(); }
	[[nodiscard]] QString title() const { return this->handle->title(); }
	[[nodiscard]] bool activated() const { return this->handle->activated(); }

signals:
	void titleChanged();
	void activatedChanged();

private:
	WindowHandle* handle;
};

// Tracks the user's application windows with out-of-context WinEvent hooks (no injection).
class WindowTracker: public QObject {
	Q_OBJECT;

public:
	static WindowTracker* instance();

	[[nodiscard]] ObjectModel<WindowHandle>* windows() { return &this->mWindows; }
	[[nodiscard]] WindowHandle* active() const { return this->mActive; }
	[[nodiscard]] WindowHandle* forHwnd(quintptr hwnd) const { return this->byHwnd.value(hwnd); }

	// True for windows that Alt+Tab / the taskbar would show.
	static bool isAppWindow(quintptr hwnd);
	// Brings a window to the foreground even when another process owns the foreground.
	static void forceForeground(quintptr hwnd);

	// A full scan of every window soon (the safety net; events are handled window by window).
	void scheduleRefresh();
	// A WinEvent about one window (plan 2026-10-31, D1).
	void onEvent(quint32 event, quintptr hwnd);
	// A window flashed its taskbar button (shell hook, plan 2026-10-31, F17); certain: HSHELL_FLASH itself.
	void onFlash(quintptr hwnd, bool certain);

signals:
	void activeChanged();
	void windowsChanged();

private slots:
	void refresh();
	void processPending();
	void logStats();

private:
	explicit WindowTracker(QObject* parent = nullptr);
	void markDirty(quintptr hwnd, quint8 parts);
	void setForeground(quintptr hwnd);
	void addWindow(quintptr hwnd, quintptr foreground);
	void removeWindow(WindowHandle* handle);

	ObjectModel<WindowHandle> mWindows {this};
	QHash<quintptr, WindowHandle*> byHwnd;
	// When each window last changed its title (raw NAMECHANGE, before the debounce): a shell REDRAW right after one is
	// that title change arriving late, not a flash (plan 2026-10-31, F17).
	QHash<quintptr, quint64> lastNameChange;
	QPointer<WindowHandle> mActive;
	QTimer debounce;          // full scan after structural events, at most one a second
	QTimer pendingTimer;      // the windows with changes, together every 50 ms
	QTimer periodic;          // full scan every 10 s; every third one also re-reads app ids and desktops
	QHash<quintptr, quint8> pending; // hwnd -> parts to re-read; Membership: check whether it is (still) an app window
	quint8 fullParts = WindowHandle::Title | WindowHandle::State | WindowHandle::Activated;
	int periodicCount = 0;
	struct {
		qint64 events = 0;
		qint64 fullScans = 0;
		qint64 windowUpdates = 0;
	} stats;
	QTimer statsTimer;        // QS_TOPLEVELS_STATS=1: the counts in the log every 10 s (to measure D1)
	void* shellHookWindow = nullptr; // HWND registered with RegisterShellHookWindow (F17)
};

} // namespace qs::win32
