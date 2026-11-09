// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

struct IDXGISwapChain;

// The ImGui layer drawn over the game (Present hook), and the mouse and keys while a game is open. The same plumbing
// as Hearthsworn and Hearthframe: input arrives as the game's own events, is queued here, and is handed both to ImGui
// (the scorecards, the wager) and to the table (the cup and the dice).
namespace Overlay
{
	struct Press
	{
		float x = 0.0f, y = 0.0f;
		int   button = 0;
		bool  down = false;
		bool  onUi = false;  // over an ImGui window when it happened: the table ignores it
	};

	void SetOpen(bool a_open);  // the game opened or closed
	bool IsOpen();

	// For the table, on the main thread
	void               MousePos(float& a_x, float& a_y);
	std::vector<Press> TakePresses();
	std::vector<int>   TakeKeys();  // DirectInput scan codes pressed
	bool               ButtonHeld(int a_button);

	// An RGBA texture for ImGui (on the render thread, from DrawUi); null if it couldn't be made
	void* CreateTexture(int a_w, int a_h, const void* a_rgba);

	bool InstallInputSink();
	void OnPresent(IDXGISwapChain* a_swapChain);
	int  SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where);
	std::string ToUtf8(const std::wstring& a_w);
}
