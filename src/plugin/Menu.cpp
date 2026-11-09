// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
//
// The settings page in SKSE Menu Framework (if it's installed): every INI setting, changed live, saved to the INI.
#include "Menu.h"
#include "Config.h"

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
		namespace UI = ImGuiMCP;

		bool g_dirty = false;

		void Hint(const char* a_text)
		{
			UI::TextDisabled("%s", a_text);
			UI::Spacing();
		}
		void Changed(bool a_changed) { g_dirty |= a_changed; }

		void SaveRow()
		{
			UI::Spacing();
			UI::Separator();
			if (UI::Button("Save to SkyrimYacht.ini")) {
				Config::Save();
				g_dirty = false;
			}
			UI::SameLine();
			if (UI::Button("Undo changes")) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("(not saved yet: changes last until you quit)");
			}
		}

		void __stdcall Table()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("Where the table sits");
			Hint("Changes show at once if a game is open.");
			Changed(UI::SliderFloat("Distance", &v.distance, 40.0f, 300.0f, "%.0f units"));
			Hint("In the room only: how far in front of the camera.");
			Changed(UI::SliderFloat("Size on screen", &v.fitWidth, 0.3f, 1.2f, "%.2f"));
			Hint("How much of the screen's width the tray and both cups take.");
			Changed(UI::SliderFloat("Tilt", &v.tiltDeg, 20.0f, 85.0f, "%.0f degrees"));
			Hint("How far you look down on it.");
			Changed(UI::SliderFloat("Across", &v.centerX, 0.2f, 0.8f, "%.2f"));
			Changed(UI::SliderFloat("Up / down", &v.centerY, 0.3f, 0.8f, "%.2f"));
			Hint("Where its middle sits on screen (0.5 is the centre).");
			Changed(UI::Checkbox("Flip the tilt", &v.flipTilt));
			Hint("Turn this on if you see the tray from underneath.");
			UI::SeparatorText("Look");
			Changed(UI::SliderFloat("Brightness", &v.brightness, 1.0f, 4.0f, "%.1f"));
			Hint("In the room only: lifts the table out of a dark room. Applies to the next table.");
			int mode = v.mode;
			const char* modes[] = { "By the mod, over the game (recommended)", "In the game's item view", "In the room" };
			if (UI::Combo("Drawn", &mode, modes, 3)) {
				v.mode = mode;
				g_dirty = true;
			}
			Hint("Applies to the next table.");
			SaveRow();
		}

		void __stdcall Play()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("The wager");
			if (UI::SliderInt("Smallest bet", &v.minBet, 5, 500, "%d septims")) {
				v.maxBet = (std::max)(v.maxBet, v.minBet);
				g_dirty = true;
			}
			if (UI::SliderInt("Largest bet", &v.maxBet, 5, 2000, "%d septims")) {
				v.minBet = (std::min)(v.minBet, v.maxBet);
				g_dirty = true;
			}
			UI::SeparatorText("Who you can ask");
			Changed(UI::SliderFloat("Table reach", &v.tableReach, 60.0f, 400.0f, "%.0f units"));
			Hint("How near a table a seated person must be to offer a game (70 is about a metre).");
			UI::SeparatorText("Controls and sound");
			Changed(UI::SliderFloat("Mouse speed", &v.mouseSpeed, 0.2f, 3.0f, "%.2f"));
			Changed(UI::SliderFloat("Volume", &v.volume, 0.0f, 2.0f, "%.2f"));
			Hint("The dice, the cups and the Shout.");
			Changed(UI::SliderFloat("Interface size", &v.uiScale, 0.6f, 1.6f, "%.2f"));
			Hint("Takes effect the next time the game starts.");
			Changed(UI::Checkbox("Pause the world while playing", &v.pauseGame));
			Hint("In the room only (the other ways always pause).");
			SaveRow();
		}

		void __stdcall Debug()
		{
			auto& v = Config::Edit();
			Changed(UI::Checkbox("Show the tray outline and the dice the mouse can hit", &v.debugOverlay));
			Hint("To check that clicks line up with the table.");
			SaveRow();
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in SkyrimYacht.ini");
			return;
		}
		SKSEMenuFramework::SetSection("Yacht");
		SKSEMenuFramework::AddSectionItem("Table", Table);
		SKSEMenuFramework::AddSectionItem("Play", Play);
		SKSEMenuFramework::AddSectionItem("Debug", Debug);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
