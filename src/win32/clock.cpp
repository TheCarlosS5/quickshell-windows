#include "clock.hpp"

#include <windows.h>

namespace qs::win32 {

namespace {

using QueryInterruptTimeFn = void(WINAPI*)(PULONGLONG);

QueryInterruptTimeFn interruptTime() {
	// Windows 10 and later. Resolved once, so older systems fall back instead of failing to start.
	static auto fn = []() -> QueryInterruptTimeFn {
		for (auto* name: {L"kernelbase.dll", L"kernel32.dll"}) {
			if (auto* module = GetModuleHandleW(name)) {
				if (auto* proc = GetProcAddress(module, "QueryInterruptTime")) return reinterpret_cast<QueryInterruptTimeFn>(proc); // NOLINT
			}
		}
		return nullptr;
	}();
	return fn;
}

} // namespace

qint64 MonotonicClock::nowMs() const {
	if (auto* fn = interruptTime()) {
		ULONGLONG hundredNs = 0;
		fn(&hundredNs); // 100 ns units
		return static_cast<qint64>(hundredNs / 10000ULL);
	}
	return static_cast<qint64>(GetTickCount64());
}

bool MonotonicClock::includesSleep() const { return interruptTime() != nullptr; }

} // namespace qs::win32
