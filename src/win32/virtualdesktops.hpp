#pragma once

#include <qlist.h>
#include <qobject.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <quuid.h>

class QWinEventNotifier;

namespace qs::win32 {

// Windows' virtual desktops, as far as documented means allow.
//  - Which desktop a window is on: IVirtualDesktopManager (documented).
//  - The list of desktops and the current one: Windows has no documented API. Explorer keeps
//    them in HKCU\...\Explorer\VirtualDesktops (VirtualDesktopIDs, CurrentVirtualDesktop,
//    Desktops\{id}\Name); it is read only, watched for changes, and anything missing or
//    malformed means a single desktop (Explorer only writes it once a second one exists).
//  - Switching, creating and closing: Windows' own shortcuts (Win+Ctrl+Left/Right, Win+Ctrl+D,
//    Win+Ctrl+F4), sent with SendInput and tagged so ii-host's keyboard hook passes them on.
class VirtualDesktops: public QObject {
	Q_OBJECT;

public:
	static VirtualDesktops* instance();

	[[nodiscard]] qsizetype count() const { return std::max<qsizetype>(1, this->ids.size()); }
	[[nodiscard]] qsizetype current() const { return this->mCurrent; }
	// The desktop's name, or "" when the user never named it (shells number it).
	[[nodiscard]] QString name(qsizetype index) const { return this->names.value(index); }

	// Index of the desktop the window is on, -1 if unknown (e.g. pinned to all desktops).
	[[nodiscard]] qsizetype desktopOf(quintptr hwnd) const;
	// The window is on a desktop other than the current one (Windows cloaks those).
	[[nodiscard]] bool onOtherDesktop(quintptr hwnd) const;

	void switchTo(qsizetype index);
	void create();
	// Closes a desktop (its windows move to a neighbour, as in Task View).
	void remove(qsizetype index);

signals:
	void changed();

private:
	explicit VirtualDesktops(QObject* parent = nullptr);
	void reload();
	void watch();
	void step();

	QList<QUuid> ids;
	QStringList names;
	qsizetype mCurrent = 0;
	void* manager = nullptr; // IVirtualDesktopManager*
	void* key = nullptr;     // HKEY
	void* event = nullptr;   // HANDLE
	QWinEventNotifier* notifier = nullptr;

	// Shortcuts still to send, one at a time so Windows takes each switch.
	QList<QList<quint16>> pending;
	QTimer pacer;
};

} // namespace qs::win32
