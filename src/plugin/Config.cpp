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

	void Load()
	{
		Values v;
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
		v.diceSet = Str("Play", "DiceSet", "");
		v.debugOverlay = Bool("Debug", "Overlay", v.debugOverlay);
		v.logInput = Bool("Debug", "LogInput", v.logInput);
		g_values = v;
		spdlog::info("Settings: tilt {} fit {} centre {},{} bets {}-{} reach {}", v.tiltDeg, v.fitWidth, v.centerX, v.centerY, v.minBet,
			v.maxBet, v.tableReach);
	}
}
