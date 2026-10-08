#pragma once

#include <memory>
#include <functional>
#include <mutex>
#include <qobject.h>
#include <winrt/Windows.Foundation.h>

namespace qs::win32::hardware {
// WinRT events run on arbitrary threads. Never retain a QML object in an event handler.
// close() precedes event revocation; queued calls have a QObject context and are discarded
// by Qt if the receiver is destroyed (including during a QML engine reload).
struct Delivery {
	std::mutex mutex;
	QObject* receiver;
	explicit Delivery(QObject* object): receiver(object) {}
	template <typename F> void post(F function) {
		std::scoped_lock lock(mutex);
		if (receiver) QMetaObject::invokeMethod(receiver, std::move(function), Qt::QueuedConnection);
	}
	void close() { std::scoped_lock lock(mutex); receiver = nullptr; }
};
template <typename Async, typename F> void complete(Async operation, F function, std::function<void(QString)> failure = {}) {
	operation.Completed([function = std::move(function), failure = std::move(failure)](auto const& result, auto status) {
		try {
			if (status == winrt::Windows::Foundation::AsyncStatus::Completed) function(result.GetResults());
			else if (failure) failure(QStringLiteral("Operation cancelled or unavailable"));
		} catch (const winrt::hresult_error& error) { if (failure) failure(QString::fromWCharArray(error.message().c_str())); }
	});
}
}
