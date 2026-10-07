#pragma once

// Thin OS abstraction for the few places quickshell talks to POSIX directly
// (instance locks, runtime directory links, anonymous log buffers, pids).
// The POSIX implementation is the code that previously lived inline.

#include <qstring.h>

#ifdef _WIN32
using pid_t = int; // NOLINT
#else
#include <sys/types.h>
#endif

class QFile;

namespace qs::platform {

pid_t currentPid();

// Default directory for runtime files (sockets, locks, instance dirs).
QString runtimeDir();

enum class LockMode : quint8 {
	Shared,
	Exclusive,
};

// Takes an advisory whole-file lock that lives as long as the file handle.
// When wait is false and the lock is held elsewhere, returns false with busy set.
bool lockFile(QFile* file, LockMode mode, bool wait = false, bool* busy = nullptr);

// Returns true when another process holds a write lock on the file.
// ownerPid is filled where the OS can tell (POSIX); otherwise left untouched.
bool isFileLocked(QFile* file, pid_t* ownerPid = nullptr);

// Creates a directory link (symlink on POSIX, junction on Windows) at linkPath pointing to target.
bool linkDirectory(const QString& target, const QString& linkPath);

// Anonymous, self-deleting read/write file descriptor for early log storage.
// Returns -1 on failure.
int createAnonymousFile(const char* name);

QString lastErrorString();

// Shells often build paths by stripping "file://" from URLs, which on Windows leaves
// "/C:/dir". That form works as a URL path but not as a filesystem path, so file APIs
// normalize it to "C:/dir". Identity on other platforms and for other paths.
QString normalizeLocalPath(const QString& path);

} // namespace qs::platform
