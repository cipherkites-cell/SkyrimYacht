// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include "../sim/Math.h"

// The table, shown through the game's inventory 3D preview: the tray is loaded as an item (YA_Tray), the dice and the
// two cups hang under it as our own nodes, and the whole thing is tilted toward the camera. The game is paused and the
// world blurred behind it, as it is behind Skyrim's own menus. Positions are in table space: game units, z up, the
// felt at z = 0, the same as the physics.
namespace Scene
{
	void Register();  // kDataLoaded

	void Open();   // shows the menu (pauses, blurs) and loads the table
	void Close();
	bool IsOpen();
	bool Ready();  // the tray has loaded and the dice and cups are attached

	void SetDie(int a_index, const yacht::Vec3& a_pos, const yacht::Quat& a_rot, bool a_visible);
	void SetCup(int a_which, const yacht::Vec3& a_pos, const yacht::Quat& a_rot);  // 0 yours, 1 the opponent's
	void UseDice(const std::string& a_set);   // bone, iron, ... (falls back to bone)
	void UseCup(int a_which, const std::string& a_cupId);  // tankard, goblet, dwemer, silver, golden

	// Table space <-> screen pixels
	bool Project(const yacht::Vec3& a_p, float& a_sx, float& a_sy);
	bool Unproject(float a_sx, float a_sy, double a_planeZ, double& a_x, double& a_y);
	float PixelsPerUnit(const yacht::Vec3& a_at);

	// Called every frame by the menu (main thread), before the 3D draws
	void Update();
	void TeardownTick();  // finishes a close that had to wait for a model load
	std::string Status();  // for the log and the debug overlay
}
