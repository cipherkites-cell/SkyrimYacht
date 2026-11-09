// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Audio.h"
#include "Config.h"

// CommonLib targets Windows 7; XAudio2 2.8+ needs 8 or later in its header. We load the DLL ourselves (see Init).
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <xaudio2.h>
#pragma pop_macro("_WIN32_WINNT")

extern "C" int stb_vorbis_decode_filename(const char* filename, int* channels, int* sample_rate, short** output);

namespace Audio
{
	namespace
	{
		constexpr UINT32 kRate = 48000;
		constexpr int    kVoices = 24;
		constexpr float  kMaster = 0.85f * 2.2f;  // the preview's master level, and its compressor's make-up (the browser's output is louder)

		IXAudio2*                g_xa = nullptr;
		IXAudio2MasteringVoice*  g_master = nullptr;
		UINT32                   g_outChannels = 2;
		IXAudio2SourceVoice*     g_pool[kVoices]{};
		IXAudio2SourceVoice*     g_roll = nullptr;
		float                    g_rollVol = 0.0f;
		bool                     g_tried = false, g_ok = false;
		std::atomic<bool>        g_muted = false;
		std::mutex               g_mtx;
		std::unordered_map<std::string, std::vector<std::int16_t>> g_bank;

		// Tavern music: CC0 tracks (RandomMind, OpenGameArt), decoded once, played in turn while you're at the table
		struct Track
		{
			std::string               name;
			std::vector<std::int16_t> pcm;  // stereo, interleaved
			int                       rate = 44100;
		};
		std::vector<Track>   g_tracks;
		bool                 g_tracksTried = false;
		IXAudio2SourceVoice* g_music = nullptr;
		bool                 g_musicOn = false;
		float                g_musicVol = 0.0f;
		std::size_t          g_nextTrack = 0;

