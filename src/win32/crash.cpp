// ii-windows crash handler (plan 2026-10-31, A3). See crash.hpp.
//
// Installed by a static initializer of this object library, before main(): nothing here may use Qt.
//
// On an unhandled SEH exception (an access violation...), std::terminate, abort() or a CRT invalid-parameter /
// pure-call error, the failing thread hands its EXCEPTION_POINTERS to a writer thread created at start-up (the failing
// thread's stack or heap may be broken: it writes nothing itself), waits up to 10 s, then ends the process with the
// exception code. Windows Error Reporting never gets the process: it used to keep it suspended for 4-5 s while it
// wrote its report, which ii-host took for a frozen shell (2026-10-25).
//
// Output, in %LOCALAPPDATA%\ii-windows\crashes (II_DATA_DIR\crashes for test instances):
//   quickshell-<version>-<yyyymmdd-hhmmss>.dmp   a small minidump (threads, stacks, referenced memory)
//   quickshell-<version>-<yyyymmdd-hhmmss>.json  code, module+offset, subsystem, uptime... (read by ii-host)
// Only the newest 5 are kept, none older than 14 days. Nothing leaves the PC: the report tool asks first.
//
// Tests: II_TEST_CRASH=null|terminate|abort|mpris (only honoured with II_DATA_DIR set, like the other II_TEST_*
// switches) crashes the process II_TEST_CRASH_AFTER_MS (default 3000) ms after start.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
// windows.h first
#include <dbghelp.h>

#include "crash.hpp"

namespace qs::win32::crash {
namespace {

HANDLE requestEvent = nullptr;
HANDLE doneEvent = nullptr;
EXCEPTION_POINTERS* pendingPointers = nullptr;
DWORD pendingThread = 0;
DWORD pendingCode = 0;
const char* pendingKind = "exception";
const char* pendingSubsystem = "gui";
std::atomic<bool> handling {false};
std::wstring crashDir;
ULONGLONG startedAt = 0;

std::wstring env(const wchar_t* name) {
	wchar_t value[1024];
	auto n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
	return n > 0 && n < std::size(value) ? std::wstring(value, n) : std::wstring();
}

std::wstring dataDir() {
	auto custom = env(L"II_DATA_DIR");
	if (!custom.empty()) return custom;
	auto local = env(L"LOCALAPPDATA");
	return local.empty() ? std::wstring() : local + L"\\ii-windows";
}

std::string utf8(const std::wstring& text) {
	if (text.empty()) return {};
	auto n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
	return out;
}

std::string jsonString(const std::string& text) {
	std::string out = "\"";
	for (auto c: text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
			out += buf;
		} else {
			out += c;
		}
	}
	return out + "\"";
}

// Newest 5 kept, nothing older than 14 days (a .dmp and its .json go together).
void trimOldReports() {
	struct Entry {
		std::wstring stem;
		FILETIME written;
	};
	std::vector<Entry> reports;
	WIN32_FIND_DATAW found {};
	auto handle = FindFirstFileW((crashDir + L"\\*.json").c_str(), &found);
	if (handle == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring name = found.cFileName;
		reports.push_back({name.substr(0, name.size() - 5), found.ftLastWriteTime});
	} while (FindNextFileW(handle, &found));
	FindClose(handle);
	std::sort(reports.begin(), reports.end(), [](const Entry& a, const Entry& b) {
		return CompareFileTime(&a.written, &b.written) > 0;
	});
	FILETIME nowFt;
	GetSystemTimeAsFileTime(&nowFt);
	auto now = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
	const ULONGLONG fourteenDays = 14ULL * 24 * 3600 * 10000000ULL;
	for (size_t i = 0; i < reports.size(); ++i) {
		auto written = (static_cast<ULONGLONG>(reports[i].written.dwHighDateTime) << 32) | reports[i].written.dwLowDateTime;
		if (i < 5 && now - written < fourteenDays) continue;
		DeleteFileW((crashDir + L"\\" + reports[i].stem + L".json").c_str());
		DeleteFileW((crashDir + L"\\" + reports[i].stem + L".dmp").c_str());
	}
}

std::wstring exePath() {
	wchar_t path[MAX_PATH * 2];
	auto n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
	return std::wstring(path, n);
}

// "quickshell.exe+0x19ed74" for an address inside a loaded module.
std::string whereOf(void* address) {
	HMODULE module = nullptr;
	if (!address
	    || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                           static_cast<LPCWSTR>(address), &module))
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(address)));
		return buf;
	}
	wchar_t path[MAX_PATH * 2];
	auto n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
	std::wstring name(path, n);
	name = name.substr(name.find_last_of(L"\\/") + 1);
	char offset[32];
	std::snprintf(offset, sizeof(offset), "+0x%llx",
	              static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(module)));
	return utf8(name) + offset;
}

