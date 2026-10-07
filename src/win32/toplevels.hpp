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
	/// Windows extensions
	Q_PROPERTY(qulonglong hwnd READ hwnd CONSTANT);
	Q_PROPERTY(QString executable READ executable CONSTANT);
	Q_PROPERTY(qint64 pid READ pid CONSTANT);
	Q_PROPERTY(QRect geometry READ geometry NOTIFY geometryChanged);
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

	void setMaximized(bool maximized);
	void setMinimized(bool minimized);
	void setFullscreen(bool fullscreen);

	Q_INVOKABLE void activate();
	Q_INVOKABLE void close();
	Q_INVOKABLE void fullscreenOn(QuickshellScreenInfo* screen);
	Q_INVOKABLE void setRectangle(QObject* window, QRect rect);
	Q_INVOKABLE void unsetRectangle();

	// Re-reads state from the window; returns false if it no longer qualifies.
	bool refresh(quintptr foreground);

signals:
	void appIdChanged();
	void titleChanged();
	void activatedChanged();
	void screensChanged();
	void maximizedChanged();
	void minimizedChanged();
	void fullscreenChanged();
	void geometryChanged();
	void closed();

private:
	quintptr mHwnd;
	qint64 mPid = 0;
	QString mExecutable;
	QString mAppId;
	QString mTitle;
	bool mActivated = false;
	bool mMaximized = false;
	bool mMinimized = false;
	bool mFullscreen = false;
	QRect mGeometry;
	QList<QuickshellScreenInfo*> mScreens;
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

	void scheduleRefresh();

signals:
	void activeChanged();
	void windowsChanged();

private slots:
	void refresh();

private:
	explicit WindowTracker(QObject* parent = nullptr);

	ObjectModel<WindowHandle> mWindows {this};
	QHash<quintptr, WindowHandle*> byHwnd;
	QPointer<WindowHandle> mActive;
	QTimer debounce;
};

} // namespace qs::win32
