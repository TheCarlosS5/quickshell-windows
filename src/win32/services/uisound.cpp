#include "uisound.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

#include <qfile.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qurl.h>

#include <windows.h>
#include <mmsystem.h>

#include "../../core/logcat.hpp"

namespace qs::win32::uisound {

namespace {
QS_LOGGING_CATEGORY(logUiSound, "quickshell.win32.uisound", QtInfoMsg);

// Scales the samples of a 16-bit PCM WAV in place. False if it is not one.
bool scalePcm16(QByteArray& wav, qreal volume) {
	if (wav.size() < 12 || std::memcmp(wav.constData(), "RIFF", 4) != 0 || std::memcmp(wav.constData() + 8, "WAVE", 4) != 0) {
		return false;
	}
	quint16 bits = 0;
	quint16 format = 0;
	qsizetype pos = 12;
	while (pos + 8 <= wav.size()) {
		quint32 size = 0;
		std::memcpy(&size, wav.constData() + pos + 4, 4);
		auto body = pos + 8;
		if (std::memcmp(wav.constData() + pos, "fmt ", 4) == 0 && body + 16 <= wav.size()) {
			std::memcpy(&format, wav.constData() + body, 2);
			std::memcpy(&bits, wav.constData() + body + 14, 2);
		} else if (std::memcmp(wav.constData() + pos, "data", 4) == 0) {
			if (format != 1 || bits != 16) return false;
			auto end = std::min<qsizetype>(body + size, wav.size());
			auto* samples = reinterpret_cast<int16_t*>(wav.data() + body); // NOLINT
			for (qsizetype i = 0; i < (end - body) / 2; ++i) {
				samples[i] = static_cast<int16_t>(std::clamp<int>(static_cast<int>(samples[i] * volume), -32768, 32767)); // NOLINT
			}
			return true;
		}
		pos = body + size + (size & 1);
	}
	return false;
}
} // namespace

UiSound::~UiSound() { this->stop(); }

void UiSound::play(const QString& path, qreal volume) {
	volume = std::clamp<qreal>(volume, 0, 1);
	if (volume <= 0) return;
	auto local = path.startsWith("file:") ? QUrl(path).toLocalFile() : path;
	auto key = QString::number(qRound(volume * 100)) + "|" + local;

	auto it = this->cache.find(key);
	if (it == this->cache.end()) {
		QFile file(local);
		if (!file.open(QFile::ReadOnly)) {
			qCWarning(logUiSound) << "Cannot read" << local;
			return;
		}
		auto wav = file.readAll();
		if (!scalePcm16(wav, volume)) {
			qCWarning(logUiSound) << "Not a 16-bit PCM WAV:" << local;
			return;
		}
		it = this->cache.insert(key, wav);
	}
	// The data stays in the cache while it plays (PlaySound reads it asynchronously).
	PlaySoundW(reinterpret_cast<LPCWSTR>(it->constData()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); // NOLINT
}

void UiSound::stop() { PlaySoundW(nullptr, nullptr, 0); }

} // namespace qs::win32::uisound
