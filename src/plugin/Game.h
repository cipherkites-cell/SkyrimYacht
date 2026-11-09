// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// The game: who you can play (seated NPCs near a table), the wager, the dice and cups on the table (the same rules,
// physics and opponent as the browser preview), the result, and what's remembered in the save.
namespace Game
{
	void OnDataLoaded();
	void OnGameLoaded();
	void FrameTick();          // every frame, main thread (tags seated NPCs, opens a game after the dialogue)
	void Tick(float a_dt);     // while a game is open, from its menu (main thread, game paused)
	void Abort();              // something failed: close the table, refund the wager
	void SetupUi(float a_screenH);
	void DrawUi();             // ImGui, inside Present

	// SKSE co-save
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc);
	void Revert(SKSE::SerializationInterface* a_intfc);
}
