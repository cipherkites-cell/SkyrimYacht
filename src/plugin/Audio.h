// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// The table's sounds, played through our own XAudio2 voices (the game's sound system is paused with the game).
// The WAVs come from tools/make_sounds.py (Data\SKSE\Plugins\SkyrimYacht\Sounds), the same sounds as the preview.
namespace Audio
{
	struct Voice
	{
		std::string name;          // a sound in the bank: click_3, knock_metal_7, clang_dwemer_1, shout ...
		float       volume = 1.0f;
		float       pitch = 1.0f;  // frequency ratio
		float       lowpass = 0.0f;  // cutoff in Hz; 0 = none
		float       pan = 0.0f;      // -1 left .. 1 right
	};

	bool  Ready();
	void  Play(const Voice& a_voice);
	int   Count(const std::string& a_prefix);  // how many takes a sound has (click_ -> 13)
	int   Busy();                               // voices playing now
	void  SetRoll(float a_volume);              // the looped scuff of dice on the linen
	void  StopAll();
	void  SetMuted(bool a_muted);  // M on the table
	bool  Muted();
	double Now();                               // seconds, for spacing sounds out
}
