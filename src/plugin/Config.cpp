// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Config.h"

namespace Config
{
	namespace
	{
		Values g_values;
		const char* kPath = "Data\\SKSE\\Plugins\\SkyrimYacht.ini";

		std::string Str(const char* a_section, const char* a_key, const char* a_def)
		{
			char buf[256]{};
			::GetPrivateProfileStringA(a_section, a_key, a_def, buf, sizeof(buf), kPath);
			std::string s = buf;
			if (const auto c = s.find_first_of(";#"); c != std::string::npos) {
				s.resize(c);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			return s;
		}
		float Float(const char* a_section, const char* a_key, float a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			try {
				return std::stof(s);
			} catch (...) {
				return a_def;
			}
		}
		bool Bool(const char* a_section, const char* a_key, bool a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			return s == "1" || _stricmp(s.c_str(), "true") == 0 || _stricmp(s.c_str(), "on") == 0;
		}
	}

	const Values& Get() { return g_values; }
	Values&       Edit() { return g_values; }

	void Save()
	{
		const auto& v = g_values;
		auto put = [](const char* a_section, const char* a_key, const std::string& a_value) {
			::WritePrivateProfileStringA(a_section, a_key, a_value.c_str(), kPath);
		};
		auto f = [](float a_v) { return fmt::format("{:.2f}", a_v); };
		put("Play", "MinBet", std::to_string(v.minBet));
		put("Play", "MaxBet", std::to_string(v.maxBet));
		put("Play", "TableReach", f(v.tableReach));
		put("Play", "MouseSpeed", f(v.mouseSpeed));
		put("Play", "UiScale", f(v.uiScale));
		put("Play", "Volume", f(v.volume));
		put("Play", "PauseGame", v.pauseGame ? "1" : "0");
		put("Table", "Mode", std::to_string(v.mode));
		put("Table", "Distance", f(v.distance));
		put("Table", "Brightness", f(v.brightness));
		put("Table", "TiltDegrees", f(v.tiltDeg));
		put("Table", "FitWidth", f(v.fitWidth));
		put("Table", "CenterX", f(v.centerX));
		put("Table", "CenterY", f(v.centerY));
		put("Table", "FlipTilt", v.flipTilt ? "1" : "0");
		put("Debug", "Overlay", v.debugOverlay ? "1" : "0");
		spdlog::info("Settings saved");
	}

	void Load()
	{
		Values v;
		v.mode = std::clamp(static_cast<int>(Float("Table", "Mode", 0.0f)), 0, 2);
		v.distance = std::clamp(Float("Table", "Distance", v.distance), 20.0f, 400.0f);
		v.brightness = std::clamp(Float("Table", "Brightness", v.brightness), 0.0f, 4.0f);
		v.tiltDeg = Float("Table", "TiltDegrees", v.tiltDeg);
		v.fitWidth = std::clamp(Float("Table", "FitWidth", v.fitWidth), 0.2f, 1.5f);
		v.centerX = Float("Table", "CenterX", v.centerX);
		v.centerY = Float("Table", "CenterY", v.centerY);
		v.depth = Float("Table", "Depth", v.depth);
		v.flipTilt = Bool("Table", "FlipTilt", v.flipTilt);
		v.minBet = static_cast<int>(Float("Play", "MinBet", static_cast<float>(v.minBet)));
		v.maxBet = (std::max)(v.minBet, static_cast<int>(Float("Play", "MaxBet", static_cast<float>(v.maxBet))));
		v.tableReach = Float("Play", "TableReach", v.tableReach);
		v.mouseSpeed = std::clamp(Float("Play", "MouseSpeed", v.mouseSpeed), 0.1f, 5.0f);
		v.uiScale = std::clamp(Float("Play", "UiScale", v.uiScale), 0.5f, 2.0f);
		v.volume = std::clamp(Float("Play", "Volume", v.volume), 0.0f, 2.0f);
		v.pauseGame = Bool("Play", "PauseGame", v.pauseGame);
		v.debugOverlay = Bool("Debug", "Overlay", v.debugOverlay);
		v.logInput = Bool("Debug", "LogInput", v.logInput);
		g_values = v;
		spdlog::info("Settings: {} table, tilt {} fit {} centre {},{} bets {}-{} reach {}", v.mode == 0 ? "drawn by the mod" : v.mode == 1 ? "item view" : "in the room", v.tiltDeg, v.fitWidth, v.centerX, v.centerY, v.minBet,
			v.maxBet, v.tableReach);
	}
}
