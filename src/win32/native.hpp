#pragma once

#include <qabstractnativeeventfilter.h>
#include <qhash.h>
#include <qset.h>
#include <qlist.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qpointer.h>
#include <qrect.h>
#include <qregion.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qwindow.h>

// Win32 plumbing shared by the Windows window backends. windows.h stays out of headers.
namespace qs::win32 {

// Mirrors wlr-layer-shell layers. Windows only has "topmost" and "normal" bands,
// so Top/Overlay are both topmost with Overlay re-raised above Top, and
// Background/Bottom are kept directly above the Windows desktop (Progman): under every
// application window, over the Windows wallpaper. They are never children of Explorer's
// windows, so an Explorer restart cannot take them down.
enum class Layer : quint8 {
	Background = 0,
	Bottom = 1,
	Top = 2,
	Overlay = 3,
};

quintptr hwnd(QWindow* window);

// Frameless, no taskbar button, no Alt+Tab entry, no DWM border/corners/shadow.
void applyShellWindowStyle(QWindow* window, bool focusable);

// Places the window in its layer and remembers it so ordering can be restored
// (Overlay above Top) whenever a layered window is shown or raised.
class LayerManager: public QObject {
	Q_OBJECT;

public:
	static LayerManager* instance();

	void setLayer(QWindow* window, Layer layer);
	void remove(QWindow* window);
	// Temporarily drops Top-layer windows on a monitor below everything (fullscreen apps).
	void setFullscreenAppActive(quintptr monitor, bool active);
	// Overlay-layer windows that step aside for fullscreen apps too (notification popups,
	// screen corners), unlike ones the user opens on purpose (overlay, session menu).
	void setYieldsToFullscreen(QWindow* window, bool yields);
	void restack();
	// Puts desktop-layer windows back right above Progman (z-order drifts as apps activate).
	void restackDesktop();

private:
	explicit LayerManager(QObject* parent = nullptr);
	void apply(QWindow* window, Layer layer);
	[[nodiscard]] bool yields(QWindow* window, Layer layer) const {
		return layer == Layer::Top || this->yielding.contains(window);
	}

	QHash<QWindow*, Layer> layers;
	QSet<QWindow*> yielding;
	QList<quintptr> fullscreenMonitors;
};

// Input regions without clipping rendering. QWindow::setMask maps to SetWindowRgn on
// Windows, which also clips what is drawn (losing rounded corners, shadows and anything
// outside the region). Instead the window is made click-through (WS_EX_TRANSPARENT via
// Qt::WindowTransparentForInput) whenever the cursor is outside its input region.
class InputRegions: public QObject {
	Q_OBJECT;

public:
	static InputRegions* instance();

	// region is in window-local logical coordinates. hasMask false = whole window accepts input.
	void setRegion(QWindow* window, const QRegion& region, bool hasMask);
	void remove(QWindow* window);

private slots:
	void poll();

private:
	explicit InputRegions(QObject* parent = nullptr);
	void setPassthrough(QWindow* window, bool passthrough);

	struct Entry {
		QRegion region;
		bool passthrough = false;
	};

	QHash<QWindow*, Entry> entries;
	QTimer timer;
};

// One registered Windows AppBar (SHAppBarMessage) reserving space on a screen edge,
// so maximized windows and the desktop work area respect the panel.
class AppBar: public QObject {
	Q_OBJECT;

public:
	explicit AppBar(QWindow* window);
	~AppBar() override;
	Q_DISABLE_COPY_MOVE(AppBar);

	// edge 0 or thickness <= 0 unregisters. thickness is in logical pixels.
	void update(Qt::Edge edge, qint32 thickness);
	[[nodiscard]] QRect reservedRect() const { return this->mReserved; }

	// Called by the native event router for this AppBar's callback message.
	void handleCallback(quintptr wParam, qintptr lParam);

signals:
	void fullscreenAppChanged(bool active);

private:
	bool registerBar();
	void unregisterBar();
	void apply();
	void verify();
	[[nodiscard]] bool reservationHolds() const;

	QPointer<QWindow> window;
	quintptr mHwnd = 0;
	bool registered = false;
	Qt::Edge edge = static_cast<Qt::Edge>(0);
	qint32 thickness = 0;
	QRect mReserved;
	// Explorer drops appbars when it restarts and can be too busy to answer ABM_NEW:
	// keep checking that the space is really reserved while it should be.
	QTimer watchdog;
	int misses = 0;
	bool warned = false;
};

// Windows session lock/unlock notifications (WTSRegisterSessionNotification).
class SessionEvents: public QObject {
	Q_OBJECT;

public:
	static SessionEvents* instance();
	void dispatch(quint32 event);

signals:
	void locked();
	void unlocked();

private:
	explicit SessionEvents(QObject* parent = nullptr);
	quintptr hwnd = 0;
};

// Routes native messages addressed to our windows (AppBar callbacks) to their owners.
class NativeEventRouter: public QAbstractNativeEventFilter {
public:
	static NativeEventRouter* instance();
	static quint32 appBarMessage();

	void addAppBar(quintptr hwnd, AppBar* bar);
	void removeAppBar(quintptr hwnd);

	bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
	NativeEventRouter() = default;
	QHash<quintptr, AppBar*> appBars;
};

} // namespace qs::win32