void writeReport() {
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t stamp[32];
	swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	auto version = env(L"II_VERSION");
	auto stem = std::wstring(L"quickshell-") + (version.empty() ? L"dev" : version) + L"-" + stamp;
	CreateDirectoryW(dataDir().c_str(), nullptr);
	CreateDirectoryW(crashDir.c_str(), nullptr);

	auto dumpPath = crashDir + L"\\" + stem + L".dmp";
	auto file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	auto dumped = false;
	if (file != INVALID_HANDLE_VALUE) {
		MINIDUMP_EXCEPTION_INFORMATION info {};
		info.ThreadId = pendingThread;
		info.ExceptionPointers = pendingPointers;
		info.ClientPointers = FALSE;
		auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
		                                       | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
		dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, pendingPointers ? &info : nullptr,
		                           nullptr, nullptr)
		      != FALSE;
		CloseHandle(file);
		if (!dumped) DeleteFileW(dumpPath.c_str());
	}

	void* address = pendingPointers && pendingPointers->ExceptionRecord ? pendingPointers->ExceptionRecord->ExceptionAddress
	                                                                    : nullptr;
	char code[16];
	std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(pendingCode));
	auto renderLoop = env(L"QSG_RENDER_LOOP");
	std::string json = "{\"schema\":1,\"app\":\"quickshell\"";
	json += ",\"version\":" + jsonString(version.empty() ? "dev" : utf8(version));
	json += ",\"kind\":" + jsonString(pendingKind);
	json += ",\"code\":" + jsonString(code);
	json += ",\"where\":" + jsonString(whereOf(address));
	json += ",\"subsystem\":" + jsonString(pendingSubsystem);
	json += ",\"thread\":" + std::to_string(pendingThread);
	json += ",\"uptimeSeconds\":" + std::to_string((GetTickCount64() - startedAt) / 1000);
	json += ",\"renderLoop\":" + jsonString(renderLoop.empty() ? "auto" : utf8(renderLoop));
	json += ",\"safeMode\":" + std::string(env(L"II_SAFE_MODE") == L"1" ? "true" : "false");
	json += ",\"disabled\":" + jsonString(utf8(env(L"II_DISABLE")));
	json += ",\"exe\":" + jsonString(utf8(exePath()));
	json += ",\"pid\":" + std::to_string(GetCurrentProcessId());
	json += ",\"dump\":" + (dumped ? jsonString(utf8(stem + L".dmp")) : std::string("null"));
	json += "}\n";
	// Written last and in one go: ii-host only reads a complete file (temporary name + rename).
	auto temp = crashDir + L"\\" + stem + L".json.tmp";
	file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
		CloseHandle(file);
		MoveFileExW(temp.c_str(), (crashDir + L"\\" + stem + L".json").c_str(), MOVEFILE_REPLACE_EXISTING);
	}
	trimOldReports();
}

DWORD WINAPI writerThread(LPVOID) {
	WaitForSingleObject(requestEvent, INFINITE);
	writeReport();
	SetEvent(doneEvent);
	return 0;
}

[[noreturn]] void handle(EXCEPTION_POINTERS* pointers, DWORD code, const char* kind) {
	if (handling.exchange(true)) {
		// A second failure (another thread, or inside the report): end now.
		TerminateProcess(GetCurrentProcess(), code);
		for (;;) Sleep(INFINITE);
	}
	pendingPointers = pointers;
	pendingThread = GetCurrentThreadId();
	pendingCode = code;
	pendingKind = kind;
	pendingSubsystem = tlsSubsystem ? tlsSubsystem : "gui";
	if (requestEvent && doneEvent) {
		SetEvent(requestEvent);
		WaitForSingleObject(doneEvent, 10000);
	}
	TerminateProcess(GetCurrentProcess(), code);
	for (;;) Sleep(INFINITE);
}

// For failures that come with no EXCEPTION_POINTERS (terminate, abort...): the current context stands in.
[[noreturn]] void handleHere(DWORD code, const char* kind) {
	CONTEXT context {};
	RtlCaptureContext(&context);
	EXCEPTION_RECORD record {};
	record.ExceptionCode = code;
	record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
	record.ExceptionAddress = reinterpret_cast<void*>(context.Rip); // NOLINT
	EXCEPTION_POINTERS pointers {&record, &context};
	handle(&pointers, code, kind);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* pointers) {
	auto code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0xE0000000;
	handle(pointers, code, "exception");
}

// Codes of our own for the failures Windows has no NTSTATUS for (exit codes ii-host recognises: >= 0xC0000000).
constexpr DWORD CODE_TERMINATE = 0xE0000001;
constexpr DWORD CODE_ABORT = 0xE0000002;
constexpr DWORD CODE_INVALID_PARAMETER = 0xE0000003;
constexpr DWORD CODE_PURE_CALL = 0xE0000004;

void onTerminate() { handleHere(CODE_TERMINATE, "terminate"); }
void onAbort(int) { handleHere(CODE_ABORT, "abort"); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
	handleHere(CODE_INVALID_PARAMETER, "invalid-parameter");
}
void onPureCall() { handleHere(CODE_PURE_CALL, "pure-call"); }

void armTestCrash() {
	auto kind = env(L"II_TEST_CRASH");
	if (kind.empty() || env(L"II_DATA_DIR").empty()) return;
	auto afterText = env(L"II_TEST_CRASH_AFTER_MS");
	auto after = afterText.empty() ? 3000 : _wtoi(afterText.c_str());
	std::thread([kind, after]() {
		Sleep(static_cast<DWORD>(std::max(0, after)));
		if (kind == L"mpris") setThreadSubsystem("mpris");
		if (kind == L"terminate") std::terminate();
		if (kind == L"abort") std::abort();
		volatile int* nothing = nullptr;
		*nothing = 1; // NOLINT: the access violation this test is about
	}).detach();
}

bool install() {
	startedAt = GetTickCount64();
	auto dir = dataDir();
	if (dir.empty()) return false;
	crashDir = dir + L"\\crashes";
	requestEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (auto* thread = CreateThread(nullptr, 256 * 1024, writerThread, nullptr, 0, nullptr)) CloseHandle(thread);
	SetUnhandledExceptionFilter(onUnhandled);
	std::set_terminate(onTerminate);
	std::signal(SIGABRT, onAbort);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	_set_invalid_parameter_handler(onInvalidParameter);
	_set_purecall_handler(onPureCall);
	armTestCrash();
	return true;
}

const bool installed = install();

} // namespace
} // namespace qs::win32::crash
