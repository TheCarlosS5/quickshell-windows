#include "systemprobe.hpp"

#include <qdatetime.h>
#include <qfileinfo.h>

#include <windows.h>
#include <tlhelp32.h>

namespace qs::win32::systemprobe {

bool SystemProbe::processRunning(const QStringList& names) {
	if (names.isEmpty()) return false;
	DWORD session = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session);
	auto* snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W entry {};
	entry.dwSize = sizeof(entry);
	auto found = false;
	for (auto ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
		auto exe = QString::fromWCharArray(entry.szExeFile);
		for (const auto& name: names) {
			if (exe.compare(name, Qt::CaseInsensitive) != 0) continue;
			DWORD processSession = 0;
			if (ProcessIdToSessionId(entry.th32ProcessID, &processSession) && processSession == session) found = true;
			break;
		}
	}
	CloseHandle(snapshot);
	return found;
}

qint64 SystemProbe::modifiedMs(const QString& path) {
	if (path.isEmpty()) return -1;
	QFileInfo info(path);
	if (!info.exists()) return -1;
	return info.lastModified().toMSecsSinceEpoch();
}

} // namespace qs::win32::systemprobe
