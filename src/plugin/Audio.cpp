// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Audio.h"
#include "Config.h"

#include <thread>

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

		// The music at the table: each inn has its own tracks (Music\Music.ini), decoded when you sit down, in turn
		struct Track
		{
			std::string               name;
			std::vector<std::int16_t> pcm;  // stereo, interleaved, 44.1 kHz
		};
		constexpr UINT32                    kMusicRate = 44100;
		std::vector<std::shared_ptr<Track>> g_tracks;     // the playlist, as far as it's decoded
		std::vector<std::string>            g_playlist;   // the files it holds
		std::vector<std::string>            g_wanted;     // the next place's, once the old ones have stopped
		bool                                g_swap = false;
		std::uint32_t                       g_gen = 0;
		IXAudio2SourceVoice*                g_music = nullptr;
		bool                                g_musicOn = false;
		float                               g_musicVol = 0.0f;
		std::size_t                         g_nextTrack = 0;

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
		const auto& cfg = Config::Get();
		const auto& n = a_voice.name;
		const bool  effect = n.rfind("ui_", 0) == 0 || n.rfind("coins", 0) == 0 || n.rfind("quill", 0) == 0 || n.rfind("swoosh", 0) == 0;
		const float level = n.rfind("shout", 0) == 0 ? cfg.shoutVolume : effect ? cfg.effectsVolume : cfg.volume;
		const float vol = g_muted ? 0.0f : a_voice.volume * kMaster * level;

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
		const std::filesystem::path kMusicDir = "Data\\SKSE\\Plugins\\SkyrimYacht\\Music";

		std::string Norm(const std::string& a_s)
		{
			std::string o;
			for (const char c : a_s) {
				if (c == '\'') {
					continue;
				}
				const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (std::isalnum(static_cast<unsigned char>(l))) {
					o += l;
				} else if (!o.empty() && o.back() != ' ') {
					o += ' ';
				}
			}
			while (!o.empty() && o.back() == ' ') {
				o.pop_back();
			}
			if (o.rfind("the ", 0) == 0) {
				o.erase(0, 4);
			}
			return o;
		}
		std::string Trim(std::string a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r\n");
			const auto e = a_s.find_last_not_of(" \t\r\n");
			return b == std::string::npos ? std::string() : a_s.substr(b, e - b + 1);
		}
		std::vector<std::string> Split(const std::string& a_s, char a_sep)
		{
			std::vector<std::string> out;
			std::size_t              p = 0;
			while (p <= a_s.size()) {
				const auto q = (std::min)(a_s.find(a_sep, p), a_s.size());
				if (auto t = Trim(a_s.substr(p, q - p)); !t.empty()) {
					out.push_back(t);
				}
				p = q + 1;
			}
			return out;
		}

		std::string Join(const std::vector<std::string>& a_list)
		{
			std::string o;
			for (const auto& n : a_list) {
				o += (o.empty() ? "" : ", ") + n;
			}
			return o;
		}

		// The room's name, then the places it's in (the inn, the town, the hold)
		std::vector<std::string> Places()
		{
			std::vector<std::string> out;
			auto*                    pc = RE::PlayerCharacter::GetSingleton();
			if (!pc) {
				return out;
			}
			if (auto* cell = pc->GetParentCell()) {
				if (const char* n = cell->GetFullName(); n && *n) {
					out.emplace_back(n);
				}
			}
			int depth = 0;
			for (auto* loc = pc->GetCurrentLocation(); loc && depth < 8; loc = loc->parentLoc, ++depth) {
				if (const char* n = loc->GetFullName(); n && *n) {
					out.emplace_back(n);
				}
			}
			return out;
		}

		// Which tracks play here: the inn's own, else the cold places' or everyone else's (Music.ini)
		std::vector<std::string> PickPlaylist()
		{
			std::vector<std::pair<std::string, std::vector<std::string>>> inns;
			std::vector<std::string>                                      winterPlaces, winter, deflt;
			{
				std::ifstream f(kMusicDir / "Music.ini");
				std::string   line, section;
				while (std::getline(f, line)) {
					if (const auto c = line.find(';'); c != std::string::npos) {
						line.erase(c);
					}
					line = Trim(line);
					if (line.empty()) {
						continue;
					}
					if (line.front() == '[') {
						section = Norm(line);
						continue;
					}
					const auto eq = line.find('=');
					if (eq == std::string::npos) {
						continue;
					}
					const std::string key = Trim(line.substr(0, eq)), val = line.substr(eq + 1);
					if (section == "inns") {
						inns.emplace_back(Norm(key), Split(val, '|'));
					} else if (section == "winter" && Norm(key) == "places") {
						for (const auto& p : Split(val, ',')) {
							winterPlaces.push_back(Norm(p));
						}
					} else if (section == "winter" && Norm(key) == "tracks") {
						winter = Split(val, '|');
					} else if (section == "default" && Norm(key) == "tracks") {
						deflt = Split(val, '|');
					}
				}
			}
			const auto places = Places();
			std::string where;
			for (const auto& p : places) {
				where += (where.empty() ? "" : " / ") + p;
			}
			auto existing = [](const std::vector<std::string>& a_list) {
				std::vector<std::string> out;
				for (const auto& n : a_list) {
					std::error_code ec;
					if (std::filesystem::exists(kMusicDir / n, ec)) {
						out.push_back(n);
					} else {
						spdlog::warn("Music: {} is named in Music.ini but isn't in the Music folder", n);
					}
				}
				return out;
			};
			for (const auto& p : places) {  // the room first, then the places around it
				const auto np = Norm(p);
				for (const auto& [inn, tracks] : inns) {
					if (!inn.empty() && np.find(inn) != std::string::npos) {
						if (auto list = existing(tracks); !list.empty()) {
							spdlog::info("Music: {} ({}): {}", p, where, Join(list));
							return list;
						}
					}
				}
			}
			for (const auto& p : places) {
				const auto np = Norm(p);
				for (const auto& w : winterPlaces) {
					if (!w.empty() && np.find(w) != std::string::npos) {
						if (auto list = existing(winter); !list.empty()) {
							spdlog::info("Music: a cold place ({}): {}", where, Join(list));
							return list;
						}
					}
				}
			}
			auto list = existing(deflt);
			std::error_code iniEc;
			if (list.empty() && !std::filesystem::exists(kMusicDir / "Music.ini", iniEc)) {  // no Music.ini: whatever is in the folder
				std::error_code ec;
				for (const auto& e : std::filesystem::directory_iterator(kMusicDir, ec)) {
					if (e.path().extension() == ".ogg") {
						list.push_back(e.path().filename().string());
					}
				}
			}
			spdlog::info("Music: {}: {}", where.empty() ? "somewhere" : where, Join(list));
			return list;
		}

		// Decodes the playlist off the game's thread, one track at a time (the first is ready in a moment)
		void Decode(std::vector<std::string> a_names, std::uint32_t a_gen)
		{
			std::thread([a_names = std::move(a_names), a_gen]() {
				for (const auto& n : a_names) {
					int       ch = 0, rate = 0;
					short*    out = nullptr;
					const int frames = stb_vorbis_decode_filename((kMusicDir / n).string().c_str(), &ch, &rate, &out);
					if (frames <= 0 || !out || (ch != 1 && ch != 2)) {
						std::free(out);
						spdlog::warn("Music: couldn't read {}", n);
						continue;
					}
					if (rate != static_cast<int>(kMusicRate)) {
						std::free(out);
						spdlog::warn("Music: {} is {} Hz; the table plays 44100 Hz tracks only", n, rate);
						continue;
					}
					auto t = std::make_shared<Track>();
					t->name = n;
					t->pcm.resize(static_cast<std::size_t>(frames) * 2);
					for (int i = 0; i < frames; ++i) {
						t->pcm[i * 2] = out[i * ch];
						t->pcm[i * 2 + 1] = out[i * ch + (ch - 1)];
					}
					std::free(out);
					std::lock_guard lk(g_mtx);
					if (g_gen != a_gen) {
						return;  // you've moved on
					}
					g_tracks.push_back(std::move(t));
				}
			}).detach();
		}

		bool MusicVoice()
		{
			if (g_music) {
				return true;
			}
			if (!g_xa) {
				return false;
			}
			WAVEFORMATEX wf{};
			wf.wFormatTag = WAVE_FORMAT_PCM;
			wf.nChannels = 2;
			wf.nSamplesPerSec = kMusicRate;
			wf.wBitsPerSample = 16;
			wf.nBlockAlign = 4;
			wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
			if (FAILED(g_xa->CreateSourceVoice(&g_music, &wf, 0, 2.0f))) {
				g_music = nullptr;
				return false;
			}
			g_music->SetVolume(0.0f);
			g_music->Start();
			return true;
		}
	}

	void SetMusic(bool a_on)
	{
		if (a_on && !g_musicOn) {
			auto list = PickPlaylist();  // reads the game (main thread), outside the lock
			std::lock_guard lk(g_mtx);
			if (!Init() || !MusicVoice()) {
				return;
			}
			if (list != g_playlist && !list.empty()) {
				g_wanted = std::move(list);
				g_swap = true;
			}
			g_musicOn = true;
			return;
		}
		std::lock_guard lk(g_mtx);
		g_musicOn = a_on;
	}

	void Tick(float a_dt)
	{
		std::lock_guard lk(g_mtx);
		if (!g_ok || !g_music) {
			return;
		}
		XAUDIO2_VOICE_STATE st{};
		g_music->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		// a new place's tracks: once the old ones have faded and stopped, drop them and decode the new
		if (g_swap && st.BuffersQueued == 0) {
			g_tracks.clear();
			g_playlist = std::move(g_wanted);
			g_wanted.clear();
			g_swap = false;
			g_nextTrack = static_cast<std::size_t>(std::rand());
			Decode(g_playlist, ++g_gen);
		}
		// keep a track queued behind the one playing, each in turn
		if (g_musicOn && !g_swap && !g_tracks.empty() && st.BuffersQueued < 2) {
			const auto& t = g_tracks[g_nextTrack % g_tracks.size()];
			++g_nextTrack;
			XAUDIO2_BUFFER buf{};
			buf.AudioBytes = static_cast<UINT32>(t->pcm.size() * 2);
			buf.pAudioData = reinterpret_cast<const BYTE*>(t->pcm.data());
			buf.LoopCount = t->pcm.size() < kMusicRate * 2 * 90 ? 1 : 0;  // a short one plays twice
			g_music->SubmitSourceBuffer(&buf);
		}
		// a slow fade in as you sit down, and out as you leave
		const bool  playing = g_musicOn && !g_swap;
		const float target = playing && !g_muted ? Config::Get().musicVolume : 0.0f;
		const float speed = playing ? 0.5f : g_swap ? 2.0f : 1.2f;  // per second
		g_musicVol += std::clamp(target - g_musicVol, -speed * a_dt, speed * a_dt);
		g_music->SetVolume(g_musicVol);
		if (!playing && g_musicVol <= 0.0001f && st.BuffersQueued > 0) {
			g_music->Stop();
			g_music->FlushSourceBuffers();
			g_music->Start();
		}
	}
}
