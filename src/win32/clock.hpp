#pragma once

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtypes.h>

namespace qs::win32 {

/// A monotonic clock for timers: milliseconds that only go forward, are not changed by the user (or
/// time sync) setting the wall clock, and keep counting while the PC sleeps or hibernates.
///
/// It is QueryInterruptTime: "interrupt time" includes the time spent in sleep and hibernation
/// (QueryUnbiasedInterruptTime, GetTickCount64-style counters that stop in sleep, would make a
/// pomodoro that was running during a suspend come back late). It starts at boot, so it is never
/// stored: callers compare it with the wall clock to notice that the wall clock was adjusted.
class MonotonicClock: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit MonotonicClock(QObject* parent = nullptr): QObject(parent) {}

	/// Milliseconds since boot, including sleep. Never decreases.
	Q_INVOKABLE [[nodiscard]] qint64 nowMs() const;
	/// Whether the clock really is QueryInterruptTime (otherwise GetTickCount64, which may not
	/// count sleep: timers still work, but a suspend is then not counted as elapsed).
	Q_INVOKABLE [[nodiscard]] bool includesSleep() const;
};

} // namespace qs::win32
