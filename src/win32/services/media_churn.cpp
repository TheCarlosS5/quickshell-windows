// ii-media-churn: creates and drops Windows media sessions (GSMTC) as fast as a browser feed does, to test the shell's
// MPRIS backend (plan 2026-10-31, A8). Each session is a WinRT MediaPlayer playing one second of SILENCE in a loop
// (nothing audible: the sandbox's audio reaches the user's speakers), with a title, artist and cover, so Windows
// lists it like a YouTube tab.
//
//   ii-media-churn.exe [--seconds N] [--parallel P] [--min-ms A] [--max-ms B]
//
// Defaults: 60 s, 3 players at once (three tabs), each living 40-400 ms. Prints the number of sessions created.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

// clang-format off
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
// clang-format on

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Storage::Streams;

namespace {

std::vector<uint8_t> silentWav() {
	const uint32_t rate = 8000, samples = rate; // one second, 16-bit mono
	const uint32_t dataBytes = samples * 2;
	std::vector<uint8_t> wav(44 + dataBytes, 0);
	auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) wav[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
	auto put16 = [&](size_t at, uint16_t v) { wav[at] = static_cast<uint8_t>(v); wav[at + 1] = static_cast<uint8_t>(v >> 8); };
	std::memcpy(wav.data(), "RIFF", 4);
	put32(4, 36 + dataBytes);
	std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1);
	put16(22, 1);
	put32(24, rate);
	put32(28, rate * 2);
	put16(32, 2);
	put16(34, 16);
	std::memcpy(wav.data() + 36, "data", 4);
	put32(40, dataBytes);
	return wav;
}

// A tiny solid-colour BMP as the cover (the backend reads and caches covers: exercise that path too).
std::vector<uint8_t> coverBmp(uint8_t shade) {
	const int w = 8, h = 8;
	const uint32_t pixels = w * h * 4;
	std::vector<uint8_t> bmp(54 + pixels, 0);
	auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bmp[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
	bmp[0] = 'B';
	bmp[1] = 'M';
	put32(2, static_cast<uint32_t>(bmp.size()));
	put32(10, 54);
	put32(14, 40);
	put32(18, w);
	put32(22, h);
	bmp[26] = 1;
	bmp[28] = 32;
	put32(34, pixels);
	for (uint32_t i = 54; i < bmp.size(); i += 4) {
		bmp[i] = shade;
		bmp[i + 1] = static_cast<uint8_t>(255 - shade);
		bmp[i + 2] = 128;
		bmp[i + 3] = 255;
	}
	return bmp;
}

InMemoryRandomAccessStream streamOf(const std::vector<uint8_t>& bytes) {
	InMemoryRandomAccessStream stream;
	DataWriter writer(stream);
	writer.WriteBytes(array_view<const uint8_t>(bytes));
	writer.StoreAsync().get();
	writer.DetachStream();
	stream.Seek(0);
	return stream;
}

int argInt(int argc, char** argv, const char* name, int fallback) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::string(argv[i]) == name) return std::atoi(argv[i + 1]);
	}
	return fallback;
}

} // namespace

int main(int argc, char** argv) {
	init_apartment(apartment_type::multi_threaded);
	const int seconds = argInt(argc, argv, "--seconds", 60);
	const int parallel = argInt(argc, argv, "--parallel", 3);
	const int minMs = argInt(argc, argv, "--min-ms", 40);
	const int maxMs = argInt(argc, argv, "--max-ms", 400);
	const auto wav = silentWav();
	std::atomic<long> created {0};
	std::atomic<long> failed {0};
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

	std::vector<std::thread> workers;
	for (int t = 0; t < parallel; ++t) {
		workers.emplace_back([&, t]() {
			init_apartment(apartment_type::multi_threaded);
			std::mt19937 random(static_cast<unsigned>(t * 7919 + 17));
			std::uniform_int_distribution<int> life(minMs, maxMs);
			long n = 0;
			while (std::chrono::steady_clock::now() < until) {
				try {
					MediaPlayer player;
					player.Volume(1.0); // silence at full volume: Windows lists it as a playing session
					player.IsLoopingEnabled(true);
					auto source = MediaSource::CreateFromStream(streamOf(wav), L"audio/wav");
					MediaPlaybackItem item(source);
					auto props = item.GetDisplayProperties();
					props.Type(MediaPlaybackType::Music);
					props.MusicProperties().Title(hstring(L"Churn video " + std::to_wstring(t) + L"-" + std::to_wstring(++n)));
					props.MusicProperties().Artist(L"ii-media-churn");
					props.Thumbnail(RandomAccessStreamReference::CreateFromStream(streamOf(coverBmp(static_cast<uint8_t>(n * 37)))));
					item.ApplyDisplayProperties(props);
					player.Source(item);
					player.Play();
					++created;
					std::this_thread::sleep_for(std::chrono::milliseconds(life(random)));
					if (n % 3 == 0) player.Pause(); // some end paused, like a scrolled-away video
					std::this_thread::sleep_for(std::chrono::milliseconds(life(random) / 4));
					player.Close();
				} catch (const hresult_error& e) {
					++failed;
					std::fwprintf(stderr, L"session failed: 0x%08x %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
			}
		});
	}
	for (auto& w: workers) w.join();
	std::printf("created %ld sessions, %ld failed\n", created.load(), failed.load());
	return failed.load() > created.load() / 2 ? 1 : 0;
}
