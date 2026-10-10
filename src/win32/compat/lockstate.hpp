#pragma once

// The lock contract of WlSessionLock, with no Qt and no Win32 in it so it can be tested alone.
//
// Windows' own lock screen is the only secure one, and LockWorkStation() only *starts* the lock:
// it returns before the session is locked. So four things are kept apart:
//   requested   the shell (QML) asked for a lock
//   accepted    LockWorkStation() returned success (an asynchronous operation was started)
//   locked      the session is locked, known only from Windows (WTS_SESSION_LOCK or a query)
//   error       the call failed, or Windows never confirmed
// `secure` is true only in `locked`. Nothing the shell does can make it true, and the shell can
// never leave `locked`: only Windows' unlock does (a QML "unlock" is not an authentication).

#include <cstdint>
#include <string>

namespace qs::win32::compat {

class LockStateMachine {
public:
	enum class State { Unknown, Unlocked, Requested, Accepted, Locked, Error };
	enum class SessionEvent { Lock, Unlock };

	// What the owner has to do after a transition. Pure data: the machine performs no I/O.
	struct Effect {
		bool callLock = false;       // call LockWorkStation() now, then report lockCallResult(generation)
		bool armConfirm = false;     // start the confirmation timeout for `generation`
		bool cancelConfirm = false;  // stop the confirmation timeout
		bool stateChanged = false;
		bool secureChanged = false;
		bool requestedChanged = false;
		bool lockedConfirmed = false;   // emit windowsLocked
		bool unlockedConfirmed = false; // emit windowsUnlocked
		bool failed = false;            // emit lockFailed(error)
		std::string error;
		uint64_t generation = 0;
	};

	[[nodiscard]] State state() const { return this->mState; }
	[[nodiscard]] bool secure() const { return this->mState == State::Locked; }
	// The shell's lock presentation is wanted: asked for, or Windows is locked.
	[[nodiscard]] bool requested() const { return this->mRequested; }
	[[nodiscard]] const std::string& lastError() const { return this->mError; }
	[[nodiscard]] uint64_t generation() const { return this->mGeneration; }

	static const char* name(State state) {
		switch (state) {
		case State::Unknown: return "unknown";
		case State::Unlocked: return "unlocked";
		case State::Requested: return "requested";
		case State::Accepted: return "accepted";
		case State::Locked: return "locked";
		case State::Error: return "error";
		}
		return "unknown";
	}

	// First observation of the real session at startup. `observed` is Unknown when it could not
	// be read: nothing is assumed in that case.
	Effect init(State observed) {
		Effect e;
		this->change(e, observed == State::Locked || observed == State::Unlocked ? observed : State::Unknown);
		this->setRequested(e, this->mState == State::Locked);
		return e;
	}

	// QML: locked = true.
	Effect requestLock() {
		Effect e;
		if (this->mState == State::Locked) { // Win+L got there first
			this->setRequested(e, true);
			return e;
		}
		if (this->mState == State::Requested || this->mState == State::Accepted) return e; // already under way
		this->setRequested(e, true);
		++this->mGeneration;
		this->mError.clear();
		this->change(e, State::Requested);
		e.callLock = true;
		e.generation = this->mGeneration;
		return e;
	}

	// The outcome of the LockWorkStation() call made for `generation`.
	Effect lockCallResult(uint64_t generation, bool ok, unsigned long errorCode, const std::string& errorText) {
		Effect e;
		if (generation != this->mGeneration || this->mState != State::Requested) return e; // stale
		if (!ok) return this->fail(e, "LockWorkStation failed (" + std::to_string(errorCode) + "): " + errorText);
		this->change(e, State::Accepted);
		e.armConfirm = true;
		e.generation = this->mGeneration;
		return e;
	}

	// The confirmation timeout of `generation` fired without a lock event.
	Effect confirmTimeout(uint64_t generation) {
		Effect e;
		if (generation != this->mGeneration || this->mState != State::Accepted) return e; // stale
		return this->fail(e, "Windows did not confirm the lock");
	}

	// WTS_SESSION_LOCK / WTS_SESSION_UNLOCK. `ownSession` is false for another session's event,
	// which changes nothing.
	Effect sessionEvent(SessionEvent event, bool ownSession) {
		Effect e;
		if (!ownSession) return e;
		++this->mGeneration; // anything still waiting for an earlier request is now stale
		e.cancelConfirm = true;
		this->mError.clear();
		if (event == SessionEvent::Lock) {
			auto wasLocked = this->mState == State::Locked;
			this->change(e, State::Locked);
			this->setRequested(e, true);
			if (!wasLocked) e.lockedConfirmed = true;
		} else {
			auto wasLocked = this->mState == State::Locked;
			this->change(e, State::Unlocked);
			this->setRequested(e, false);
			if (wasLocked) e.unlockedConfirmed = true;
		}
		return e;
	}

	// QML: locked = false. This is the shell cancelling its own request; it is never an unlock.
	Effect qmlUnlock() {
		Effect e;
		if (this->mState == State::Locked) return e; // only Windows can unlock Windows
		if (this->mState == State::Accepted) return e; // the lock is already under way: wait for Windows' word
		if (this->mState == State::Requested) return e; // the call is being made right now
		this->setRequested(e, false);
		return e;
	}

	// A fresh reading of the session (reconnect, RDP). Only a definite answer moves anything.
	Effect resync(State observed) {
		if (observed == State::Locked && this->mState != State::Locked) return this->sessionEvent(SessionEvent::Lock, true);
		if (observed == State::Unlocked && this->mState == State::Locked) return this->sessionEvent(SessionEvent::Unlock, true);
		return {};
	}

private:
	Effect& fail(Effect& e, const std::string& why) {
		this->mError = why;
		this->change(e, State::Error);
		this->setRequested(e, false);
		e.failed = true;
		e.error = why;
		e.cancelConfirm = true;
		return e;
	}

	void change(Effect& e, State next) {
		if (next == this->mState) return;
		auto wasSecure = this->secure();
		this->mState = next;
		e.stateChanged = true;
		if (wasSecure != this->secure()) e.secureChanged = true;
	}

	void setRequested(Effect& e, bool requested) {
		if (requested == this->mRequested) return;
		this->mRequested = requested;
		e.requestedChanged = true;
	}

	State mState = State::Unknown;
	bool mRequested = false;
	std::string mError;
	uint64_t mGeneration = 0;
};

} // namespace qs::win32::compat
