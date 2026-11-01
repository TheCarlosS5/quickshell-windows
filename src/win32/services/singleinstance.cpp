#include "singleinstance.hpp"

#include <string>

#include <qpointer.h>

#include <windows.h>

namespace qs::win32::singleinstance {

namespace {

constexpr ULONG_PTR COPYDATA_ACTIVATE = 0x49490050;
QPointer<SingleInstance> owner; // NOLINT: the instance whose window receives the others' arguments

LRESULT CALLBACK receiverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_COPYDATA) {
		auto* copy = reinterpret_cast<COPYDATASTRUCT*>(lParam); // NOLINT
		if (copy && copy->dwData == COPYDATA_ACTIVATE && owner) {
			auto argument = QString::fromWCharArray(static_cast<const wchar_t*>(copy->lpData),
			                                        static_cast<qsizetype>(copy->cbData / sizeof(wchar_t)));
			while (argument.endsWith(QChar(0))) argument.chop(1);
			QMetaObject::invokeMethod(owner, [argument]() {
				if (owner) owner->receive(argument);
			}, Qt::QueuedConnection);
			return TRUE;
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool SingleInstance::claim(const QString& name, const QString& argument) {
	auto key = name.toStdWString();
	for (auto& c: key) {
		if (c == L'\\' || c == L'/') c = L'-';
	}
	auto mutexName = L"Local\\qs-single-" + key;
	auto* mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
	auto first = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
	auto className = L"QsSingleInstance-" + key;
	if (first) {
		// Kept for the life of the process (the mutex handle is never closed).
		WNDCLASSW wc {};
		wc.lpfnWndProc = receiverProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = className.c_str();
		RegisterClassW(&wc);
		auto* window = CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
		ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
		owner = this;
		return true;
	}
	if (mutex) CloseHandle(mutex);
	// Another process has it: hand it the argument and let it take the foreground (this process has it now).
	auto* receiver = FindWindowExW(HWND_MESSAGE, nullptr, className.c_str(), nullptr);
	if (!receiver) return true; // it is gone or still starting: better two windows than none
	DWORD pid = 0;
	GetWindowThreadProcessId(receiver, &pid);
	AllowSetForegroundWindow(pid);
	auto text = argument.toStdWString();
	COPYDATASTRUCT data {COPYDATA_ACTIVATE, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), text.data()};
	DWORD_PTR result = 0;
	if (!SendMessageTimeoutW(receiver, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result)) return true;
	return false;
}

} // namespace qs::win32::singleinstance
