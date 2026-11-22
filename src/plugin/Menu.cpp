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
			if (UI::Button("撤销改动")) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("（未保存：改动在退出前有效）");
			}
		}

		void __stdcall Table()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("牌桌位置");
			Hint("牌局打开时改动立即生效。");
			Changed(UI::SliderFloat("距离", &v.distance, 40.0f, 300.0f, "%.0f units"));
			Hint("仅房间模式：牌桌在镜头前方多远。");
			Changed(UI::SliderFloat("屏幕占比", &v.fitWidth, 0.3f, 1.2f, "%.2f"));
			Hint("托盘与两只骰盅占屏幕宽度的比例。");
			Changed(UI::SliderFloat("俯角", &v.tiltDeg, 20.0f, 85.0f, "%.0f degrees"));
			Hint("你俯视牌桌的角度。");
			Changed(UI::SliderFloat("左右", &v.centerX, 0.2f, 0.8f, "%.2f"));
			Changed(UI::SliderFloat("上下", &v.centerY, 0.3f, 0.8f, "%.2f"));
			Hint("牌桌中心在屏幕上的位置（0.5 为正中）。");
			Changed(UI::Checkbox("翻转俯角", &v.flipTilt));
			Hint("从下往上看托盘时打开。");
			UI::SeparatorText("画面");
			Changed(UI::SliderFloat("亮度", &v.brightness, 1.0f, 4.0f, "%.1f"));
			Hint("仅房间模式：把牌桌从暗处提亮。对下一张牌桌生效。");
			int mode = v.mode;
			const char* modes[] = { "由插件绘制，覆盖在画面上（推荐）", "绘制在游戏的物品预览里", "放置到房间中" };
			if (UI::Combo("绘制方式", &mode, modes, 3)) {
				v.mode = mode;
				g_dirty = true;
			}
			Hint("对下一张牌桌生效。");
			SaveRow();
		}

		void __stdcall Play()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("赌注");
			if (UI::SliderInt("最小赌注", &v.minBet, 5, 500, "%d 金币")) {
				v.maxBet = (std::max)(v.maxBet, v.minBet);
				g_dirty = true;
			}
			if (UI::SliderInt("最大赌注", &v.maxBet, 5, 2000, "%d 金币")) {
				v.minBet = (std::min)(v.minBet, v.maxBet);
				g_dirty = true;
			}
			UI::SeparatorText("可对局的人");
			Changed(UI::SliderFloat("牌桌范围", &v.tableReach, 60.0f, 400.0f, "%.0f units"));
			Hint("坐着的人离牌桌多近才会被邀请（70 约等于一米）。");
			UI::SeparatorText("操作与声音");
			Changed(UI::SliderFloat("鼠标速度", &v.mouseSpeed, 0.2f, 3.0f, "%.2f"));
			Changed(UI::SliderFloat("音量", &v.volume, 0.0f, 2.0f, "%.2f"));
			Hint("骰子、骰盅与牌桌。");
			Changed(UI::SliderFloat("Fus Ro Dah", &v.shoutVolume, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat("音效", &v.effectsVolume, 0.0f, 2.0f, "%.2f"));
			Hint("菜单点击、钱币、羽毛笔。");
			Changed(UI::SliderFloat("音乐", &v.musicVolume, 0.0f, 1.0f, "%.2f"));
			Hint("对局时的酒馆音乐（0 为关闭）。");
			Changed(UI::SliderFloat("界面大小", &v.uiScale, 0.6f, 1.6f, "%.2f"));
			Hint("牌桌的记分卡、按钮与文字。立即生效。");
			Changed(UI::Checkbox("对局时暂停世界", &v.pauseGame));
			Hint("仅房间模式（其他方式总是暂停）。");
			SaveRow();
		}

		void __stdcall Debug()
		{
			auto& v = Config::Edit();
			Changed(UI::Checkbox("显示托盘轮廓与鼠标可点中的骰子", &v.debugOverlay));
			Hint("用来检查点击是否与牌桌对齐。");
			SaveRow();
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in SkyrimYacht.ini");
			return;
		}
		SKSEMenuFramework::SetSection("游艇");
		SKSEMenuFramework::AddSectionItem("牌桌", Table);
		SKSEMenuFramework::AddSectionItem("玩法", Play);
		SKSEMenuFramework::AddSectionItem("调试", Debug);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
