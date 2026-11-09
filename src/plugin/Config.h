// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

// Data\SKSE\Plugins\SkyrimYacht.ini
namespace Config
{
	struct Values
	{
		// The table: drawn in the world in front of the camera (0), or in the game's 3D item view (1)
		int   mode = 0;
		float distance = 110.0f;    // world mode: how far in front of the camera (game units)
		float brightness = 1.0f;    // world mode: lifts the table out of a dark room (1 = as the room lights it)
		float tiltDeg = 55.0f;      // how far you look down on the tray
		float fitWidth = 0.72f;     // the whole table (both cups) across this much of the screen width
		float centerX = 0.50f;      // where its middle sits on screen, 0..1
		float centerY = 0.56f;
		float depth = 0.0f;         // > 0: distance from the camera instead of the game's own
		bool  flipTilt = false;     // if the tray shows from underneath, turn this on

		// Play
		int   minBet = 25;
		int   maxBet = 100;
		float tableReach = 180.0f;  // a seated NPC this close to a table can be asked to play
		float mouseSpeed = 1.0f;
		float uiScale = 1.0f;
		float volume = 1.0f;        // dice and table sounds
		bool  pauseGame = false;    // pause the world while you play (the world table only draws while it runs)

		// Diagnostics
		bool debugOverlay = false;  // draws the projected tray outline over the 3D, to check picking
		bool logInput = false;
	};

	const Values& Get();
	void          Load();
}
