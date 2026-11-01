#pragma once

// ii-windows crash handling (plan 2026-10-31, A3/A4).
//
// crash.cpp (linked into every quickshell.exe) writes %LOCALAPPDATA%\ii-windows\crashes\<name>.dmp + .json when the
// process dies on an unhandled exception, std::terminate or abort, and then ends it at once (no Windows Error Reporting
// pause, so ii-host notices within milliseconds). The JSON names the "subsystem" that was running on the crashing
// thread, which is what this header marks: worker threads and native callbacks say who they are with ScopedSubsystem,
// so a crash is attributed (and can be switched off by ii-host) without debug symbols.
//
// disabled("mpris") is the other half: ii-host restarts a shell that keeps crashing in one part with that part off
// (II_DISABLE=mpris,notifications,...), or everything optional off (II_SAFE_MODE=1).

#include <cstdlib>
#include <cstring>

namespace qs::win32::crash {

// What the current thread is doing; nullptr = the GUI thread's own code ("gui").
inline thread_local const char* tlsSubsystem = nullptr; // NOLINT

class ScopedSubsystem {
public:
	explicit ScopedSubsystem(const char* name): previous(tlsSubsystem) { tlsSubsystem = name; }
	~ScopedSubsystem() { tlsSubsystem = this->previous; }
	ScopedSubsystem(const ScopedSubsystem&) = delete;
	ScopedSubsystem& operator=(const ScopedSubsystem&) = delete;
	ScopedSubsystem(ScopedSubsystem&&) = delete;
	ScopedSubsystem& operator=(ScopedSubsystem&&) = delete;

private:
	const char* previous;
};

// For a thread that does only one thing for its whole life.
inline void setThreadSubsystem(const char* name) { tlsSubsystem = name; }

// II_SAFE_MODE=1 switches off every optional service; II_DISABLE lists single ones ("mpris,cava").
// The tray is never switched off by safe mode alone (without it the user loses their icons).
inline bool disabled(const char* service) {
	const char* list = std::getenv("II_DISABLE"); // NOLINT(concurrency-mt-unsafe): read-only after start
	if (list && *list) {
		const auto length = std::strlen(service);
		for (const char* p = list; *p;) {
			const char* end = std::strchr(p, ',');
			const auto n = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
			if (n == length && std::strncmp(p, service, n) == 0) return true;
			if (!end) break;
			p = end + 1;
		}
	}
	const char* safe = std::getenv("II_SAFE_MODE"); // NOLINT(concurrency-mt-unsafe)
	return safe && safe[0] == '1' && std::strcmp(service, "tray") != 0;
}

} // namespace qs::win32::crash
