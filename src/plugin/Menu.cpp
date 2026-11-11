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
			if (UI::Button("保存到 SkyrimYacht.ini")) {
				Config::Save();
				g_dirty = false;
			}
			UI::SameLine();
			if (UI::Button("撤销更改")) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("（尚未保存：更改仅在本次游戏期间生效）");
			}
		}

		void __stdcall Table()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("牌桌位置");
			Hint("游戏进行中修改会立即生效。");
			Changed(UI::SliderFloat("距离", &v.distance, 40.0f, 300.0f, "%.0f 游戏单位"));
			Hint("仅在房间内显示：牌桌位于镜头前方的距离。");
			Changed(UI::SliderFloat("屏幕尺寸", &v.fitWidth, 0.3f, 1.2f, "%.2f"));
			Hint("骰盘和两个骰杯占屏幕宽度的比例。");
			Changed(UI::SliderFloat("倾斜角度", &v.tiltDeg, 20.0f, 85.0f, "%.0f 度"));
			Hint("俯视牌桌的角度。");
			Changed(UI::SliderFloat("水平位置", &v.centerX, 0.2f, 0.8f, "%.2f"));
			Changed(UI::SliderFloat("垂直位置", &v.centerY, 0.3f, 0.8f, "%.2f"));
			Hint("牌桌中心在屏幕上的位置（0.5 为正中央）。");
			Changed(UI::Checkbox("反转倾斜方向", &v.flipTilt));
			Hint("如果你从下方看到了骰盘，请启用此项。");
			UI::SeparatorText("外观");
			Changed(UI::SliderFloat("亮度", &v.brightness, 1.0f, 4.0f, "%.1f"));
			Hint("仅在房间内显示：提高昏暗环境中牌桌的亮度。对下一次牌局生效。");
			int mode = v.mode;
			const char* modes[] = { "由模组绘制，覆盖在游戏画面上（推荐）", "在游戏物品预览界面中", "在房间内" };
			if (UI::Combo("绘制方式", &mode, modes, 3)) {
				v.mode = mode;
				g_dirty = true;
			}
			Hint("对下一次牌局生效。");
			SaveRow();
		}

		void __stdcall Play()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("下注");
			if (UI::SliderInt("最低下注", &v.minBet, 5, 500, "%d 金币")) {
				v.maxBet = (std::max)(v.maxBet, v.minBet);
				g_dirty = true;
			}
			if (UI::SliderInt("最高下注", &v.maxBet, 5, 2000, "%d 金币")) {
				v.minBet = (std::min)(v.minBet, v.maxBet);
				g_dirty = true;
			}
			UI::SeparatorText("可参与游戏的对象");
			Changed(UI::SliderFloat("牌桌检测范围", &v.tableReach, 60.0f, 400.0f, "%.0f 游戏单位"));
			Hint("坐着的 NPC 必须距离牌桌多近才会出现游戏邀请（70 大约为一米）。");
			UI::SeparatorText("控制与声音");
			Changed(UI::SliderFloat("鼠标速度", &v.mouseSpeed, 0.2f, 3.0f, "%.2f"));
			Changed(UI::SliderFloat("音量", &v.volume, 0.0f, 2.0f, "%.2f"));
			Hint("骰子、骰杯和牌桌的声音。");
			Changed(UI::SliderFloat("不卸之力", &v.shoutVolume, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat("音效", &v.effectsVolume, 0.0f, 2.0f, "%.2f"));
			Hint("菜单点击、金币和羽毛笔等音效。");
			Changed(UI::SliderFloat("音乐", &v.musicVolume, 0.0f, 1.0f, "%.2f"));
			Hint("游戏期间播放的酒馆音乐（设为 0 可关闭）。");
			Changed(UI::SliderFloat("界面大小", &v.uiScale, 0.6f, 1.6f, "%.2f"));
			Hint("牌桌计分卡、按钮和文字的大小。修改后立即生效。");
			Changed(UI::Checkbox("游戏时暂停世界", &v.pauseGame));
			Hint("仅在房间内显示时有效（其他显示方式始终会暂停游戏）。");
			SaveRow();
		}

		void __stdcall Debug()
		{
			auto& v = Config::Edit();
			Changed(UI::Checkbox("显示骰盘轮廓及鼠标可点击的骰子", &v.debugOverlay));
			Hint("用于检查鼠标点击位置是否与牌桌上的物体对应。");
			SaveRow();
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in SkyrimYacht.ini");
			return;
		}
		SKSEMenuFramework::SetSection("快艇骰子");
		SKSEMenuFramework::AddSectionItem("牌桌", Table);
		SKSEMenuFramework::AddSectionItem("游戏", Play);
		SKSEMenuFramework::AddSectionItem("调试", Debug);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
