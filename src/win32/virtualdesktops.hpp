#pragma once

#include <functional>

#include <qelapsedtimer.h>
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
	// Moves another app's window to a desktop. Needs `ii-shim vdesk` (Windows 11 24H2+).
	bool moveWindow(quintptr hwnd, qsizetype index);
	// Explorer's internal desktop interface is usable through `ii-shim vdesk`: direct switches,
	// closing without visiting, moving windows. Otherwise Windows' shortcuts, one step at a time.
	// Each operation is available on its own: `ii-shim vdesk probe` checks the interface for each of
	// them on this Windows (no build number is trusted), and one that keeps failing at run time is
	// switched off for a while (growing back-off) instead of being retried on every request.
	[[nodiscard]] bool direct() const { return this->canSwitchDirect() || this->canRemoveDirect() || this->canMove(); }
	[[nodiscard]] bool canSwitchDirect() const { return this->available(Op::Switch); }
	[[nodiscard]] bool canRemoveDirect() const { return this->available(Op::Remove); }
	[[nodiscard]] bool canMove() const { return this->available(Op::Move); }

	// Closes the desktop the user just left, but only if it is still exactly as it was when they
	// left it: same list of desktops in the same order, nobody on it, not named by the user, and
	// not the first. `index` is the position the caller believes it has; it is checked against the
	// desktop recorded when the user left, never trusted by itself. Returns whether a removal was
	// requested (ii-shim re-checks everything right before acting and may still decline).
	bool removeLeftIfEmpty(qsizetype index);

signals:
	void changed();

private:
	enum class Op { Switch, Remove, Move, Count };
	explicit VirtualDesktops(QObject* parent = nullptr);
	void reload();
	void watch();
	void step();
	void clear();

	QList<QUuid> ids;
	QStringList names;
	qsizetype mCurrent = 0;
	void* manager = nullptr; // IVirtualDesktopManager*
	void* key = nullptr;     // HKEY
	void* event = nullptr;   // HANDLE
	QWinEventNotifier* notifier = nullptr;

	// What was asked, done one shortcut at a time, each confirmed by Windows (registry change).
	qsizetype target = -1; // desktop to end on
	bool creating = false;
	QUuid closing;         // desktop to close once it is the current one
	QUuid returnTo;        // where to go back after closing it
	bool inFlight = false;
	QElapsedTimer sentAt;
	QTimer pacer;

	// What `ii-shim vdesk probe` said, and how each operation has been doing since.
	struct OpState {
		bool verified = false;     // probe said "yes" for it
		QString reason;            // probe's explanation when it did not
		int failures = 0;          // consecutive real failures (not "cancelled": those are normal)
		int trips = 0;             // times it was switched off
		QElapsedTimer offSince;    // valid while switched off
		qint64 offForMs = 0;
	};
	OpState ops[static_cast<int>(Op::Count)];
	[[nodiscard]] bool available(Op op) const;
	// Runs one `ii-shim vdesk` command; `failed` runs if Windows or the shim could not do it
	// (not when the shim declined because things had changed), so the caller can fall back.
	bool runVdesk(Op op, const QStringList& args, std::function<void()> failed = {});
	void recordResult(Op op, int exitCode, const QString& lastLine);
	void applyProbe(const QString& output);

	// The desktop the user left last, with the ordered list of desktops at that moment.
	struct Departure {
		QUuid id;
		QList<QUuid> ids;
		QElapsedTimer when;
	} departure;
};

} // namespace qs::win32
