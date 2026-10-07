#include "platform.hpp"

#include <qdir.h>
#include <qfile.h>
#include <qstring.h>

#ifdef _WIN32
#include <array>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <qstandardpaths.h>
#include <sys/stat.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <cstdlib>

#include <fcntl.h>
#include <qlogging.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qs::platform {

#ifdef _WIN32

namespace {

HANDLE osHandle(QFile* file) {
	return reinterpret_cast<HANDLE>(_get_osfhandle(file->handle())); // NOLINT
}

// Locks live far past the end of the data so they never block reads of the file
// itself (Windows byte-range locks are mandatory, unlike POSIX advisory locks).
OVERLAPPED lockRegion() {
	OVERLAPPED ov {};
	ov.Offset = 0;
	ov.OffsetHigh = 0x7fffffff;
	return ov;
}

// Not exported by the user-mode SDK headers (lives in ntifs.h).
struct MountPointReparseBuffer {
	DWORD reparseTag;
	WORD reparseDataLength;
	WORD reserved;
	WORD substituteNameOffset;
	WORD substituteNameLength;
	WORD printNameOffset;
	WORD printNameLength;
	WCHAR pathBuffer[1];
};

} // namespace

pid_t currentPid() { return _getpid(); }

QString runtimeDir() { return QDir::tempPath(); }

bool lockFile(QFile* file, LockMode mode, bool wait, bool* busy) {
	auto ov = lockRegion();
	DWORD flags = mode == LockMode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
	if (!wait) flags |= LOCKFILE_FAIL_IMMEDIATELY;

	if (LockFileEx(osHandle(file), flags, 0, 1, 0, &ov)) return true;
	if (busy) *busy = GetLastError() == ERROR_LOCK_VIOLATION || GetLastError() == ERROR_IO_PENDING;
	return false;
}

bool isFileLocked(QFile* file, pid_t* /*ownerPid*/) {
	auto ov = lockRegion();
	auto* handle = osHandle(file);

	if (LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
		UnlockFileEx(handle, 0, 1, 0, &ov);
		return false;
	}

	return true;
}

bool linkDirectory(const QString& target, const QString& linkPath) {
	auto link = QDir::toNativeSeparators(linkPath).toStdWString();
	auto dest = QDir::toNativeSeparators(QDir(target).absolutePath()).toStdWString();

	// An existing junction is an empty directory as far as RemoveDirectory is concerned;
	// this never touches the target's contents.
	RemoveDirectoryW(link.c_str());
	DeleteFileW(link.c_str());

	if (!CreateDirectoryW(link.c_str(), nullptr)) return false;

	auto substitute = L"\\??\\" + dest;
	auto substituteBytes = static_cast<WORD>(substitute.size() * sizeof(WCHAR));
	auto printBytes = static_cast<WORD>(dest.size() * sizeof(WCHAR));
	// both names are NUL terminated inside the path buffer
	auto pathBytes = substituteBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR);
	auto headerBytes = offsetof(MountPointReparseBuffer, pathBuffer);

	std::vector<char> buffer(headerBytes + pathBytes);
	auto* data = reinterpret_cast<MountPointReparseBuffer*>(buffer.data()); // NOLINT
	data->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	data->reparseDataLength = static_cast<WORD>(pathBytes + 8);
	data->substituteNameOffset = 0;
	data->substituteNameLength = substituteBytes;
	data->printNameOffset = static_cast<WORD>(substituteBytes + sizeof(WCHAR));
	data->printNameLength = printBytes;
	std::memcpy(data->pathBuffer, substitute.c_str(), substituteBytes + sizeof(WCHAR));
	std::memcpy(
	    reinterpret_cast<char*>(data->pathBuffer) + data->printNameOffset, // NOLINT
	    dest.c_str(),
	    printBytes + sizeof(WCHAR)
	);

	auto* handle = CreateFileW(
	    link.c_str(),
	    GENERIC_WRITE,
	    0,
	    nullptr,
	    OPEN_EXISTING,
	    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	    nullptr
	);

	if (handle == INVALID_HANDLE_VALUE) {
		RemoveDirectoryW(link.c_str());
		return false;
	}

	DWORD returned = 0;
	auto ok = DeviceIoControl(
	    handle,
	    FSCTL_SET_REPARSE_POINT,
	    data,
	    // tag + length + reserved, then the reparse data itself
	    static_cast<DWORD>(8 + data->reparseDataLength),
	    nullptr,
	    0,
	    &returned,
	    nullptr
	);

	CloseHandle(handle);
	if (!ok) RemoveDirectoryW(link.c_str());
	return ok;
}

int createAnonymousFile(const char* name) {
	auto path = QDir(QDir::tempPath())
	                .filePath(
	                    QStringLiteral("quickshell-%1-%2-%3.tmp")
	                        .arg(QString::fromUtf8(name).replace(':', '-'))
	                        .arg(_getpid())
	                        .arg(reinterpret_cast<quintptr>(name), 0, 16) // NOLINT
	                );

	// _O_TEMPORARY deletes the file when the last descriptor closes, like a memfd.
	return _wopen(
	    reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), // NOLINT
	    _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT,
	    _S_IREAD | _S_IWRITE
	);
}

QString lastErrorString() {
	auto code = GetLastError();
	std::array<wchar_t, 512> buf {};
	FormatMessageW(
	    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	    nullptr,
	    code,
	    0,
	    buf.data(),
	    static_cast<DWORD>(buf.size()),
	    nullptr
	);
	return QStringLiteral("error %1: %2").arg(code).arg(QString::fromWCharArray(buf.data()).trimmed());
}

#else

pid_t currentPid() { return getpid(); }

QString runtimeDir() {
	auto dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
	if (dir.isEmpty()) dir = QString("/run/user/%1").arg(getuid());
	return dir;
}

bool lockFile(QFile* file, LockMode mode, bool wait, bool* busy) {
	struct flock lock = {
	    .l_type = static_cast<short>(mode == LockMode::Exclusive ? F_WRLCK : F_RDLCK),
	    .l_whence = SEEK_SET,
	    .l_start = 0,
	    .l_len = 0,
	    .l_pid = 0,
	};

	if (fcntl(file->handle(), wait ? F_SETLKW : F_SETLK, &lock) == 0) return true; // NOLINT
	if (busy) *busy = errno == EACCES || errno == EAGAIN;
	return false;
}

bool isFileLocked(QFile* file, pid_t* ownerPid) {
	struct flock lock = {
	    .l_type = F_WRLCK,
	    .l_whence = SEEK_SET,
	    .l_start = 0,
	    .l_len = 0,
	    .l_pid = 0,
	};

	fcntl(file->handle(), F_GETLK, &lock); // NOLINT
	auto locked = lock.l_type != F_UNLCK;
	if (locked && ownerPid) *ownerPid = lock.l_pid;
	return locked;
}

bool linkDirectory(const QString& target, const QString& linkPath) {
	QFile::remove(linkPath);
	auto canonical = QDir(target).canonicalPath();
	return symlinkat(canonical.toStdString().c_str(), 0, linkPath.toStdString().c_str()) == 0;
}

int createAnonymousFile(const char* name) { return memfd_create(name, 0); }

QString lastErrorString() { return QStringLiteral("error %1: %2").arg(errno).arg(qt_error_string()); }

#endif

} // namespace qs::platform
