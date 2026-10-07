#include "virtualfiles.hpp"

#ifdef _WIN32
#include <qoperatingsystemversion.h>
#include <qsysinfo.h>

#include <windows.h>

namespace qs::platform {

namespace {

QByteArray uptime() {
	auto seconds = static_cast<double>(GetTickCount64()) / 1000.0;
	return QByteArray::number(seconds, 'f', 2) + ' ' + QByteArray::number(seconds, 'f', 2) + '\n';
}

QByteArray meminfo() {
	MEMORYSTATUSEX mem {};
	mem.dwLength = sizeof(mem);
	GlobalMemoryStatusEx(&mem);

	auto kb = [](DWORDLONG bytes) { return QByteArray::number(static_cast<qulonglong>(bytes / 1024)); };
	// The page file is Windows' swap: commit limit minus physical memory.
	auto swapTotal = mem.ullTotalPageFile > mem.ullTotalPhys ? mem.ullTotalPageFile - mem.ullTotalPhys : 0;
	auto swapFree = mem.ullAvailPageFile > mem.ullAvailPhys ? mem.ullAvailPageFile - mem.ullAvailPhys : 0;
	if (swapFree > swapTotal) swapFree = swapTotal;

	return "MemTotal:       " + kb(mem.ullTotalPhys) + " kB\n"
	     + "MemFree:        " + kb(mem.ullAvailPhys) + " kB\n"
	     + "MemAvailable:   " + kb(mem.ullAvailPhys) + " kB\n"
	     + "SwapTotal:      " + kb(swapTotal) + " kB\n"
	     + "SwapFree:       " + kb(swapFree) + " kB\n";
}

QByteArray stat() {
	FILETIME idle, kernel, user;
	GetSystemTimes(&idle, &kernel, &user);
	auto ticks = [](const FILETIME& t) {
		auto v = (static_cast<quint64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
		return v / 100000; // 100 ns units -> USER_HZ (10 ms) ticks
	};
	auto idleTicks = ticks(idle);
	auto systemTicks = ticks(kernel) - idleTicks; // kernel time includes idle time
	auto n = [](quint64 v) { return QByteArray::number(v); };
	// cpu user nice system idle iowait irq softirq steal guest guest_nice
	return "cpu  " + n(ticks(user)) + " 0 " + n(systemTicks) + ' ' + n(idleTicks) + " 0 0 0 0 0 0\n";
}

QByteArray osRelease() {
	auto pretty = QSysInfo::prettyProductName();
	// Windows 11 still reports "Windows 10" in some product strings.
	if (QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11) {
		pretty.replace("Windows 10", "Windows 11");
	}
	return "NAME=\"Windows\"\n"
	       "PRETTY_NAME=\""
	     + pretty.toUtf8()
	     + "\"\n"
	       "ID=windows\n"
	       "VERSION_ID=\""
	     + QSysInfo::kernelVersion().toUtf8()
	     + "\"\n"
	       "HOME_URL=\"https://www.microsoft.com/windows\"\n"
	       "SUPPORT_URL=\"https://support.microsoft.com\"\n"
	       "LOGO=microsoft-symbolic\n";
}

} // namespace

std::optional<QByteArray> virtualFile(const QString& path) {
	if (path == "/proc/uptime") return uptime();
	if (path == "/proc/meminfo") return meminfo();
	if (path == "/proc/stat") return stat();
	if (path == "/etc/os-release" || path == "/usr/lib/os-release") return osRelease();
	return std::nullopt;
}

} // namespace qs::platform

#else

namespace qs::platform {
std::optional<QByteArray> virtualFile(const QString& /*path*/) { return std::nullopt; }
} // namespace qs::platform

#endif