		bool ReadWav(const std::filesystem::path& a_path, std::vector<std::int16_t>& a_out)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				return false;
			}
			std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
				return false;
			}
			std::size_t p = 12;
			while (p + 8 <= b.size()) {
				std::uint32_t len = 0;
				std::memcpy(&len, b.data() + p + 4, 4);
				if (std::memcmp(b.data() + p, "data", 4) == 0) {
					const std::size_t n = (std::min<std::size_t>)(len, b.size() - p - 8) / 2;
					a_out.resize(n);
					std::memcpy(a_out.data(), b.data() + p + 8, n * 2);
					return true;
				}
				p += 8 + len + (len & 1);
			}
			return false;
		}

		WAVEFORMATEX Format()
		{
			WAVEFORMATEX wf{};
			wf.wFormatTag = WAVE_FORMAT_PCM;
			wf.nChannels = 1;
			wf.nSamplesPerSec = kRate;
			wf.wBitsPerSample = 16;
			wf.nBlockAlign = 2;
			wf.nAvgBytesPerSec = kRate * 2;
			return wf;
		}

		void SetPan(IXAudio2SourceVoice* a_v, float a_pan)
		{
			// equal power, mono into the first two speakers
			const float a = (std::clamp(a_pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
			float       m[8]{};
			m[0] = std::cos(a) * 1.41421f * 0.7071f;
			if (g_outChannels > 1) {
				m[1] = std::sin(a) * 1.41421f * 0.7071f;
			} else {
				m[0] = 1.0f;
			}
			a_v->SetOutputMatrix(g_master, 1, (std::min)(g_outChannels, 8u), m);
		}

		bool Init()
		{
			if (g_tried) {
				return g_ok;
			}
			g_tried = true;
			// the sounds
			const std::filesystem::path dir = "Data\\SKSE\\Plugins\\SkyrimYacht\\Sounds";
			std::error_code             ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.path().extension() == ".wav") {
					std::vector<std::int16_t> pcm;
					if (ReadWav(e.path(), pcm)) {
						g_bank.emplace(e.path().stem().string(), std::move(pcm));
					}
				}
			}
			if (g_bank.empty()) {
				spdlog::warn("Sounds: none found in {} - the table will be silent", dir.string());
				return false;
			}
			// our own little mixer
			if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
				// already initialised differently on this thread: XAudio2 copes
			}
			// XAudio2 2.9 ships with Windows 10 and 11 (2.8 with Windows 8); loaded by hand so either works
			using create_t = HRESULT(__stdcall*)(IXAudio2**, UINT32, UINT32);
			create_t create = nullptr;
			for (const char* dll : { "xaudio2_9.dll", "xaudio2_8.dll" }) {
				if (HMODULE h = ::LoadLibraryA(dll)) {
					create = reinterpret_cast<create_t>(::GetProcAddress(h, "XAudio2Create"));
					if (create) {
						spdlog::info("Sounds: using {}", dll);
						break;
					}
				}
			}
			if (!create || FAILED(create(&g_xa, 0, 0x00000001 /* processor 1 */)) || !g_xa) {
				spdlog::warn("Sounds: XAudio2 couldn't start - the table will be silent");
				return false;
			}
			if (FAILED(g_xa->CreateMasteringVoice(&g_master)) || !g_master) {
				spdlog::warn("Sounds: no audio device - the table will be silent");
				return false;
			}
			XAUDIO2_VOICE_DETAILS md{};
			g_master->GetVoiceDetails(&md);
			g_outChannels = md.InputChannels;
			const auto wf = Format();
			for (auto& v : g_pool) {
				g_xa->CreateSourceVoice(&v, &wf, XAUDIO2_VOICE_USEFILTER, 2.0f);
			}
			g_xa->CreateSourceVoice(&g_roll, &wf, 0, 2.0f);
			if (auto it = g_bank.find("roll"); g_roll && it != g_bank.end()) {
				XAUDIO2_BUFFER buf{};
				buf.AudioBytes = static_cast<UINT32>(it->second.size() * 2);
				buf.pAudioData = reinterpret_cast<const BYTE*>(it->second.data());
				buf.LoopCount = XAUDIO2_LOOP_INFINITE;
				g_roll->SetVolume(0.0f);
				g_roll->SubmitSourceBuffer(&buf);
				g_roll->Start();
			}
			g_ok = true;
			spdlog::info("Sounds: {} loaded, {} output channels", g_bank.size(), g_outChannels);
			return true;
		}
	}

	bool Ready()
	{
		std::lock_guard lk(g_mtx);
		return Init();
	}

	double Now()
	{
		static const auto t0 = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}

	int Count(const std::string& a_prefix)
	{
		std::lock_guard lk(g_mtx);
		Init();
		int n = 0;
		while (g_bank.contains(a_prefix + std::to_string(n))) {
			++n;
		}
		return n;
	}

	int Busy()
	{
		std::lock_guard lk(g_mtx);
		if (!g_ok) {
			return 0;
		}
		int n = 0;
		for (auto* v : g_pool) {
			XAUDIO2_VOICE_STATE st{};
			if (v) {
				v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
				n += st.BuffersQueued > 0;
			}
		}
		return n;
	}

	void Play(const Voice& a_voice)
	{
		std::lock_guard lk(g_mtx);
		if (!Init()) {
			return;
		}
		const float vol = g_muted ? 0.0f : a_voice.volume * kMaster * Config::Get().volume;

		if (vol <= 0.0001f) {
			return;
		}
		auto it = g_bank.find(a_voice.name);
		if (it == g_bank.end()) {
			return;
		}
		IXAudio2SourceVoice* voice = nullptr;
		for (auto* v : g_pool) {
			XAUDIO2_VOICE_STATE st{};
			if (v) {
				v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
				if (st.BuffersQueued == 0) {
					voice = v;
					break;
				}
			}
		}
		if (!voice) {
			return;  // all busy: one hit fewer is never heard
		}
		voice->Stop();
		voice->FlushSourceBuffers();
		voice->SetVolume(vol);
		voice->SetFrequencyRatio(std::clamp(a_voice.pitch, 0.25f, 2.0f));
		XAUDIO2_FILTER_PARAMETERS fp{ LowPassFilter, 1.0f, 1.0f };
		if (a_voice.lowpass > 0.0f) {
			fp.Frequency = (std::min)(1.0f, 2.0f * std::sin(3.14159265f * a_voice.lowpass / kRate));
			fp.OneOverQ = 1.4142f;  // Q 0.7
		}
		voice->SetFilterParameters(&fp);
		SetPan(voice, a_voice.pan);
		XAUDIO2_BUFFER buf{};
		buf.AudioBytes = static_cast<UINT32>(it->second.size() * 2);
		buf.pAudioData = reinterpret_cast<const BYTE*>(it->second.data());
		buf.Flags = XAUDIO2_END_OF_STREAM;
		voice->SubmitSourceBuffer(&buf);
		voice->Start();
	}

	void SetRoll(float a_volume)
	{
		std::lock_guard lk(g_mtx);
		if (!g_ok || !g_roll) {
			return;
		}
		// eased toward the target, as the preview's setTargetAtTime
		g_rollVol += (a_volume - g_rollVol) * 0.25f;
		g_roll->SetVolume(g_muted ? 0.0f : g_rollVol * kMaster * Config::Get().volume);
	}

	void StopAll()
	{
		std::lock_guard lk(g_mtx);
		if (!g_ok) {
			return;
		}
		for (auto* v : g_pool) {
			if (v) {
				v->Stop();
				v->FlushSourceBuffers();
			}
		}
		g_rollVol = 0.0f;
		if (g_roll) {
			g_roll->SetVolume(0.0f);
		}
	}

	void SetMuted(bool a_muted) { g_muted = a_muted; }
	bool Muted() { return g_muted; }

	namespace
	{
		void LoadTracks()
		{
			if (g_tracksTried) {
				return;
			}
			g_tracksTried = true;
			const std::filesystem::path dir = "Data\\SKSE\\Plugins\\SkyrimYacht\\Music";
			std::error_code             ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".ogg") {
					continue;
				}
				int    ch = 0, rate = 0;
				short* out = nullptr;
				const int frames = stb_vorbis_decode_filename(e.path().string().c_str(), &ch, &rate, &out);
				if (frames <= 0 || !out || (ch != 1 && ch != 2)) {
					std::free(out);
					continue;
				}
				Track t;
				t.name = e.path().stem().string();
				t.rate = rate;
				t.pcm.resize(static_cast<std::size_t>(frames) * 2);
				for (int i = 0; i < frames; ++i) {
					t.pcm[i * 2] = out[i * ch];
					t.pcm[i * 2 + 1] = out[i * ch + (ch - 1)];
				}
				std::free(out);
				g_tracks.push_back(std::move(t));
			}
			spdlog::info("Music: {} tracks", g_tracks.size());
			if (g_tracks.empty() || !g_xa) {
				return;
			}
			WAVEFORMATEX wf{};
			wf.wFormatTag = WAVE_FORMAT_PCM;
			wf.nChannels = 2;
			wf.nSamplesPerSec = static_cast<DWORD>(g_tracks[0].rate);
			wf.wBitsPerSample = 16;
			wf.nBlockAlign = 4;
			wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
			if (FAILED(g_xa->CreateSourceVoice(&g_music, &wf, 0, 2.0f))) {
				g_music = nullptr;
				return;
			}
			g_music->SetVolume(0.0f);
			g_music->Start();
		}
	}

	void SetMusic(bool a_on)
	{
		std::lock_guard lk(g_mtx);
		if (!Init()) {
			return;
		}
		if (a_on) {
			LoadTracks();
		}
		g_musicOn = a_on;
	}

	void Tick(float a_dt)
	{
		std::lock_guard lk(g_mtx);
		if (!g_ok || !g_music || g_tracks.empty()) {
			return;
		}
		// keep a track queued behind the one playing, each in turn
		XAUDIO2_VOICE_STATE st{};
		g_music->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		if (g_musicOn && st.BuffersQueued < 2) {
			const auto& t = g_tracks[g_nextTrack % g_tracks.size()];
			++g_nextTrack;
			XAUDIO2_BUFFER buf{};
			buf.AudioBytes = static_cast<UINT32>(t.pcm.size() * 2);
			buf.pAudioData = reinterpret_cast<const BYTE*>(t.pcm.data());
			buf.LoopCount = 1;  // each plays twice, then the next
			g_music->SubmitSourceBuffer(&buf);
		}
		// a slow fade in as you sit down, and out as you leave
		const float target = g_musicOn && !g_muted ? Config::Get().musicVolume : 0.0f;
		const float speed = g_musicOn ? 0.5f : 1.2f;  // per second
		g_musicVol += std::clamp(target - g_musicVol, -speed * a_dt, speed * a_dt);
		g_music->SetVolume(g_musicVol);
		if (!g_musicOn && g_musicVol <= 0.0001f && st.BuffersQueued > 0) {
			g_music->Stop();
			g_music->FlushSourceBuffers();
			g_music->Start();
		}
	}
}
