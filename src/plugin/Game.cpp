// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Game.h"

#include "Audio.h"
#include "Config.h"
#include "EspIds.h"
#include "Overlay.h"
#include "Scene.h"

#include "../sim/Ai.h"
#include "../sim/Dice.h"
#include "../sim/Yacht.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <set>

#undef GetObject  // wingdi.h turns it into GetObjectA, breaking BSScript::Variable::GetObject

// The topic info event (not in CommonLibSSE-NG 3.7): speaker at 0x00, topic info form ID at 0x10, 0 = begin / 1 = end.
namespace RE
{
	struct TESTopicInfoEvent
	{
		NiPointer<TESObjectREFR> speakerRef;       // 00
		void*                    callback;         // 08
		FormID                   topicInfoFormID;  // 10
		std::uint32_t            type;             // 14
	};
}

namespace Game
{
	namespace
	{
		using yacht::Quat;
		using yacht::Vec3;

		// ---------------------------------------------------------------- the table's layout (game units, as the preview)
		constexpr double kX = 12.65, kYh = 8.6, kR = 1.9, kCupWall = 0.22, kHalf = 0.6, kWallT = 0.8;
		const Vec3       kRest{ kX + kWallT + 3.6, -3.5, 3.0 };
		const Vec3       kNpcRest{ -(kX + kWallT + 3.6), 0.5, 3.0 };
		constexpr double kShakeZ = 5.6, kCarryZ = 3.2;
		constexpr double kRackY = -kYh - kWallT - 1.7, kNpcRackY = -kRackY, kRackTop = 0.0;
		double           RackX(int a_slot) { return (a_slot - 2) * 2.4; }

		struct CupInfo
		{
			const char* id;
			const char* name;
			double      height;  // inside depth (the vanilla meshes, scaled)
			const char* sound;   // whose clang it makes in the hand (tankard, goblet, dwemer, silver, golden)
		};
		constexpr CupInfo kCups[] = {
			{ "tankard", "Tankard", 4.21, "tankard" },
			{ "bloodtankard", "Bloody Tankard", 4.21, "tankard" },
			{ "glazedcup", "Glazed Cup", 4.68, "goblet" },
			{ "goblet", "Glazed Goblet", 4.69, "goblet" },
			{ "dwemer", "Dwemer Cup", 5.10, "dwemer" },
			{ "dwemer2", "Ornate Dwemer Cup", 5.15, "dwemer" },
			{ "silver", "Silver Goblet", 4.62, "silver" },
			{ "silver2", "Silver Chalice", 4.39, "silver" },
		};
		constexpr int kNumCups = static_cast<int>(std::size(kCups));
		constexpr const char* kDiceSets[] = { "bone", "iron", "steel", "corundum", "dwemer", "orichalcum", "silver", "quicksilver", "moonstone",
			"gold", "malachite", "ebony", "stalhrim", "dragonbone", "daedric" };

		enum class Phase
		{
			Idle,
			Wager,
			Ready,
			Shaking,
			Pouring,
			Rolling,
			Redo,
			Choose,
			Scored,
			Over,
			Shop
		};

		struct Cup
		{
			Vec3 p, v;
			Quat q;
			Vec3 rest;
		};

		struct Tween
		{
			bool   on = false;
			Vec3   from;
			Quat   fromQ;
			double t = 0, dur = 0.25, arc = 1.2;
			Vec3   spin;
			bool   spins = false;
		};

		struct Record
		{
			std::uint32_t w = 0, l = 0, d = 0;
		};

		enum class NpcStep
		{
			Start,
			Shake,
			Throw,
			Think,
			Move,
			Score,
			Done
		};

		enum class ActKind
		{
			Keep,
			ToCup
		};

		// ---------------------------------------------------------------- state (g_mtx: Tick and DrawUi)
		std::recursive_mutex g_mtx;
		yacht::DiceSim       g_sim({}, 1234u);
		yacht::Scorecard     g_cards[2];
		yacht::Opponent      g_ai(77u);
		std::mt19937         g_rng{ 42u };

		Phase  g_phase = Phase::Idle;
		int    g_turn = 0;  // 0 you, 1 the opponent
		Cup    g_cups[2];
		Vec3   g_cupTarget{ kRest.x, kRest.y, 0 };
		int    g_faces[5]{};
		bool   g_kept[5]{};
		int    g_rackSlot[5] = { -1, -1, -1, -1, -1 };
		std::optional<std::pair<double, double>> g_lastSpot[5];
		Tween  g_anim[5];
		Vec3   g_drawPos[5];
		Quat   g_drawRot[5];
		int    g_rollsUsed = 0;
		unsigned g_rolled = 0;
		double g_rollingTime = 0, g_shakeTime = 0, g_redoTime = 0, g_scoredTime = 0;
		bool   g_pourPending = false;
		double g_aimX = -1, g_aimY = 0.2;
		struct
		{
			bool   on = false;
			double t = 0;
			Vec3   at;
		} g_pour;
		struct
		{
			bool   on = false;
			Vec3   axis;
			double angle = 0, vel = 0;
			Quat   lean0;
		} g_tilt;
		struct
		{
			bool  on = false;
			bool  cup = false;
			int   die = -1;
			float x0 = 0, y0 = 0;
			bool  moved = false;
		} g_press;
		struct
		{
			int    die = -1;
			double x = 0, y = 0, z = kCarryZ;
			int    from = 0;  // 0 pad, 1 rack, 2 cup
		} g_carry;
		struct
		{
			NpcStep                                 step = NpcStep::Start;
			double                                  t = 0, shakeFor = 1.2;
			std::deque<std::pair<ActKind, int>>     queue;
		} g_npc;

		// the opponent and the wager
		RE::ActorHandle g_opponent;
		std::string     g_oppName = "Your opponent";
		RE::FormID      g_oppId = 0;
		int             g_bet = 25;
		int             g_betHeld = 0;  // taken from you, not yet settled
		std::string     g_outcome;      // "win", "lose", "draw", "forfeit"
		std::string     g_payoutText;
		bool            g_confirmForfeit = false;
		std::string     g_toast;
		double          g_toastTime = 0;
		double          g_clock = 0;
		double          g_shoutAt = -1;      // the Shout started (the dice go on "Dah")
		double          g_shoutReady = 0;    // the Thu'um has recharged
		constexpr double kShoutCooldown = 4.0;
		constexpr double kWordAt[3] = { 0.0, 0.56, 0.98 };  // FUS, RO, DAH
		bool            g_help = false;
		bool            g_soundMenu = false;  // the little volume panel over the Sound key
		ImVec2          g_soundKey{};         // that key's top middle, to hang the panel from
		double          g_fillAt[2][yacht::kCategories]{};  // when each box was written in (the ink runs in)
		double          g_turnAt = 0;        // when the turn changed (the compass label settles)

		// remembered in the save
		std::unordered_map<RE::FormID, Record> g_records;
		int g_lastScore = 0;
		int g_totalWins = 0;
		int g_bestScore = 0;            // best finished game: unlocks the cups
		int g_games = 0;                // games finished (the innkeepers' line shows after the first)
		std::uint32_t g_ownedDice = 1;  // bit per set in kDiceSets; bone is always yours
		std::string   g_cupSel, g_diceSel;  // what you chose to play with ("" = the best you have)

		// UI -> game
		enum class UiAct
		{
			PlaceBet,
			Leave,
			Score,
			Rematch,
			Forfeit,
			ForfeitYes,
			ForfeitNo,
			Shout,
			ChooseCup,
			ChooseDice,
			BuyDice,
			CloseShop
		};
		std::vector<std::pair<UiAct, int>> g_uiActs;
		std::mutex                         g_uiMtx;
		void Post(UiAct a_act, int a_arg = 0)
		{
			std::lock_guard lk(g_uiMtx);
			g_uiActs.emplace_back(a_act, a_arg);
		}

		// forms
		RE::TESFaction*  g_faction = nullptr;
		RE::TESGlobal*   g_canAfford = nullptr;
		RE::FormID       g_infoPlay = 0;
		RE::FormID       g_infoDiceSale = 0;
		RE::TESObjectMISC* g_diceItem[15]{};  // the sets as items in the innkeepers' shops (1..14; bone is everyone's)
		RE::TESFaction*    g_innkeeper = nullptr;
		std::string      g_shopName;
		RE::TESGlobal*   g_diceShop = nullptr;
		std::atomic<RE::FormID> g_pendingShop{ 0 };
		RE::TESBoundObject* g_gold = nullptr;
		std::atomic<RE::FormID> g_pendingSpeaker{ 0 };
		std::unordered_map<RE::FormID, bool> g_tagged;

		double Rand() { return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng); }
		double Clamp(double v, double a, double b) { return v < a ? a : v > b ? b : v; }
		double Ease(double t) { return t * t * (3 - 2 * t); }

		void Toast(std::string a_text)
		{
			g_toast = std::move(a_text);
			g_toastTime = g_clock;
		}

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }
		// (not Actor::GetGoldAmount: it goes through the default-object table, which crashed on 1.5.97)
		int GoldHeld()
		{
			auto* p = Player();
			if (!p || !g_gold) {
				return 0;
			}
			const auto inv = p->GetInventory([](RE::TESBoundObject& a_obj) { return a_obj.GetFormID() == 0x0000000F; });
			for (const auto& [obj, data] : inv) {
				return data.first;
			}
			return 0;
		}
		void Sound(const char* a_id) { RE::PlaySound(a_id); }

		// ---------------------------------------------------------------- the table's sounds (as the preview's)
		// Each dice set's material: bone, stone and stalhrim play the recordings almost as they are; metal is pitched down
		// with the metal tone; gold and silver heavier still.
		const char* DiceMaterial(const std::string& a_set)
		{
			static const std::unordered_map<std::string, const char*> kMat = { { "bone", "bone" }, { "dragonbone", "bone" },
				{ "iron", "metal" }, { "steel", "metal" }, { "corundum", "metal" }, { "dwemer", "metal" }, { "orichalcum", "metal" },
				{ "ebony", "metal" }, { "daedric", "metal" }, { "silver", "precious" }, { "quicksilver", "precious" }, { "gold", "precious" },
				{ "malachite", "stone" }, { "moonstone", "stone" }, { "stalhrim", "ice" } };
			const auto it = kMat.find(a_set);
			return it == kMat.end() ? "bone" : it->second;
		}
		float MaterialPitch(const char* a_mat)
		{
			const std::string m = a_mat;
			return m == "metal" ? 0.88f : m == "precious" ? 0.84f : m == "stone" ? 0.95f : m == "ice" ? 1.15f : 1.0f;
		}
		constexpr float kDieTune[5] = { 0.97f, 1.0f, 1.035f, 0.985f, 1.02f };  // every die has its own voice
		constexpr float kSurfGain[4] = { 0.3f, 0.55f, 0.6f, 0.35f };            // die-die, linen, tray wall, cup
		constexpr float kClangLevel[5] = { 0.2f, 0.17f, 0.22f, 0.17f, 0.2f };  // tankard, goblet, dwemer, silver, golden
		double g_lastHit[24];
		double g_burstT = -1, g_lastClang = -1;
		int    g_burstN = 0;
		std::string g_soundSet = "bone";  // the dice on the table
		std::string g_soundCup = "tankard";

		float Rand01() { return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); }
		int   Pick(const std::string& a_prefix)
		{
			const int n = Audio::Count(a_prefix);
			return n > 0 ? std::rand() % n : 0;
		}

		void Ui(const char* a_name, float a_vol = 1.0f) { Audio::Play({ a_name, a_vol }); }

		// type: 0 die-die, 1 linen, 2 tray wall, 3 cup. die: which die (its pitch, where it is), or -1. inCup: the hit is
		// inside the cup (shaking).
		void Hit(int a_type, double a_speed, int a_die, bool a_inCup, double a_x)
		{
			if (Audio::Busy() > 20) {
				return;
			}
			const float v = static_cast<float>(std::clamp((a_speed - 6.0) / 95.0, 0.0, 1.0));
			if (v < 0.04f) {
				return;  // the little taps of a settling die stay silent
			}
			const double now = Audio::Now();
			if (a_die >= 0) {
				const int k = a_die * 4 + a_type;
				if (now - g_lastHit[k] < 0.05) {
					return;
				}
				g_lastHit[k] = now;
			}
			// a few hits in any 40 ms at most: five dice in a cup report far more contacts than an ear can tell apart
			if (now - g_burstT > 0.04) {
				g_burstT = now;
				g_burstN = 0;
			}
			if (++g_burstN > 3) {
				return;
			}
			const char*       mat = DiceMaterial(g_soundSet);
			const std::string m = mat;
			const std::string tone = m == "metal" ? "metal_" : m == "precious" ? "precious_" : "";
			const bool        metal = !tone.empty();
			// dice on dice and in the cup click; dice landing on the tray knock
			const std::string kind = (a_type == 0 || a_type == 3) ? "click_" : "knock_";
			const float pitch = MaterialPitch(mat) * (a_die >= 0 ? kDieTune[a_die] : 1.0f) * (1.0f + (Rand01() - 0.5f) * 0.08f);
			float       lp = a_type == 1 ? 1400.0f + 3000.0f * v : 2500.0f + 12000.0f * v * v;  // the linen damps the knock
			const float gain = std::pow(v, 1.6f) * kSurfGain[a_type];
			const float pan = static_cast<float>(std::clamp(a_x / (kX + 6.0), -1.0, 1.0) * 0.5);
			const bool  inside = a_type == 3 || (a_type == 0 && a_inCup);
			// shaking: the cup clangs a little with each hit, in its own voice (once, even when several dice hit it)
			if (inside && v > 0.08f && now - g_lastClang > 0.07) {
				g_lastClang = now;
				int ci = 0;
				const char* banks[5] = { "tankard", "goblet", "dwemer", "silver", "golden" };
				for (int c = 0; c < 5; ++c) {
					if (g_soundCup == banks[c]) {
						ci = c;
					}
				}
				Audio::Play({ "clang_" + g_soundCup + "_" + std::to_string(std::rand() % 4), kClangLevel[ci] * (a_type == 0 ? 0.6f : 1.0f) * gain,
					1.0f + (Rand01() - 0.5f) * 0.04f, 0.0f, pan });
			}
			if (metal && inside) {
				// in the cup a metal die is a dull clunk, not a click: a knock under it gives it its weight
				lp = (std::min)(lp, 2200.0f + 4500.0f * v);
				Audio::Play({ "knock_" + tone + std::to_string(Pick("knock_" + tone)), gain * 0.8f, pitch * 0.95f, lp, pan });
			}
			Audio::Play({ kind + tone + std::to_string(Pick(kind + tone)), gain, pitch, lp, pan });
		}

		// Dice sliding and rolling on the linen: a soft scuff under the knocks.
		Vec3 g_prevPos[5];
		bool g_bedsPrimed = false;
		void UpdateRoll(double a_dt);

		// A few sounds that come a moment after something (the dice dropping back into the cup after the Shout).
		struct Later
		{
			double at;
			int    type, die;
			double speed;
		};
		std::vector<Later> g_later;

		const CupInfo& CupById(const std::string& a_id)
		{
			for (const auto& c : kCups) {
				if (a_id == c.id) {
					return c;
				}
			}
			return kCups[0];
		}
		// the dice sets as the innkeepers sell them: a new set every ten wins, for gold
		struct DiceInfo
		{
			const char* name;
			int         wins, gold;
			const char* blurb;
		};
		constexpr DiceInfo kDiceInfo[15] = {
			{ "Bone", 0, 0, "Hand-carved bone. Yellowed, scratched, and dirty in the pips." },
			{ "Iron", 10, 50, "Hammered iron, pocked and spotted with rust, punched pips." },
			{ "Steel", 20, 100, "Brushed steel with crisp engraved pips." },
			{ "Corundum", 30, 150, "Coppery corundum, a little green at the edges." },
			{ "Dwemer", 40, 200, "Dwarven bronze with gear-ring pips, green with age." },
			{ "Orichalcum", 50, 250, "Orc-forged orichalcum, dark green-bronze, scored with claw marks." },
			{ "Silver", 60, 300, "Polished silver, tarnished in the corners, black niello pips." },
			{ "Quicksilver", 70, 400, "Mirror-bright quicksilver that looks wet." },
			{ "Moonstone", 80, 500, "Pale elven moonstone with fine leaf lines." },
			{ "Gold", 90, 650, "Solid gold with garnet pips." },
			{ "Malachite", 100, 750, "Polished glass-green malachite, banded, with white pips." },
			{ "Ebony", 110, 900, "Black ebony with inlaid gold pips." },
			{ "Stalhrim", 120, 1100, "Enchanted ice from Solstheim. Cold blue, cracked, faintly glowing." },
			{ "Dragonbone", 130, 1500, "Pale dragon bone, fibrous grain, pips stained dark red." },
			{ "Daedric", 140, 2500, "Black and red, cut with runes, the pips smouldering like coals." },
		};
		// a cup is yours to play with while you carry one (the plain tankard always is)
		bool CupUnlocked(int k) { return k == 0 || (k > 0 && k < kNumCups && Scene::OwnsCup(kCups[k].id)); }
		bool DiceOwned(int k) { return k >= 0 && k < 15 && (k == 0 || (g_ownedDice >> k) & 1u); }
		bool DiceOnSale(int k) { return k > 0 && k < 15 && !DiceOwned(k) && g_totalWins >= kDiceInfo[k].wins; }
		int  DiceIndex(const std::string& a_set)
		{
			for (int k = 0; k < 15; ++k) {
				if (a_set == kDiceSets[k]) {
					return k;
				}
			}
			return -1;
		}
		bool AnyDiceOnSale()
		{
			for (int k = 1; k < 15; ++k) {
				if (DiceOnSale(k)) {
					return true;
				}
			}
			return false;
		}
		const CupInfo& PlayerCup()
		{
			for (int k = 0; k < kNumCups; ++k) {
				if (g_cupSel == kCups[k].id && CupUnlocked(k)) {
					return kCups[k];
				}
			}
			return kCups[0];
		}
		std::string PlayerDice()
		{
			const int k = DiceIndex(g_diceSel);
			if (DiceOwned(k)) {
				return kDiceSets[k];
			}
			for (int j = 14; j >= 0; --j) {  // the finest you own
				if (DiceOwned(j)) {
					return kDiceSets[j];
				}
			}
			return "bone";
		}
		double ActiveCupH() { return g_turn == 0 ? PlayerCup().height : kCups[0].height; }

		// ---------------------------------------------------------------- the dice
		bool Active(int i) { return g_sim.DieAt(i).state == yacht::DieState::kActive; }
		bool InCup(int i) { return g_sim.InCup(i); }
		bool DiceOnPad()
		{
			for (int i = 0; i < 5; ++i) {
				if (Active(i) && !InCup(i)) {
					return true;
				}
			}
			return false;
		}
		bool AnyInCup() { return g_sim.AnyInCup(); }
		bool CanRoll()
		{
			return (g_phase == Phase::Ready || g_phase == Phase::Choose) && g_rollsUsed < 3 && g_carry.die < 0 && AnyInCup() && !DiceOnPad();
		}

		void TweenFrom(int i, double a_dur, double a_arc = 1.2)
		{
			g_anim[i] = { true, g_drawPos[i], g_drawRot[i], 0.0, a_dur, a_arc, {}, false };
		}
		int FreeSlot()
		{
			for (int k = 0; k < 5; ++k) {
				bool used = false;
				for (int j = 0; j < 5; ++j) {
					used |= g_rackSlot[j] == k;
				}
				if (!used) {
					return k;
				}
			}
			return 0;
		}

		void StartTurn(int a_owner)
		{
			g_turn = a_owner;
			g_turnAt = g_clock;
			Scene::UseDice(a_owner == 0 ? PlayerDice() : "bone");
			g_soundSet = a_owner == 0 ? PlayerDice() : "bone";
			auto& c = g_cups[a_owner];
			c.p = c.rest;
			c.v = {};
			c.q = {};
			g_tilt.on = false;
			g_pour.on = false;
			g_sim.PlaceCup(c.p, c.q);
			g_sim.SetCupHeight(ActiveCupH());
			g_npc = {};
			for (int i = 0; i < 5; ++i) {
				g_sim.SetFrozen(i, false);
				TweenFrom(i, 0.3);
				g_faces[i] = 0;
				g_kept[i] = false;
				g_rackSlot[i] = -1;
				g_lastSpot[i].reset();
			}
			g_carry.die = -1;
			g_sim.LoadCup(0x1F);
			g_rollsUsed = 0;
			g_phase = Phase::Ready;
			g_cupTarget = { c.rest.x, c.rest.y, 0 };
		}

		void Keep(int i)
		{
			if (Active(i) && !InCup(i)) {
				g_lastSpot[i] = std::make_pair(g_sim.DieAt(i).pos.x, g_sim.DieAt(i).pos.y);
			}
			TweenFrom(i, 0.25);
			g_sim.SetActive(i, false);
			g_kept[i] = true;
			g_rackSlot[i] = FreeSlot();
			Hit(2, 30, i, false, RackX(g_rackSlot[i] < 0 ? 2 : g_rackSlot[i]));  // set down on the rack
		}
		void PlaceOnPad(int i, double x, double y, bool a_drop)
		{
			TweenFrom(i, a_drop ? 0.12 : 0.25);
			g_kept[i] = false;
			g_rackSlot[i] = -1;
			g_sim.PlaceDie(i, { Clamp(x, -kX + kHalf, kX - kHalf), Clamp(y, -kYh + kHalf, kYh - kHalf), kHalf + (a_drop ? 0.5 : 0.02) },
				g_faces[i] ? g_faces[i] : 1);
			Hit(1, 34, i, false, x);  // onto the linen
		}
		void PutInCup(int i)
		{
			TweenFrom(i, 0.18);
			g_kept[i] = false;
			g_rackSlot[i] = -1;
			g_sim.AddToCup(i);
			Hit(3, 32, i, true, g_cups[0].p.x);  // into the cup
		}
		std::pair<double, double> FrontSpot(int i)
		{
			if (g_lastSpot[i]) {
				return *g_lastSpot[i];
			}
			return { g_kept[i] ? RackX(g_rackSlot[i]) : kX - 2, -kYh + 1.4 };
		}
		void ClickDie(int i)
		{
			if (g_phase != Phase::Choose) {
				return;
			}
			if (g_kept[i]) {
				const auto sp = FrontSpot(i);
				PlaceOnPad(i, sp.first, sp.second, false);
			} else {
				Keep(i);
			}
		}

		bool BeginShake()
		{
			if (!CanRoll()) {
				return false;
			}
			g_rolled = 0;
			for (int i = 0; i < 5; ++i) {
				if (InCup(i)) {
					g_rolled |= 1u << i;
				} else if (Active(i)) {
					g_sim.SetFrozen(i, true);
				}
			}
			g_phase = Phase::Shaking;
			g_shakeTime = 0;
			g_pourPending = false;
			g_sim.SetLid(true);
			auto&        c = g_cups[g_turn];
			const double l = std::hypot(c.p.x, c.p.y);
			g_aimX = l > 0 ? -c.p.x / l : -1;
			g_aimY = l > 0 ? -c.p.y / l : 0;
			return true;
		}

		void BeginPour()
		{
			if (g_phase != Phase::Shaking) {
				return;
			}
			g_sim.SetLid(false);
			auto&        c = g_cups[g_turn];
			const double len = std::hypot(g_aimX, g_aimY);
			const double dx = len > 0 ? g_aimX / len : -1, dy = len > 0 ? g_aimY / len : 0;
			const double reach = ActiveCupH() * 0.85;
			const double px = Clamp(c.p.x, -kX + 2, kX - 2), py = Clamp(c.p.y, -kYh + 2, kYh - 2);
			const double tipX = Clamp(px + dx * reach, -kX + 2.6, kX - 2.6) - dx * reach;
			const double tipY = Clamp(py + dy * reach, -kYh + 2.6, kYh - 2.6) - dy * reach;
			g_pour = { true, 0.0, { tipX, tipY, kShakeZ - 0.4 } };
			g_tilt.on = true;
			g_tilt.axis = Vec3{ -dy, dx, 0 }.Normalized();
			g_tilt.angle = 0;
			g_tilt.vel = 10;
			g_tilt.lean0 = c.q;
			c.v.x += dx * 22;
			c.v.y += dy * 22;
			g_phase = Phase::Pouring;
			Ui("swoosh", 0.25f);
		}

		void RedoThrow()
		{
			g_phase = Phase::Redo;
			g_redoTime = 0;
			Toast("Off the table! Throw again");
		}

		void FinishRoll()
		{
			for (int i = 0; i < 5; ++i) {
				if (g_rolled & (1u << i)) {
					g_faces[i] = g_sim.TopFace(i);
				}
				if (Active(i)) {
					g_sim.SetFrozen(i, false);
				}
			}
			++g_rollsUsed;
			g_phase = Phase::Choose;
		}

		void Dice(int (&a_out)[5])
		{
			for (int i = 0; i < 5; ++i) {
				a_out[i] = g_faces[i] ? g_faces[i] : 1;
			}
		}

		// ---------------------------------------------------------------- game over and the wager
		void Settle(const std::string& a_outcome)
		{
			const int mine = g_cards[0].Total(), theirs = g_cards[1].Total();
			std::string outcome = a_outcome;
			if (outcome.empty()) {
				outcome = mine > theirs ? "win" : mine < theirs ? "lose" : "draw";
			}
			g_outcome = outcome;
			g_phase = Phase::Over;
			g_confirmForfeit = false;
			g_sim.SetLid(false);
			auto& r = g_records[g_oppId];
			if (outcome == "win") {
				++r.w;
				++g_totalWins;
				if (g_gold && Player() && g_betHeld > 0) {
					Player()->AddObjectToContainer(g_gold, nullptr, g_betHeld * 2, nullptr);
				}
				g_payoutText = fmt::format("You take {} septims", g_betHeld * 2);
				Ui("coins", 0.9f);
				if (g_totalWins % 10 == 0 && g_totalWins / 10 < 15) {
					Toast(fmt::format("Innkeepers now sell {} dice", kDiceInfo[g_totalWins / 10].name));
				}
			} else if (outcome == "draw") {
				++r.d;
				if (g_gold && Player() && g_betHeld > 0) {
					Player()->AddObjectToContainer(g_gold, nullptr, g_betHeld, nullptr);
				}
				g_payoutText = fmt::format("Your {} septims come back to you", g_betHeld);
			} else {
				++r.l;
				g_payoutText = fmt::format("{} keeps your {} septims", g_oppName, g_betHeld);
			}
			g_betHeld = 0;
			g_lastScore = outcome == "forfeit" ? 0 : mine;
			++g_games;
			if (outcome != "forfeit" && mine > g_bestScore) {
				g_bestScore = mine;
			}
			spdlog::info("Game over vs {}: {} ({} - {}), record {}-{}-{}", g_oppName, outcome, mine, theirs, r.w, r.l, r.d);
		}

		void Refund()
		{
			if (g_betHeld > 0 && g_gold && Player()) {
				Player()->AddObjectToContainer(g_gold, nullptr, g_betHeld, nullptr);
			}
			g_betHeld = 0;
		}

		void NewGame()
		{
			g_cards[0].Clear();
			g_cards[1].Clear();
			g_outcome.clear();
			g_confirmForfeit = false;
			Scene::UseCup(0, PlayerCup().id);
			Scene::UseCup(1, "tankard");
			g_cups[0].rest = kRest;
			g_cups[1].rest = kNpcRest;
			for (int k = 0; k < 2; ++k) {
				g_cups[k].p = g_cups[k].rest;
				g_cups[k].v = {};
				g_cups[k].q = {};
			}
			StartTurn(0);
		}

		void CloseGame()
		{
			Refund();
			g_phase = Phase::Idle;
			Scene::Close();
			Audio::StopAll();
			g_later.clear();
			g_bedsPrimed = false;
			spdlog::info("Left the table");
		}

		// ---------------------------------------------------------------- the opponent's turn, played on the table
		void NpcTick(double dt)
		{
			if (g_turn != 1 || g_phase == Phase::Over) {
				return;
			}
			g_npc.t += dt;
			switch (g_npc.step) {
			case NpcStep::Start:
				if (g_npc.t > 0.9 && BeginShake()) {
					g_npc.step = NpcStep::Shake;
					g_npc.t = 0;
					g_npc.shakeFor = 1.0 + Rand() * 0.6;
				}
				break;
			case NpcStep::Shake:
				{
					const double a = g_npc.t * 17;
					g_cupTarget.x = -kX * 0.4 + std::cos(a) * 1.5 + std::sin(a * 0.37) * 0.8;
					g_cupTarget.y = kYh * 0.25 + std::sin(a * 1.3) * 1.2;
					if (g_npc.t > g_npc.shakeFor) {
						const double d = std::hypot(1.0, -0.35);
						g_aimX = 1 / d;
						g_aimY = -0.35 / d;
						g_pourPending = true;
						g_npc.step = NpcStep::Throw;
					}
				}
				break;
			case NpcStep::Throw:
				if (g_phase == Phase::Choose) {
					g_npc.step = NpcStep::Think;
					g_npc.t = 0;
				}
				break;
			case NpcStep::Think:
				if (g_npc.t > 0.8) {
					int dice[5];
					Dice(dice);
					const unsigned mask = g_rollsUsed >= 3 ? 0x1Fu : g_ai.ChooseKeep(dice, 3 - g_rollsUsed, g_cards[1]);
					if (mask == 0x1F) {
						g_npc.step = NpcStep::Score;
						g_npc.t = 0;
						break;
					}
					for (int i = 0; i < 5; ++i) {
						const bool keepIt = (mask & (1u << i)) != 0;
						if (keepIt && !g_kept[i]) {
							g_npc.queue.emplace_back(ActKind::Keep, i);
						}
						if (!keepIt && (g_kept[i] || (Active(i) && !InCup(i)))) {
							g_npc.queue.emplace_back(ActKind::ToCup, i);
						}
					}
					g_npc.step = NpcStep::Move;
					g_npc.t = 0;
				}
				break;
			case NpcStep::Move:
				if (g_npc.t > 0.3) {
					g_npc.t = 0;
					if (!g_npc.queue.empty()) {
						const auto [kind, i] = g_npc.queue.front();
						g_npc.queue.pop_front();
						if (kind == ActKind::Keep) {
							Keep(i);
						} else {
							PutInCup(i);
						}
					} else {
						g_npc.step = NpcStep::Start;
						g_npc.t = 0.3;
					}
				}
				break;
			case NpcStep::Score:
				if (g_npc.t > 0.5) {
					int dice[5];
					Dice(dice);
					const int cat = g_ai.ChooseCategory(dice, g_cards[1]);
					const int pts = g_cards[1].Fill(cat, dice);
					g_fillAt[1][cat] = g_clock;
					Toast(fmt::format("{} scores {}: {}", g_oppName, yacht::CategoryName(cat), pts));
					Ui("quill");
					g_npc.step = NpcStep::Done;
					g_npc.t = 0;
					g_phase = Phase::Scored;
				}
				break;
			case NpcStep::Done:
				if (g_npc.t > 1.6) {
					if (g_cards[0].Full() && g_cards[1].Full()) {
						Settle("");
					} else {
						StartTurn(0);
					}
				}
				break;
			}
		}

		void ScoreBox(int a_cat)
		{
			if (g_phase != Phase::Choose || g_carry.die >= 0 || g_turn != 0) {
				return;
			}
			int dice[5];
			Dice(dice);
			const int pts = g_cards[0].Fill(a_cat, dice);
			g_fillAt[0][a_cat] = g_clock;
			if (pts < 0) {
				return;
			}
			Toast(pts > 0 ? fmt::format("+{}", pts) : "Scratched");
			Ui("quill");
			g_phase = Phase::Scored;
			g_scoredTime = 0;
		}

		bool ShoutTargets()
		{
			for (int i = 0; i < 5; ++i) {
				if (Active(i) && !InCup(i)) {
					return true;
				}
			}
			return false;
		}
		bool CanShout() { return g_turn == 0 && g_phase == Phase::Choose && g_carry.die < 0 && g_rollsUsed < 3 && g_shoutAt < 0 && g_clock >= g_shoutReady; }

		void Shout()
		{
			if (g_turn != 0 || g_phase != Phase::Choose || g_carry.die >= 0 || g_shoutAt >= 0) {
				return;
			}
			if (g_clock < g_shoutReady) {
				Ui("ui_deny");
				return;
			}
			if (g_rollsUsed >= 3) {
				Toast("No rolls left");
				return;
			}
			if (!ShoutTargets()) {
				Toast("The board is already clear");
				return;
			}
			g_shoutAt = g_clock;
			g_shoutReady = g_clock + kShoutCooldown + kWordAt[2];
			Ui("shout");
		}

		// On "Dah", the blast: the dice are thrown back into the cup, the nearest first so it reads as a wave.
		void ShoutBlast()
		{
			Scene::ShoutFx();
			int order[5], n = 0;
			for (int i = 0; i < 5; ++i) {
				if (Active(i) && !InCup(i)) {
					order[n++] = i;
				}
			}
			std::sort(order, order + n, [](int a, int b) { return g_sim.DieAt(a).pos.y < g_sim.DieAt(b).pos.y; });
			for (int k = 0; k < n; ++k) {
				const int    i = order[k];
				const double delay = 0.04 + k * 0.06;
				// it drops into the cup with a knock, and a smaller one as it settles against the others
				g_later.push_back({ Audio::Now() + delay + 0.78, 3, i, 70 + Rand() * 20 });
				g_later.push_back({ Audio::Now() + delay + 0.9 + Rand() * 0.06, 0, i, 30 + Rand() * 10 });
				TweenFrom(i, 0.8, 6);
				g_anim[i].spins = true;
				g_anim[i].spin = Vec3{ Rand() - 0.5, Rand() - 0.5, Rand() - 0.5 }.Normalized();
				g_sim.AddToCup(i);
			}
		}

		// ---------------------------------------------------------------- the cup's motion (loose springs, as the preview)
		void SpringCup(Cup& c, const Vec3& t, double dt, double w, double z)
		{
			const int    steps = (std::max)(1, static_cast<int>(std::ceil(dt / 0.008)));
			const double h = dt / steps;
			for (int s = 0; s < steps; ++s) {
				c.v.x += (w * w * (t.x - c.p.x) - 2 * z * w * c.v.x) * h;
				c.v.y += (w * w * (t.y - c.p.y) - 2 * z * w * c.v.y) * h;
				c.v.z += (w * w * (t.z - c.p.z) - 2 * z * w * c.v.z) * h;
				c.p.x += c.v.x * h;
				c.p.y += c.v.y * h;
				c.p.z += c.v.z * h;
			}
		}
		void SpringTilt(double target, double dt, double w, double z)
		{
			const int    steps = (std::max)(1, static_cast<int>(std::ceil(dt / 0.008)));
			const double h = dt / steps;
			for (int s = 0; s < steps; ++s) {
				g_tilt.vel += (w * w * (target - g_tilt.angle) - 2 * z * w * g_tilt.vel) * h;
				g_tilt.angle += g_tilt.vel * h;
			}
		}
		void ApplyTilt(Cup& c, double leanFade)
		{
			g_tilt.lean0 = Quat::Nlerp(g_tilt.lean0, Quat{}, leanFade);
			c.q = Quat::FromAxisAngle(g_tilt.axis, g_tilt.angle) * g_tilt.lean0;
		}
		void LeanCup(Cup& c, double dt, double maxLean)
		{
			const double sp = std::hypot(c.v.x, c.v.y);
			const double lean = (std::min)(maxLean, sp * 0.012);
			const Quat   target = sp > 0.3 ? Quat::FromAxisAngle({ -c.v.y / sp, c.v.x / sp, 0 }, lean) : Quat{};
			c.q = Quat::Nlerp(c.q, target, 1 - std::exp(-dt * 9));
		}

		Quat FaceUp(int a_face) { return yacht::DiceSim::FaceUp(a_face ? a_face : 1); }

		void UpdateRoll(double a_dt)
		{
			double rolling = 0;
			for (int i = 0; i < 5; ++i) {
				const Vec3   p = g_sim.DieAt(i).pos;
				const double sp = g_bedsPrimed ? (p - g_prevPos[i]).Length() / (std::max)(a_dt, 1e-3) : 0.0;
				g_prevPos[i] = p;
				if (!Active(i) || InCup(i)) {
					continue;
				}
				// only a die sliding or rolling on the linen, inside the tray, scuffs
				if (p.z < kHalf + 0.15 && std::fabs(p.x) < kX && std::fabs(p.y) < kYh && sp > 1.5 && sp < 60) {
					rolling += (std::min)(sp, 25.0);
				}
			}
			g_bedsPrimed = true;
			Audio::SetRoll(static_cast<float>((std::min)(0.07, rolling * 0.002)));
		}

		// ---------------------------------------------------------------- the pointer on the table
		int PickDie(float mx, float my)
		{
			int   best = -1;
			float bestD = 1e9f;
			for (int i = 0; i < 5; ++i) {
				if (InCup(i) && !g_kept[i]) {
					continue;  // a press on the cup always means the cup
				}
				float sx = 0, sy = 0;
				if (!Scene::Project(g_drawPos[i], sx, sy)) {
					continue;
				}
				const float r = (std::max)(16.0f, Scene::PixelsPerUnit(g_drawPos[i]) * 0.9f);
				const float d = std::hypot(sx - mx, sy - my);
				if (d < r && d < bestD) {
					best = i;
					bestD = d;
				}
			}
			return best;
		}

		bool OverCup(float mx, float my)
		{
			const auto&  c = g_cups[g_turn];
			const double h = ActiveCupH();
			float        best = 1e9f, rad = 0;
			for (double z : { h, h * 0.5 }) {
				const Vec3 at = c.p + c.q.Rotate({ 0, 0, z });
				float      sx = 0, sy = 0;
				if (!Scene::Project(at, sx, sy)) {
					continue;
				}
				rad = (std::max)(rad, Scene::PixelsPerUnit(at) * static_cast<float>(kR + kCupWall));
				best = (std::min)(best, std::hypot(mx - sx, my - sy));
			}
			return best < (std::max)(30.0f, rad * 1.6f);
		}

		double CarryHeight(double x)
		{
			const double t = Clamp((x - (kX - 3)) / ((kRest.x - kR) - (kX - 3)), 0.0, 1.0);
			const double k = t * t * (3 - 2 * t);
			return kCarryZ + (kRest.z + ActiveCupH() + 1.2 * 1.4 - kCarryZ) * k;
		}

		void AimCup(float mx, float my)
		{
			double x = 0, y = 0;
			if (Scene::Unproject(mx, my, kShakeZ, x, y)) {
				const double m = kR + kCupWall + 0.4;
				g_cupTarget.x = Clamp(x, -kX + m, kX - m);
				g_cupTarget.y = Clamp(y, -kYh + m, kYh - m);
			}
		}

		void HandleInput()
		{
			float mx = 0, my = 0;
			Overlay::MousePos(mx, my);
			for (int key : Overlay::TakeKeys()) {
				if (key == 0x01 || key == 0x0F) {  // Esc, Tab
					if (g_phase == Phase::Wager || g_phase == Phase::Over) {
						Post(UiAct::Leave);
					} else if (g_phase != Phase::Idle) {
						g_confirmForfeit = !g_confirmForfeit;
					}
				} else if (key == 0x2C) {  // Z
					Shout();
				} else if (key == 0x23) {  // H
					g_help = !g_help;
					Ui("ui_select");
				} else if (key == 0x32) {  // M
					Audio::SetMuted(!Audio::Muted());
					Ui("ui_select");
				} else if (key == Overlay::kPadSoundKey) {  // the controller's Back: the sound panel
					if (g_soundMenu) {
						Config::Save();
					}
					g_soundMenu = !g_soundMenu;
					Ui("ui_select");
				}
			}
			for (const auto& p : Overlay::TakePresses()) {
				if (p.button != 0) {
					continue;
				}
				if (p.down) {
					if (p.onUi || g_press.on || g_turn != 0 || g_confirmForfeit) {
						continue;
					}
					if (g_phase == Phase::Choose) {
						if (const int i = PickDie(p.x, p.y); i >= 0) {
							g_press = { true, false, i, p.x, p.y, false };
							continue;
						}
					}
					if ((g_phase == Phase::Ready || g_phase == Phase::Choose) && g_rollsUsed < 3) {
						g_press = { true, true, -1, p.x, p.y, false };
					}
				} else if (g_press.on) {
					const auto press = g_press;
					g_press.on = false;
					if (press.cup) {
						if (press.moved && g_phase == Phase::Shaking) {
							g_pourPending = true;
						}
						continue;
					}
					if (!press.moved) {
						ClickDie(press.die);
						continue;
					}
					// let go of a carried die
					const int i = g_carry.die;
					g_carry.die = -1;
					if (i < 0) {
						continue;
					}
					double x = 0, y = 0;
					const bool onTable = Scene::Unproject(p.x, p.y, 0.0, x, y);
					if (OverCup(p.x, p.y) && g_rollsUsed < 3) {
						PutInCup(i);
					} else if (onTable && y < -kYh - kWallT * 0.5 && y > kRackY - 2 && std::fabs(x) < 7.5) {
						Keep(i);
					} else if (onTable && std::fabs(x) < kX + kWallT && std::fabs(y) < kYh + kWallT) {
						PlaceOnPad(i, x, y, true);
					} else if (g_carry.from == 2 && g_rollsUsed < 3) {
						PutInCup(i);
					} else if (g_carry.from == 1) {
						Keep(i);
					} else {
						const auto sp = FrontSpot(i);
						PlaceOnPad(i, sp.first, sp.second, true);
					}
				}
			}
			if (!g_press.on) {
				return;
			}
			const float dist = std::hypot(mx - g_press.x0, my - g_press.y0);
			if (g_press.cup) {
				if (!g_press.moved && dist > 10) {
					g_press.moved = true;
					if (DiceOnPad()) {
						Toast("Put the dice on the pad in the cup or on the rack first");
					} else if (!AnyInCup()) {
						Toast("The cup is empty");
					} else {
						AimCup(mx, my);
						BeginShake();
					}
				}
				if (g_press.moved && g_phase == Phase::Shaking) {
					AimCup(mx, my);
				}
				return;
			}
			const int i = g_press.die;
			if (!g_press.moved && dist > 6) {
				g_press.moved = true;
				if (Active(i) && !InCup(i)) {
					g_lastSpot[i] = std::make_pair(g_sim.DieAt(i).pos.x, g_sim.DieAt(i).pos.y);
				}
				const int from = g_kept[i] ? 1 : InCup(i) ? 2 : 0;
				TweenFrom(i, 0.12);
				g_sim.SetActive(i, false);
				g_kept[i] = false;
				g_rackSlot[i] = -1;
				g_carry = { i, g_drawPos[i].x, g_drawPos[i].y, kCarryZ, from };
			}
			if (g_press.moved && g_carry.die >= 0) {
				double z = g_carry.z;
				for (int pass = 0; pass < 2; ++pass) {
					double x = 0, y = 0;
					if (!Scene::Unproject(mx, my, z, x, y)) {
						break;
					}
					g_carry.x = x;
					g_carry.y = y;
					z = CarryHeight(x);
				}
				g_carry.z = z;
			}
		}

		void ProcessUiActs()
		{
			std::vector<std::pair<UiAct, int>> acts;
			{
				std::lock_guard lk(g_uiMtx);
				acts.swap(g_uiActs);
			}
			for (const auto& [act, arg] : acts) {
				switch (act) {
				case UiAct::PlaceBet:
					if (g_phase == Phase::Wager) {
						const int bet = std::clamp(arg, Config::Get().minBet, Config::Get().maxBet);
						if (GoldHeld() < bet || !g_gold) {
							Toast("You don't have that much gold");
							break;
						}
						Player()->RemoveItem(g_gold, bet, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
						g_bet = bet;
						g_betHeld = bet;
						Ui("coins", 0.7f);
						spdlog::info("Wager {} against {}", bet, g_oppName);
						Scene::Reveal();
						NewGame();
					}
					break;
				case UiAct::Leave:
					CloseGame();
					break;
				case UiAct::Score:
					ScoreBox(arg);
					break;
				case UiAct::Rematch:
					if (g_phase == Phase::Over) {
						g_phase = Phase::Wager;
						g_bet = std::clamp(g_bet, Config::Get().minBet, Config::Get().maxBet);
					}
					break;
				case UiAct::Forfeit:
					g_confirmForfeit = true;
					break;
				case UiAct::ForfeitYes:
					Settle("forfeit");
					break;
				case UiAct::ForfeitNo:
					g_confirmForfeit = false;
					break;
				case UiAct::Shout:
					Shout();
					break;
				case UiAct::ChooseCup:
					if (g_phase == Phase::Wager && CupUnlocked(arg)) {
						g_cupSel = kCups[arg].id;
						Scene::UseCup(0, PlayerCup().id);
						Ui("ui_equip");
					}
					break;
				case UiAct::ChooseDice:
					if ((g_phase == Phase::Wager || g_phase == Phase::Shop) && DiceOwned(arg)) {
						g_diceSel = kDiceSets[arg];
						Scene::UseDice(PlayerDice());
						g_soundSet = PlayerDice();
						Ui("ui_equip");
					}
					break;
				case UiAct::BuyDice:
					if (g_phase == Phase::Shop && DiceOnSale(arg)) {
						const int price = kDiceInfo[arg].gold;
						if (GoldHeld() < price || !g_gold) {
							Ui("ui_deny");
							Toast("You can't afford those");
							break;
						}
						Player()->RemoveItem(g_gold, price, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
						Ui("coins", 0.7f);
						g_ownedDice |= 1u << arg;
						g_diceSel = kDiceSets[arg];
						Toast(fmt::format("{} dice: yours", kDiceInfo[arg].name));
						spdlog::info("Bought {} dice for {}", kDiceInfo[arg].name, price);
					}
					break;
				case UiAct::CloseShop:
					if (g_phase == Phase::Shop) {
						g_phase = Phase::Idle;
						Scene::Close();
					}
					break;
				}
			}
		}

		// ---------------------------------------------------------------- per frame
		void Step(double dt)
		{
			g_clock += dt;
			ProcessUiActs();
			if (g_shoutAt >= 0 && g_clock - g_shoutAt >= kWordAt[2]) {
				g_shoutAt = -1;
				if (g_phase == Phase::Choose && g_turn == 0) {
					ShoutBlast();
				}
			}
			if (g_phase == Phase::Idle || g_phase == Phase::Wager) {
				// the table sits ready behind the wager: cups at rest, dice in the cup
				for (int k = 0; k < 2; ++k) {
					Scene::SetCup(k, g_cups[k].p, g_cups[k].q);
				}
				for (int key : Overlay::TakeKeys()) {  // Esc (or B) leaves; Back opens the sound panel
					if ((key == 0x01 || key == 0x0F) && g_phase == Phase::Wager) {
						Post(UiAct::Leave);
					} else if (key == Overlay::kPadSoundKey) {
						g_soundMenu = !g_soundMenu;
					}
				}
				return;
			}
			if (!g_confirmForfeit) {
				HandleInput();
			} else {
				Overlay::TakePresses();
				for (int key : Overlay::TakeKeys()) {  // Esc (or B) again: keep playing
					if (key == 0x01 || key == 0x0F) {
						Post(UiAct::ForfeitNo);
					}
				}
			}
			NpcTick(dt);
			auto& c = g_cups[g_turn];
			if (g_phase == Phase::Shaking) {
				g_shakeTime += dt;
				SpringCup(c, { g_cupTarget.x, g_cupTarget.y, kShakeZ }, dt, 22, 0.72);
				LeanCup(c, dt, 0.4);
				const double sp = std::hypot(c.v.x, c.v.y);
				if (sp > 4) {
					const double k = 1 - std::exp(-dt * 12);
					g_aimX += (c.v.x / sp - g_aimX) * k;
					g_aimY += (c.v.y / sp - g_aimY) * k;
				}
				if (g_pourPending && c.p.z > kShakeZ - 1 && std::fabs(c.p.x) < kX - 2 && std::fabs(c.p.y) < kYh - 2) {
					g_pourPending = false;
					BeginPour();
				}
			} else if (g_phase == Phase::Pouring && g_pour.on) {
				g_pour.t += dt;
				if (g_pour.t < 0.9) {
					SpringCup(c, g_pour.at, dt, 10, 0.42);
					SpringTilt(120.0 * 0.0174533, dt, 11, 0.36);
				} else {
					g_pour.on = false;
					g_phase = Phase::Rolling;
					g_rollingTime = 0;
				}
				ApplyTilt(c, 1 - std::exp(-dt * 6));
			} else {
				SpringCup(c, c.rest, dt, 6, g_tilt.on ? 0.5 : 0.8);
				if (g_tilt.on) {
					SpringTilt(0, dt, 7, 0.38);
					ApplyTilt(c, 1);
					if (std::fabs(g_tilt.angle) < 0.01 && std::fabs(g_tilt.vel) < 0.05) {
						g_tilt.on = false;
					}
				} else {
					LeanCup(c, dt, 0.25);
				}
			}

			// the other cup, if the turn changed while it was still tipped from its pour: back upright on its spot
			{
				auto& o = g_cups[1 - g_turn];
				SpringCup(o, o.rest, dt, 6, 0.8);
				o.q = Quat::Nlerp(o.q, Quat{}, 1 - std::exp(-dt * 5));
			}
			const int nEv = g_sim.Step(dt, c.p, c.q);
			g_soundCup = g_turn == 0 ? PlayerCup().sound : "tankard";
			for (int e = 0; e < nEv; ++e) {
				const auto& ev = g_sim.Events()[e];
				const double x = ev.die >= 0 ? g_sim.DieAt(ev.die).pos.x : c.p.x;
				Hit(static_cast<int>(ev.type), ev.speed, ev.die, ev.die >= 0 && InCup(ev.die), x);
			}
			UpdateRoll(dt);
			for (auto it = g_later.begin(); it != g_later.end();) {
				if (Audio::Now() >= it->at) {
					Hit(it->type, it->speed, it->die, true, it->die >= 0 ? g_sim.DieAt(it->die).pos.x : 0.0);
					it = g_later.erase(it);
				} else {
					++it;
				}
			}

			if (g_phase == Phase::Rolling) {
				g_rollingTime += dt;
				if (g_sim.AnyOffTable()) {
					RedoThrow();
				} else if (AnyInCup() && g_rollingTime > 1.0) {
					g_phase = Phase::Shaking;  // a die stayed in the cup: tip it out again
					g_pourPending = true;
					g_tilt.on = false;
				} else if (g_sim.AllSettled() && g_rollingTime > 0.3) {
					FinishRoll();
				}
			} else if (g_phase == Phase::Redo) {
				g_redoTime += dt;
				if (g_redoTime > 0.9) {
					for (int i = 0; i < 5; ++i) {
						if (g_rolled & (1u << i)) {
							TweenFrom(i, 0.45, 5);
							g_sim.AddToCup(i);
						}
						g_sim.SetFrozen(i, false);
					}
					g_phase = g_rollsUsed == 0 ? Phase::Ready : Phase::Choose;
					Hit(3, 30, -1, true, g_cups[0].p.x);
				}
			} else if (g_phase == Phase::Scored && g_turn == 0) {
				g_scoredTime += dt;
				if (g_scoredTime > 0.9) {
					if (g_cards[0].Full() && g_cards[1].Full()) {
						Settle("");
					} else {
						StartTurn(1);
					}
				}
			}

			// dice moved by hand count as they now lie
			if (g_phase == Phase::Choose && !(g_press.on && !g_press.cup && g_press.moved)) {
				for (int i = 0; i < 5; ++i) {
					if (!Active(i) || InCup(i) || !g_sim.DieAt(i).settled) {
						continue;
					}
					if (const int f = g_sim.TopFace(i); f) {
						g_faces[i] = f;
					}
				}
			}

			// draw: kept dice on the rack, a carried die under the pointer, the rest from the physics
			for (int i = 0; i < 5; ++i) {
				Vec3 target;
				Quat targetQ;
				bool easeIn = false;
				if (g_carry.die == i) {
					target = { g_carry.x, g_carry.y, g_carry.z };
					targetQ = FaceUp(g_faces[i]);
					easeIn = !g_anim[i].on;
				} else if (g_kept[i]) {
					const bool theirs = g_turn == 1;
					target = { theirs ? -RackX(g_rackSlot[i]) : RackX(g_rackSlot[i]), theirs ? kNpcRackY : kRackY, kRackTop + kHalf };
					targetQ = FaceUp(g_faces[i]);
					if (theirs) {
						targetQ = Quat::FromAxisAngle({ 0, 0, 1 }, 3.14159265) * targetQ;
					}
				} else {
					target = g_sim.DieAt(i).pos;
					targetQ = g_sim.DieAt(i).rot;
				}
				auto& a = g_anim[i];
				if (easeIn) {
					const double k = 1 - std::exp(-dt * 22);
					g_drawPos[i] = g_drawPos[i] + (target - g_drawPos[i]) * k;
					g_drawRot[i] = Quat::Nlerp(g_drawRot[i], targetQ, 1 - std::exp(-dt * 14));
				} else if (a.on) {
					a.t += dt;
					if (a.t < 0) {
						g_drawPos[i] = a.from;
						g_drawRot[i] = a.fromQ;
					} else {
						const double k = Ease((std::min)(1.0, a.t / a.dur));
						g_drawPos[i] = a.from + (target - a.from) * k;
						g_drawPos[i].z += std::sin(k * 3.14159265) * a.arc;
						g_drawRot[i] = Quat::Nlerp(a.fromQ, targetQ, k);
						if (a.spins) {
							g_drawRot[i] = Quat::FromAxisAngle(a.spin, std::sin(k * 3.14159265) * 3.14159265 * 3) * g_drawRot[i];
						}
						if (a.t >= a.dur) {
							a.on = false;
						}
					}
				} else {
					g_drawPos[i] = target;
					g_drawRot[i] = targetQ;
				}
				Scene::SetDie(i, g_drawPos[i], g_drawRot[i], true);
			}
			for (int k = 0; k < 2; ++k) {
				Scene::SetCup(k, g_cups[k].p, g_cups[k].q);
			}
		}

		// ---------------------------------------------------------------- who will play: seated NPCs near a table
		bool NearTable(RE::Actor* a_actor, float a_reach)
		{
			bool found = false;
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return false;
			}
			tes->ForEachReferenceInRange(a_actor, a_reach, [&](RE::TESObjectREFR& a_ref) {
				auto* base = a_ref.GetBaseObject();
				if (!base || base->Is(RE::FormType::NPC)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* model = skyrim_cast<RE::TESModel*>(base);
				const char* path = model ? model->GetModel() : nullptr;
				if (path && *path) {
					std::string low = path;
					std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (low.find("table") != std::string::npos && low.find("tablecloth") == std::string::npos) {
						found = true;
						return RE::BSContainer::ForEachResult::kStop;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return found;
		}

		template <class Self, class... Args>
		void Call(Self* a_self, const char* a_fn, Args... a_args)
		{
			if (!a_self) {
				return;
			}
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) {
				return;
			}
			RE::BSScript::Variable v;
			RE::BSScript::PackValue(&v, a_self);
			auto obj = v.GetObject();
			if (!obj) {
				return;
			}
			auto*                                                    args = RE::MakeFunctionArguments(std::move(a_args)...);
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
			vm->DispatchMethodCall1(obj, a_fn, args, cb);
		}

		void TagSeated()
		{
			auto* pl = RE::ProcessLists::GetSingleton();
			auto* player = Player();
			if (!pl || !player || !g_faction) {
				return;
			}
			std::unordered_map<RE::FormID, bool> now;
			for (auto& h : pl->highActorHandles) {
				auto a = h.get();
				if (!a || a->IsDead() || a->IsChild() || a->IsPlayerRef() || a->IsHostileToActor(player) || a->IsInCombat()) {
					continue;
				}
				if (a->GetPosition().GetDistance(player->GetPosition()) > 2500.0f) {
					continue;
				}
				auto* st = a->AsActorState();
				if (!st || st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kIsSitting) {
					continue;
				}
				if (NearTable(a.get(), Config::Get().tableReach)) {
					now[a->GetFormID()] = true;
					if (!g_tagged.contains(a->GetFormID())) {
						a->AddToFaction(g_faction, 0);
					}
				}
			}
			for (const auto& [id, _] : g_tagged) {
				if (!now.contains(id)) {
					if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) {
						Call(a, "RemoveFromFaction", g_faction);
					}
				}
			}
			if (now.size() != g_tagged.size()) {
				spdlog::info("{} seated NPC(s) at a table can be asked to play", now.size());
			}
			g_tagged.swap(now);
		}

		class Sinks : public RE::BSTEventSink<RE::TESTopicInfoEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* e, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
			{
				if (e && e->type == 1 && g_infoPlay && e->topicInfoFormID == g_infoPlay && e->speakerRef) {
					g_pendingSpeaker = e->speakerRef->GetFormID();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sinks g_sinks;

		void StartWith(RE::Actor* a_npc)
		{
			if (!a_npc || Scene::IsOpen()) {
				return;
			}
			Scene::RefreshOwnedCups();
			g_opponent = a_npc->GetHandle();
			g_oppId = a_npc->GetFormID();
			g_oppName = a_npc->GetDisplayFullName() ? a_npc->GetDisplayFullName() : "Your opponent";
			g_bet = std::clamp(g_bet, Config::Get().minBet, Config::Get().maxBet);
			g_cards[0].Clear();
			g_cards[1].Clear();
			g_cups[0].rest = kRest;
			g_cups[1].rest = kNpcRest;
			for (int k = 0; k < 2; ++k) {
				g_cups[k].p = g_cups[k].rest;
				g_cups[k].v = {};
				g_cups[k].q = {};
			}
			Scene::UseCup(0, PlayerCup().id);
			Scene::UseCup(1, "tankard");
			Scene::UseDice(PlayerDice());
			g_soundSet = PlayerDice();
			Audio::Ready();  // load the sounds now, not on the first hit
			g_sim.PlaceCup(kRest, {});
			g_sim.LoadCup(0x1F);
			for (int i = 0; i < 5; ++i) {
				g_drawPos[i] = g_sim.DieAt(i).pos;
				g_drawRot[i] = g_sim.DieAt(i).rot;
				g_anim[i].on = false;
				g_kept[i] = false;
				g_rackSlot[i] = -1;
			}
			g_phase = Phase::Wager;
			g_turn = 0;
			g_outcome.clear();
			Scene::Open();
			spdlog::info("Sat down to play {} ({:08X}); cup {}, dice {}", g_oppName, g_oppId, PlayerCup().id, PlayerDice());
		}

		// ---------------------------------------------------------------- the interface
		// Skyrim's own interface language, as the preview: off-white condensed capitals on smoke-black gradients, thin rules
		// that fade at the ends, a compass across the top, key prompts in the corner. The scorecards are notes on parchment,
		// written in ink. Laid out at 1080p sizes and scaled with the screen (g_ui).
		struct Fonts
		{
			ImFont *sans = nullptr, *sansSemi = nullptr, *turn = nullptr, *toast = nullptr, *toastLong = nullptr, *words = nullptr,
				   *title = nullptr, *titleMid = nullptr, *menu = nullptr, *score = nullptr, *skill = nullptr, *tiny = nullptr;
			ImFont *book = nullptr, *bookIt = nullptr, *bookSC = nullptr, *bookSCsm = nullptr, *hand = nullptr, *handBig = nullptr;
		} F;
		float g_ui = 1.0f;
		void* g_parchment[2]{};
		bool  g_artTried = false;

		constexpr ImU32 kWhite = IM_COL32(241, 238, 232, 255);
		constexpr ImU32 kGrey = IM_COL32(162, 157, 148, 255);
		constexpr ImU32 kDimC = IM_COL32(102, 98, 91, 255);
		constexpr ImU32 kFrost = IM_COL32(207, 226, 255, 255);
		constexpr ImU32 kInkC = IM_COL32(42, 28, 17, 255);
		constexpr ImU32 kInkSoftC = IM_COL32(94, 70, 49, 255);
		constexpr ImU32 kRubricC = IM_COL32(142, 47, 27, 255);
		const ImVec4    kRubric{ 0.557f, 0.184f, 0.106f, 1.0f };

		float S(float a_px) { return a_px * g_ui; }
		ImU32 Alpha(ImU32 a_col, float a_a)
		{
			const int a = static_cast<int>(((a_col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a_a, 0.0f, 1.0f));
			return (a_col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
		}
		std::string Upper(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
			return a_s;
		}
		double Now() { return ImGui::GetTime(); }

		// Text with letter-spacing (tracking, in em), as the CSS: drawn a character at a time.
		float Measure(ImFont* a_f, const std::string& a_s, float a_track)
		{
			if (!a_f) {
				return 0.0f;
			}
			const float size = a_f->FontSize;
			float       w = 0.0f;
			const char* p = a_s.c_str();
			const char* end = p + a_s.size();
			while (p < end) {
				const unsigned char c = static_cast<unsigned char>(*p);
				const int           len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
				w += a_f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, (std::min)(p + len, end)).x + a_track * size;
				p += len;
			}
			return w;
		}
		float Text(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track = 0.0f, float a_shadow = 0.0f)
		{
			if (!a_f) {
				return 0.0f;
			}
			const float size = a_f->FontSize;
			float       x = a_pos.x;
			const char* p = a_s.c_str();
			const char* end = p + a_s.size();
			while (p < end) {
				const unsigned char c = static_cast<unsigned char>(*p);
				const int           len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
				const char*         q = (std::min)(p + len, end);
				if (a_shadow > 0.0f) {
					a_dl->AddText(a_f, size, ImVec2(x + 1, a_pos.y + S(1.5f)), Alpha(IM_COL32(0, 0, 0, 255), a_shadow), p, q);
				}
				a_dl->AddText(a_f, size, ImVec2(x, a_pos.y), a_col, p, q);
				x += a_f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, q).x + a_track * size;
				p = q;
			}
			return x - a_pos.x;
		}
		// centred on x; tracking is added after each letter, so half of the last one is taken back
		void TextC(ImDrawList* a_dl, ImFont* a_f, float a_cx, float a_y, ImU32 a_col, const std::string& a_s, float a_track = 0.0f, float a_shadow = 0.0f)
		{
			const float w = Measure(a_f, a_s, a_track) - (a_f ? a_track * a_f->FontSize : 0.0f);
			Text(a_dl, a_f, ImVec2(a_cx - w * 0.5f, a_y), a_col, a_s, a_track, a_shadow);
		}
		// a soft glow: the text drawn a few times around itself, faintly
		void Glow(ImDrawList* a_dl, ImFont* a_f, ImVec2 a_pos, ImU32 a_col, const std::string& a_s, float a_track, float a_r)
		{
			for (int k = 0; k < 8; ++k) {
				const float a = k * 0.785f;
				Text(a_dl, a_f, ImVec2(a_pos.x + std::cos(a) * a_r, a_pos.y + std::sin(a) * a_r), a_col, a_s, a_track);
			}
		}

		// a thin rule that fades at both ends
		void FadeRule(ImDrawList* a_dl, float a_x0, float a_x1, float a_y, float a_alpha = 0.62f)
		{
			const ImU32 c = Alpha(kWhite, a_alpha), t = Alpha(kWhite, 0.0f);
			const float a = a_x0 + (a_x1 - a_x0) * 0.18f, b = a_x0 + (a_x1 - a_x0) * 0.82f;
			a_dl->AddRectFilledMultiColor(ImVec2(a_x0, a_y), ImVec2(a, a_y + 1), t, c, c, t);
			a_dl->AddRectFilled(ImVec2(a, a_y), ImVec2(b, a_y + 1), c);
			a_dl->AddRectFilledMultiColor(ImVec2(b, a_y), ImVec2(a_x1, a_y + 1), c, t, t, c);
		}
		void Diamond(ImDrawList* a_dl, ImVec2 a_c, float a_r, ImU32 a_fill, ImU32 a_line)
		{
			const ImVec2 p[4] = { { a_c.x, a_c.y - a_r }, { a_c.x + a_r, a_c.y }, { a_c.x, a_c.y + a_r }, { a_c.x - a_r, a_c.y } };
			if (a_fill) {
				a_dl->AddQuadFilled(p[0], p[1], p[2], p[3], a_fill);
			}
			if (a_line) {
				a_dl->AddQuad(p[0], p[1], p[2], p[3], a_line, 1.0f);
			}
		}
		// a rule with a diamond in the middle (the result screens)
		void DiamondRule(ImDrawList* a_dl, float a_cx, float a_y, float a_w)
		{
			FadeRule(a_dl, a_cx - a_w * 0.5f, a_cx + a_w * 0.5f, a_y);
			Diamond(a_dl, ImVec2(a_cx, a_y + 0.5f), S(5), IM_COL32(0, 0, 0, 255), kWhite);
		}
		// soft shadow under a panel
		void Shadow(ImDrawList* a_dl, ImVec2 a_a, ImVec2 a_b, float a_r, float a_alpha)
		{
			for (int k = 6; k >= 1; --k) {
				const float g = a_r * k / 6.0f;
				a_dl->AddRectFilled(ImVec2(a_a.x - g * 0.4f, a_a.y + g * 0.3f), ImVec2(a_b.x + g * 0.4f, a_b.y + g), Alpha(IM_COL32(0, 0, 0, 255), a_alpha / 6.0f), g);
			}
		}

		ImGuiWindowFlags kBare = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
		// An invisible window over a rectangle, so its buttons take the mouse (and the table doesn't)
		bool BeginArea(const char* a_id, ImVec2 a_pos, ImVec2 a_size)
		{
			ImGui::SetNextWindowPos(a_pos);
			ImGui::SetNextWindowSize(a_size);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			const bool open = ImGui::Begin(a_id, nullptr, kBare);
			ImGui::PopStyleVar(2);
			return open;
		}
		// an invisible hit area at a screen rectangle (inside BeginArea); plays the menu ticks
		ImGuiID g_hoverId = 0;
		bool Clicked(const char* a_id, ImVec2 a_min, ImVec2 a_max, bool* a_hovered = nullptr, bool a_enabled = true)
		{
			ImGui::SetCursorScreenPos(a_min);
			const bool  r = ImGui::InvisibleButton(a_id, ImVec2((std::max)(1.0f, a_max.x - a_min.x), (std::max)(1.0f, a_max.y - a_min.y)));
			const bool  h = ImGui::IsItemHovered();
			const auto  id = ImGui::GetItemID();
			if (h && a_enabled) {
				if (g_hoverId != id) {
					g_hoverId = id;
					Ui("ui_hover");
				}
			} else if (g_hoverId == id) {
				g_hoverId = 0;
			}
			if (a_hovered) {
				*a_hovered = h && a_enabled;
			}
			if (r && a_enabled) {
				Ui("ui_select");
			} else if (r) {
				Ui("ui_deny");
			}
			return r && a_enabled;
		}

		// Skyrim's menu button: grey capitals that brighten, with a short line either side, on hover
		bool MenuButton(const char* a_id, const std::string& a_label, float a_cx, float a_y, bool a_danger = false, bool a_enabled = true)
		{
			auto*       dl = ImGui::GetWindowDrawList();
			const auto  text = Upper(a_label);
			const float w = Measure(F.menu, text, 0.24f) + S(52);
			const float h = F.menu->FontSize + S(12);
			bool        hov = false;
			const bool  r = Clicked(a_id, ImVec2(a_cx - w * 0.5f, a_y), ImVec2(a_cx + w * 0.5f, a_y + h), &hov, a_enabled);
			const ImU32 col = !a_enabled ? kDimC : hov ? (a_danger ? IM_COL32(244, 185, 169, 255) : IM_COL32(255, 255, 255, 255)) : kGrey;
			if (hov) {
				Glow(dl, F.menu, ImVec2(a_cx - (w - S(52)) * 0.5f, a_y + S(6)), Alpha(a_danger ? IM_COL32(208, 106, 82, 255) : kWhite, 0.06f), text, 0.24f, S(3));
				const ImU32 lc = a_danger ? IM_COL32(208, 106, 82, 255) : kWhite;
				dl->AddRectFilled(ImVec2(a_cx - w * 0.5f, a_y + h * 0.5f), ImVec2(a_cx - w * 0.5f + S(12), a_y + h * 0.5f + 1), lc);
				dl->AddRectFilled(ImVec2(a_cx + w * 0.5f - S(12), a_y + h * 0.5f), ImVec2(a_cx + w * 0.5f, a_y + h * 0.5f + 1), lc);
			}
			TextC(dl, F.menu, a_cx + S(2), a_y + S(6), col, text, 0.24f, 0.6f);
			return r;
		}
		// a row of menu buttons, centred; returns the one pressed or -1
		int MenuButtons(const char* a_id, std::initializer_list<std::pair<std::string, bool>> a_items, float a_cx, float a_y)
		{
			std::vector<float> ws;
			float              total = 0;
			for (const auto& [label, danger] : a_items) {
				ws.push_back(Measure(F.menu, Upper(label), 0.24f) + S(52));
				total += ws.back();
			}
			total += S(46) * static_cast<float>(ws.size() - 1);
			float x = a_cx - total * 0.5f;
			int   k = 0, hit = -1;
			for (const auto& [label, danger] : a_items) {
				ImGui::PushID(k);
				if (MenuButton(a_id, label, x + ws[k] * 0.5f, a_y, danger)) {
					hit = k;
				}
				ImGui::PopID();
				x += ws[k] + S(46);
				++k;
			}
			return hit;
		}

		// ---- the pieces of the screen
		void Vignette(ImDrawList* a_dl, float W, float H, float a_veil)
		{
			const ImU32 b = IM_COL32(0, 0, 0, 255), t = IM_COL32(0, 0, 0, 0);
			a_dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.13f), Alpha(b, 0.45f), Alpha(b, 0.45f), t, t);
			a_dl->AddRectFilledMultiColor(ImVec2(0, H * 0.84f), ImVec2(W, H), t, t, Alpha(b, 0.55f), Alpha(b, 0.55f));
			a_dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.12f, H), Alpha(b, 0.35f), t, t, Alpha(b, 0.35f));
			a_dl->AddRectFilledMultiColor(ImVec2(W * 0.88f, 0), ImVec2(W, H), t, Alpha(b, 0.35f), Alpha(b, 0.35f), t);
			if (a_veil > 0.0f) {  // behind a full-screen menu: the world darkens, most at the edges
				a_dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Alpha(b, 0.45f * a_veil));
				a_dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.3f, H), Alpha(b, 0.4f * a_veil), t, t, Alpha(b, 0.4f * a_veil));
				a_dl->AddRectFilledMultiColor(ImVec2(W * 0.7f, 0), ImVec2(W, H), t, Alpha(b, 0.4f * a_veil), Alpha(b, 0.4f * a_veil), t);
			}
		}

		// The compass: whose turn, the three rolls as quest markers, and the Thu'um recharging underneath.
		void Compass(ImDrawList* dl, float W)
		{
			const float cx = W * 0.5f, top = S(18), tw = S(560), th = S(30);
			const ImVec2 a(cx - tw * 0.5f, top + S(2)), b(cx + tw * 0.5f, top + S(2) + th);
			dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 158), IM_COL32(0, 0, 0, 158), IM_COL32(0, 0, 0, 97), IM_COL32(0, 0, 0, 97));
			dl->AddLine(a, ImVec2(b.x, a.y), Alpha(kWhite, 0.72f));
			dl->AddLine(ImVec2(a.x, b.y), b, Alpha(kWhite, 0.72f));
			const float ticks[6] = { 0.08f, 0.20f, 0.32f, 0.68f, 0.80f, 0.92f };
			for (int k = 0; k < 6; ++k) {
				const float x = a.x + tw * ticks[k];
				dl->AddLine(ImVec2(x, a.y), ImVec2(x, a.y + S(k % 2 ? 7.0f : 11.0f)), Alpha(kWhite, 0.45f));
			}
			// the end caps (the compass's own arrows)
			auto cap = [&](float x0, float dir) {
				const float s = S(34) / 40.0f, y0 = top + S(-0.0f) + (th + S(4) - S(34)) * 0.5f;
				auto P = [&](float x, float y) { return ImVec2(x0 + (dir > 0 ? x : 40 - x) * s, y0 + y * s); };
				const ImVec2 o[5] = { P(40, 6), P(22, 6), P(4, 20), P(22, 34), P(40, 34) };
				dl->AddPolyline(o, 5, Alpha(kWhite, 0.9f), 0, 1.3f * g_ui);
				const ImVec2 i[3] = { P(30, 12), P(22, 20), P(30, 28) };
				dl->AddPolyline(i, 3, Alpha(kWhite, 0.9f), 0, 1.3f * g_ui);
				dl->AddCircleFilled(P(12, 20), 2 * s, Alpha(kWhite, 0.9f));
			};
			cap(a.x - S(34) + 1, 1);
			cap(b.x - 1, -1);
			// whose turn: the letters settle in as the turn changes
			const bool        over = g_phase == Phase::Over;
			const std::string t = Upper(over ? "Game over" : g_turn == 0 ? "Your turn" : g_oppName);
			const float       k = static_cast<float>(std::clamp((g_clock - g_turnAt) / 0.8, 0.0, 1.0));
			const float       ease = 1.0f - std::pow(1.0f - k, 3.0f);
			const float       track = 0.7f - (0.7f - 0.32f) * ease;
			TextC(dl, F.turn, cx, a.y + (th - F.turn->FontSize) * 0.5f, Alpha(g_turn == 0 || over ? kWhite : IM_COL32(217, 196, 187, 255), ease), t, track, 0.9f);
			// the rolls
			for (int r = 0; r < 3; ++r) {
				const ImVec2 c(b.x - S(22) - S(5.6f) - (2 - r) * S(17), a.y + th * 0.5f);
				const bool   used = g_turn == 0 && r < g_rollsUsed;
				if (used) {
					dl->AddCircleFilled(c, S(7), Alpha(kWhite, 0.18f));
				}
				Diamond(dl, c, S(5.6f), used ? kWhite : 0, Alpha(kWhite, 0.85f));
			}
			// the Thu'um
			const float mw = S(240), my = b.y + S(7);
			dl->AddRectFilled(ImVec2(cx - mw * 0.5f, my), ImVec2(cx + mw * 0.5f, my + S(3)), IM_COL32(0, 0, 0, 140));
			dl->AddRect(ImVec2(cx - mw * 0.5f - 1, my - 1), ImVec2(cx + mw * 0.5f + 1, my + S(3) + 1), Alpha(kWhite, 0.22f));
			const double span = kShoutCooldown + kWordAt[2];
			const float  fill = static_cast<float>(std::clamp(1.0 - (g_shoutReady - g_clock) / span, 0.0, 1.0));
			if (fill > 0.0f) {
				dl->AddRectFilled(ImVec2(cx - mw * 0.5f - S(2), my - S(2)), ImVec2(cx - mw * 0.5f + mw * fill + S(2), my + S(5)), IM_COL32(207, 226, 255, 40));
				dl->AddRectFilledMultiColor(ImVec2(cx - mw * 0.5f, my), ImVec2(cx - mw * 0.5f + mw * fill, my + S(3)), IM_COL32(143, 180, 232, 255),
					kFrost, kFrost, IM_COL32(143, 180, 232, 255));
			}
			// the stake, small, under it
			TextC(dl, F.tiny, cx, my + S(10), kGrey, Upper(fmt::format("Wager {} septims", g_bet)), 0.3f, 0.8f);
		}

		// The words of power, each appearing as it's spoken.
		void Words(ImDrawList* dl, float W, float H)
		{
			if (g_shoutReady <= 0.0) {
				return;
			}
			const double t0 = g_shoutReady - kShoutCooldown - kWordAt[2];  // when the Shout began
			const double t = g_clock - t0;
			if (t < 0 || t > kWordAt[2] + 1.7) {
				return;
			}
			const char*  words[3] = { "FUS", "RO", "DAH" };
			const float  gap = F.words->FontSize * 0.55f, track = 0.24f;
			float        total = 0;
			float        ws[3];
			for (int k = 0; k < 3; ++k) {
				ws[k] = Measure(F.words, words[k], track);
				total += ws[k];
			}
			total += gap * 2;
			float       x = W * 0.5f - total * 0.5f;
			const float y = H * 0.27f;
			const float out = static_cast<float>(std::clamp((t - (kWordAt[2] + 0.7)) / 0.9, 0.0, 1.0));
			for (int k = 0; k < 3; ++k) {
				const double since = t - kWordAt[k];
				if (since >= 0) {
					const float in = static_cast<float>(std::clamp(since / 0.45, 0.0, 1.0));
					const float e = 1.0f - std::pow(1.0f - in, 3.0f);
					const float a = e * (1.0f - out);
					ImFont*      f = F.words;
					const ImVec2 pos(x, y - (1.0f - e) * S(14));  // drops into place as it appears
					Glow(dl, f, pos, Alpha(IM_COL32(110, 160, 240, 255), 0.07f * a), words[k], track, S(10) + S(22) * (1.0f - e));
					Glow(dl, f, pos, Alpha(IM_COL32(160, 200, 255, 255), 0.12f * a), words[k], track, S(4));
					Text(dl, f, ImVec2(pos.x, pos.y + S(3)), Alpha(IM_COL32(0, 0, 0, 255), 0.6f * a), words[k], track);
					Text(dl, f, pos, Alpha(IM_COL32(246, 249, 255, 255), a), words[k], track);
				}
				x += ws[k] + gap;
			}
		}

		// What to do now, bottom centre, with the mouse (or, while waiting, a turning ring).
		void Hint(ImDrawList* dl, float W, float H)
		{
			std::string hint;
			bool        waiting = false;
			if (g_turn == 1 && g_phase != Phase::Over) {
				hint = g_oppName + " is playing";
				waiting = true;
			} else if (g_phase == Phase::Ready) {
				hint = "Hold the left button and move to shake the cup. Let go to throw.";
			} else if (g_phase == Phase::Shaking) {
				hint = "Shake it, then let go: it tips the way you last moved.";
			} else if (g_phase == Phase::Pouring || g_phase == Phase::Rolling) {
				hint = "Rolling";
				waiting = true;
			} else if (g_phase == Phase::Redo) {
				hint = "Off the table. Throw again.";
			} else if (g_phase == Phase::Choose) {
				hint = g_rollsUsed < 3 ? "Click a die to keep it. Drag dice into the cup to roll again, or pick a box on your card."
				                       : "No rolls left. Pick a box on your card.";
			}
			if (hint.empty()) {
				return;
			}
			const float tw = Measure(F.sans, hint, 0.07f), iw = S(18), gap = S(12);
			const float x0 = W * 0.5f - (tw + iw + gap) * 0.5f, y = H - S(26) - S(25);
			if (waiting) {
				const float  a = static_cast<float>(Now() * 5.2);
				const ImVec2 c(x0 + iw * 0.5f, y + S(12.5f));
				dl->PathArcTo(c, S(6.5f), a, a + 1.4f);
				dl->PathStroke(kWhite, 0, 1.5f * g_ui);
				dl->PathArcTo(c, S(6.5f), a + 3.14f, a + 4.54f);
				dl->PathStroke(kWhite, 0, 1.5f * g_ui);
			} else {
				// the mouse: left button lit
				const ImVec2 a(x0, y), b(x0 + iw, y + S(25));
				dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + iw * 0.5f, a.y + S(10.5f)), kWhite, S(9), ImDrawFlags_RoundCornersTopLeft);
				dl->AddRect(a, b, kWhite, S(9), 0, 1.5f * g_ui);
				dl->AddLine(ImVec2(a.x + iw * 0.5f, a.y), ImVec2(a.x + iw * 0.5f, a.y + S(10.5f)), kWhite, 1.0f);
			}
			Text(dl, F.sans, ImVec2(x0 + iw + gap, y + (S(25) - F.sans->FontSize) * 0.5f), kWhite, hint, 0.07f, 0.9f);
		}

		// Notifications, like "Location Discovered": big spaced capitals between two fading rules.
		void ToastBanner(ImDrawList* dl, float W, float H)
		{
			if (g_toast.empty()) {
				return;
			}
			const double t = g_clock - g_toastTime;
			if (t < 0 || t > 2.2) {
				return;
			}
			const float a = static_cast<float>(t < 0.35 ? t / 0.35 : t > 1.85 ? (2.2 - t) / 0.35 : 1.0);
			const bool  isLong = g_toast.size() > 16;
			ImFont*     f = isLong ? F.toastLong : F.toast;
			const float track = isLong ? 0.16f : 0.3f;
			const auto  text = Upper(g_toast);
			const float w = Measure(f, text, track) + S(120);
			const float rise = static_cast<float>(std::min(t / 0.8, 1.0)) * S(6);
			const float y = H * 0.34f - f->FontSize * 0.5f - rise;
			FadeRule(dl, W * 0.5f - w * 0.5f, W * 0.5f + w * 0.5f, y - S(12), 0.62f * a);
			FadeRule(dl, W * 0.5f - w * 0.5f, W * 0.5f + w * 0.5f, y + f->FontSize + S(12), 0.62f * a);
			TextC(dl, f, W * 0.5f, y, Alpha(kWhite, a), text, track, 0.9f * a);
		}

		// A scorecard: a note on parchment, written in ink. The one whose turn it is is lit; the other sits back.
		void Ledger(int a_card, bool a_left)
		{
			const auto& io = ImGui::GetIO();
			const float w = S(384), padX = S(30), padY = S(22);
			const float rowH = S(23);
			const float h = padY * 2 + S(36) + S(24) + S(26) + rowH * 7 + S(28);
			const ImVec2 pos(a_left ? S(22) : io.DisplaySize.x - w - S(22), S(18));
			const bool   active = a_card == g_turn || g_phase == Phase::Over;
			char         id[16];
			std::snprintf(id, sizeof(id), "##ledger%d", a_card);
			BeginArea(id, pos, ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			// The shadow follows the torn edge: the parchment itself, in black, smeared in rings (a cheap blur), drawn
			// outside the window's clip so it isn't cut off into a box.
			dl->PushClipRectFullScreen();
			if (g_parchment[a_card]) {
				const float r = S(active ? 14 : 10), a = active ? 0.55f : 0.42f;
				const ImVec2 drop(S(4), S(7));
				for (int ring = 1; ring <= 3; ++ring) {
					const float rr = r * ring / 3.0f;
					for (int k = 0; k < 8; ++k) {
						const float ang = k * 0.785398f + ring * 0.39f;
						const ImVec2 o(drop.x + std::cos(ang) * rr, drop.y + std::sin(ang) * rr);
						dl->AddImage(reinterpret_cast<ImTextureID>(g_parchment[a_card]), ImVec2(pos.x + o.x, pos.y + o.y), ImVec2(pos.x + w + o.x, pos.y + h + o.y),
							ImVec2(0, 0), ImVec2(1, 1), Alpha(IM_COL32(0, 0, 0, 255), a / 8.0f * (1.4f - ring * 0.25f)));
					}
				}
			} else {
				Shadow(dl, ImVec2(pos.x + S(6), pos.y + S(8)), ImVec2(pos.x + w - S(6), pos.y + h - S(4)), S(active ? 22 : 16), active ? 0.75f : 0.6f);
			}
			dl->PopClipRect();
			const ImU32 tint = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 140, 128, 255);
			if (g_parchment[a_card]) {
				dl->AddImage(reinterpret_cast<ImTextureID>(g_parchment[a_card]), pos, ImVec2(pos.x + w, pos.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
			} else {
				dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), active ? IM_COL32(214, 191, 149, 255) : IM_COL32(140, 124, 100, 255));
			}
			const float ink = active ? 1.0f : 0.72f;
			const ImU32 inkC = Alpha(kInkC, ink), softC = Alpha(kInkSoftC, ink), rubC = Alpha(kRubricC, ink);
			float       y = pos.y + padY;
			const float x0 = pos.x + padX, x1 = pos.x + w - padX;
			// head: who, and the total
			Text(dl, F.bookSC, ImVec2(x0, y + S(6)), inkC, a_card == 0 ? "Yours" : g_oppName, 0.04f);
			const std::string tot = std::to_string(g_cards[a_card].Total());
			Text(dl, F.handBig, ImVec2(x1 - Measure(F.handBig, tot, 0), y - S(4)), inkC, tot);
			y += S(36);
			const auto& r = g_records[g_oppId];
			const int   won = a_card == 0 ? r.w : r.l, lost = a_card == 0 ? r.l : r.w;
			Text(dl, F.bookIt, ImVec2(x0, y), softC, fmt::format("Record {}-{}{}", won, lost, r.d ? fmt::format(", {} drawn", r.d) : ""));
			y += S(26);
			// two columns: the upper section with its bonus, the lower with the yacht bonuses
			const float colGap = S(22), cw0 = (w - padX * 2 - colGap) / 2.18f, cw1 = cw0 * 1.18f;
			int         dice[5];
			Dice(dice);
			const bool pick = a_card == 0 && g_turn == 0 && g_phase == Phase::Choose && g_carry.die < 0 && g_shoutAt < 0;
			auto       column = [&](float cx0, float cwid, int from, int to, const char* label) {
                float yy = y;
                Text(dl, F.bookSCsm, ImVec2(cx0, yy), rubC, label, 0.08f);
                yy += S(24);
                for (int cat = from; cat <= to; ++cat) {
                    const auto& card = g_cards[a_card];
                    const bool  filled = card.Filled(cat);
                    const int   prev = pick && !filled ? card.Preview(cat, dice) : -1;
                    const ImVec2 ra(cx0, yy), rb(cx0 + cwid, yy + rowH);
                    bool         hov = false;
                    if (prev >= 0) {
                        ImGui::PushID(cat);
                        if (Clicked("row", ra, rb, &hov)) {
                            Post(UiAct::Score, cat);
                        }
                        ImGui::PopID();
                    }
                    if (hov) {
                        dl->AddRectFilledMultiColor(ra, rb, IM_COL32(142, 47, 27, 50), IM_COL32(142, 47, 27, 10), IM_COL32(142, 47, 27, 10), IM_COL32(142, 47, 27, 50));
                        Diamond(dl, ImVec2(ra.x + S(2.5f), ra.y + rowH * 0.5f), S(3), rubC, 0);
                    }
                    // the dotted line under each entry
                    for (float dx = ra.x + S(6); dx < rb.x; dx += S(3)) {
                        dl->AddRectFilled(ImVec2(dx, rb.y - S(2)), ImVec2(dx + 1, rb.y - S(1)), Alpha(kInkC, 0.35f * ink));
                    }
                    Text(dl, F.book, ImVec2(ra.x + S(10), ra.y + (rowH - F.book->FontSize) * 0.5f), filled ? softC : inkC, yacht::CategoryName(cat));
                    std::string val;
                    ImU32       vc = inkC;
                    if (filled) {
                        val = std::to_string(card.boxes[cat]);
                    } else if (prev >= 0) {
                        val = std::to_string(prev);
                        vc = Alpha(kRubricC, (hov ? 1.0f : prev == 0 ? 0.45f : 0.85f) * ink);
                    }
                    if (!val.empty()) {
                        const float vw = Measure(F.hand, val, 0);
                        const ImVec2 vp(rb.x - S(2) - vw, ra.y + (rowH - F.hand->FontSize) * 0.5f);
                        // freshly written: the ink runs in from the left
                        const double since = g_clock - g_fillAt[a_card][cat];
                        const float  reveal = filled && since >= 0 && since < 0.9 ? static_cast<float>(since / 0.9) : 1.0f;
                        dl->PushClipRect(vp, ImVec2(vp.x + vw * reveal + 1, vp.y + F.hand->FontSize + S(4)), true);
                        Text(dl, F.hand, vp, vc, val);
                        dl->PopClipRect();
                    }
                    yy += rowH;
                }
                return yy;
			};
			const float yu = column(x0, cw0, yacht::kOnes, yacht::kSixes, "Upper");
			const float yl = column(x0 + cw0 + colGap, cw1, yacht::kThreeKind, yacht::kChance, "Lower");
			// the sums underneath
			const auto& card = g_cards[a_card];
			const float sy = (std::max)(yu, yl) + S(4);
			Text(dl, F.bookIt, ImVec2(x0 + S(10), sy), softC, "Bonus 35 at 63");
			const std::string up = fmt::format("{} / 63", card.UpperSum());
			Text(dl, F.bookIt, ImVec2(x0 + cw0 - S(2) - Measure(F.bookIt, up, 0), sy), softC, up);
			const float lx = x0 + cw0 + colGap;
			Text(dl, F.bookIt, ImVec2(lx + S(10), sy), softC, "Yacht bonuses");
			const std::string yb = std::to_string(card.yachtBonuses * yacht::kYachtBonus);
			Text(dl, F.bookIt, ImVec2(lx + cw1 - S(2) - Measure(F.bookIt, yb, 0), sy), softC, yb);
			ImGui::End();
		}

		// The key prompts, bottom right, as on Skyrim's menus.
		void KeyBar(float W, float H)
		{
			struct Key
			{
				const char* key;
				std::string label;
				ImU32       col, hot;
				bool        enabled;
				int         act;  // 0 shout, 1 help, 2 sound, 3 forfeit
			};
			const bool canShout = CanShout() && ShoutTargets();
			const bool pad = Overlay::UsingPad();  // a controller: its buttons
			const Key  keys[4] = {
                { pad ? "Y" : "Z", "Fus Ro Dah", IM_COL32(185, 205, 234, 255), IM_COL32(255, 255, 255, 255), canShout, 0 },
                { pad ? "X" : "H", "How to play", kGrey, kWhite, true, 1 },
                { pad ? "Back" : "M", Audio::Muted() ? "Muted" : "Sound", kGrey, kWhite, true, 2 },
                { pad ? "B" : "Tab", "Forfeit", kGrey, IM_COL32(240, 182, 166, 255), g_phase != Phase::Over, 3 },
			};
			float widths[4], total = 0;
			for (int k = 0; k < 4; ++k) {
				widths[k] = (std::max)(S(26), Measure(F.sansSemi, keys[k].key, 0.04f) + S(10)) + S(9) + Measure(F.sans, Upper(keys[k].label), 0.14f);
				total += widths[k];
			}
			total += S(26) * 3;
			const float right = W - S(30), bottom = H - S(22), h = S(24);
			const ImVec2 area(right - total - S(120), bottom - h - S(26));
			BeginArea("##keys", area, ImVec2(W - area.x, H - area.y));
			auto* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilledMultiColor(area, ImVec2(area.x + (W - area.x) * 0.3f, H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 158), IM_COL32(0, 0, 0, 158),
				IM_COL32(0, 0, 0, 0));
			dl->AddRectFilled(ImVec2(area.x + (W - area.x) * 0.3f, area.y), ImVec2(W, H), IM_COL32(0, 0, 0, 158));
			float x = right - total;
			for (int k = 0; k < 4; ++k) {
				const auto& key = keys[k];
				bool        hov = false;
				ImGui::PushID(k);
				const bool pressed = Clicked("key", ImVec2(x, bottom - h - S(2)), ImVec2(x + widths[k], bottom + S(2)), &hov, key.enabled);
				ImGui::PopID();
				if (key.act == 2) {
					g_soundKey = ImVec2(x + widths[k] * 0.5f, bottom - h);
				}
				const ImU32 col = !key.enabled ? (k == 0 ? IM_COL32(76, 88, 106, 255) : kDimC) : (hov || (key.act == 2 && g_soundMenu)) ? key.hot : key.col;
				const float kw = (std::max)(S(26), Measure(F.sansSemi, key.key, 0.04f) + S(10));
				const ImVec2 ka(x, bottom - h), kb(x + kw, bottom);
				dl->AddRectFilled(ka, kb, IM_COL32(0, 0, 0, 90), S(4));
				dl->AddRect(ka, kb, col, S(4), 0, 1.5f * g_ui);
				TextC(dl, F.sansSemi, x + kw * 0.5f + S(0.5f), ka.y + (h - F.sansSemi->FontSize) * 0.5f, col, key.key, 0.04f);
				if (hov) {
					Glow(dl, F.sans, ImVec2(kb.x + S(9), ka.y + (h - F.sans->FontSize) * 0.5f), Alpha(k == 0 ? IM_COL32(160, 200, 255, 255) : kWhite, 0.07f),
						Upper(key.label), 0.14f, S(3));
				}
				Text(dl, F.sans, ImVec2(kb.x + S(9), ka.y + (h - F.sans->FontSize) * 0.5f), col, Upper(key.label), 0.14f, 0.7f);
				if (k == 0 && g_clock < g_shoutReady) {  // recharging
					const double span = kShoutCooldown + kWordAt[2];
					const float  f = static_cast<float>(std::clamp(1.0 - (g_shoutReady - g_clock) / span, 0.0, 1.0));
					const float  lw = widths[k] - kw - S(9);
					dl->AddRectFilled(ImVec2(kb.x + S(9), bottom + S(4)), ImVec2(kb.x + S(9) + lw * f, bottom + S(6)), kFrost);
				}
				if (pressed) {
					if (key.act == 0) {
						Post(UiAct::Shout);
					} else if (key.act == 1) {
						g_help = !g_help;
					} else if (key.act == 2) {
						if (g_soundMenu) {
							Config::Save();
						}
						g_soundMenu = !g_soundMenu;
					} else {
						Post(UiAct::Forfeit);
					}
				}
				x += widths[k] + S(26);
			}
			ImGui::End();
		}

		void MsgBox(ImDrawList* dl, ImVec2 a, ImVec2 b);

		// The sound panel: three sliders (the music, the dice, the Thu'um) and a mute, hung over the Sound key. Saved to the
		// INI when it closes.
		void SoundMenu(float W, float H)
		{
			(void)H;
			auto&       v = Config::Edit();
			const float w = S(330), rowH = S(58), padX = S(26), padY = S(20);
			const float h = padY * 2 + S(30) + rowH * 4 + S(40);
			const float x = std::clamp(g_soundKey.x - w * 0.5f, S(12), W - w - S(12)), y = g_soundKey.y - S(18) - h;
			BeginArea("##sound", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
			// a little pointer down to the key
			const float px = std::clamp(g_soundKey.x, x + S(20), x + w - S(20));
			dl->AddTriangleFilled(ImVec2(px - S(9), y + h), ImVec2(px + S(9), y + h), ImVec2(px, y + h + S(9)), IM_COL32(0, 0, 0, 199));
			TextC(dl, F.sansSemi, x + w * 0.5f, y + padY, kWhite, "SOUND", 0.3f);
			struct Row
			{
				const char* id;
				const char* label;
				float*      val;
				float       max;
				ImU32       tint;
			};
			const Row rows[4] = {
				{ "music", "Music", &v.musicVolume, 1.0f, IM_COL32(222, 196, 140, 255) },
				{ "dice", "Dice & cups", &v.volume, 2.0f, kWhite },
				{ "shout", "Fus Ro Dah", &v.shoutVolume, 2.0f, IM_COL32(185, 205, 234, 255) },
				{ "effects", "Effects", &v.effectsVolume, 2.0f, kGrey },
			};
			const bool muted = Audio::Muted();
			float      ry = y + padY + S(34);
			for (const auto& r : rows) {
				const float tx0 = x + padX, tx1 = x + w - padX, ty = ry + S(34);
				const float f = std::clamp(*r.val / r.max, 0.0f, 1.0f);
				ImGui::PushID(r.id);
				ImGui::SetCursorScreenPos(ImVec2(tx0 - S(8), ty - S(14)));
				ImGui::InvisibleButton("track", ImVec2(tx1 - tx0 + S(16), S(28)));
				const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
				if (act) {
					const float nf = std::clamp((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0), 0.0f, 1.0f);
					*r.val = std::round(nf * r.max * 100.0f) / 100.0f;
				}
				if (ImGui::IsItemDeactivated()) {
					Ui(std::string_view(r.id) == "dice" ? "ui_equip" : "ui_select");
				}
				ImGui::PopID();
				const ImU32 lab = muted ? kDimC : (hov || act) ? kWhite : kGrey;
				Text(dl, F.sans, ImVec2(tx0, ry), lab, Upper(r.label), 0.14f);
				const std::string pct = fmt::format("{}%", static_cast<int>(std::round(*r.val * 100.0f)));
				Text(dl, F.sans, ImVec2(tx1 - Measure(F.sans, pct, 0.0f), ry), lab, pct);
				const float hx = tx0 + (tx1 - tx0) * f;
				dl->AddRectFilled(ImVec2(tx0, ty - S(1.5f)), ImVec2(tx1, ty + S(1.5f)), IM_COL32(255, 255, 255, 38), S(2));
				dl->AddRectFilled(ImVec2(tx0, ty - S(1.5f)), ImVec2(hx, ty + S(1.5f)), muted ? kDimC : Alpha(r.tint, 0.85f), S(2));
				if (r.max > 1.0f) {  // the usual level, for reference
					const float mx = tx0 + (tx1 - tx0) * (1.0f / r.max);
					dl->AddLine(ImVec2(mx, ty - S(5)), ImVec2(mx, ty + S(5)), IM_COL32(255, 255, 255, 70), 1.0f);
				}
				Diamond(dl, ImVec2(hx, ty), S(act ? 8 : hov ? 7 : 6), IM_COL32(0, 0, 0, 255), muted ? kDimC : (hov || act) ? kWhite : r.tint);
				ry += rowH;
			}
			// mute everything, or not
			{
				const std::string lbl = muted ? "UNMUTE" : "MUTE ALL";
				bool              hov = false;
				const float       bw = Measure(F.sans, lbl, 0.16f) + S(24), by = y + h - padY - S(22);
				if (Clicked("mute", ImVec2(x + w * 0.5f - bw * 0.5f, by - S(4)), ImVec2(x + w * 0.5f + bw * 0.5f, by + S(22)), &hov)) {
					Audio::SetMuted(!muted);
				}
				TextC(dl, F.sans, x + w * 0.5f, by, hov ? kWhite : kGrey, lbl, 0.16f);
				if (hov) {
					dl->AddLine(ImVec2(x + w * 0.5f - bw * 0.5f, by + S(21)), ImVec2(x + w * 0.5f + bw * 0.5f, by + S(21)), Alpha(kWhite, 0.5f));
				}
			}
			ImGui::End();
			// a click anywhere else puts it away
			const auto& io = ImGui::GetIO();
			if (ImGui::IsMouseClicked(0)) {
				const ImVec2 m = io.MousePos;
				const bool   inside = m.x >= x && m.x <= x + w && m.y >= y && m.y <= y + h + S(10);
				const bool   onKey = std::abs(m.x - g_soundKey.x) < S(60) && m.y >= g_soundKey.y - S(6) && m.y <= g_soundKey.y + S(34);
				if (!inside && !onKey) {
					g_soundMenu = false;
					Config::Save();
				}
			}
		}

		// A message box: smoke black between two rules, with brackets at the sides.
		void MsgBox(ImDrawList* dl, ImVec2 a, ImVec2 b)
		{
			Shadow(dl, a, b, S(30), 0.7f);
			dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 219), IM_COL32(0, 0, 0, 219), IM_COL32(0, 0, 0, 199), IM_COL32(0, 0, 0, 199));
			dl->AddLine(a, ImVec2(b.x, a.y), Alpha(kWhite, 0.62f));
			dl->AddLine(ImVec2(a.x, b.y), b, Alpha(kWhite, 0.62f));
			const ImU32 br = Alpha(kWhite, 0.45f);
			const float i = S(6), bw = S(10);
			dl->AddLine(ImVec2(a.x + i + bw, a.y + i), ImVec2(a.x + i, a.y + i), br);
			dl->AddLine(ImVec2(a.x + i, a.y + i), ImVec2(a.x + i, b.y - i), br);
			dl->AddLine(ImVec2(a.x + i, b.y - i), ImVec2(a.x + i + bw, b.y - i), br);
			dl->AddLine(ImVec2(b.x - i - bw, a.y + i), ImVec2(b.x - i, a.y + i), br);
			dl->AddLine(ImVec2(b.x - i, a.y + i), ImVec2(b.x - i, b.y - i), br);
			dl->AddLine(ImVec2(b.x - i, b.y - i), ImVec2(b.x - i - bw, b.y - i), br);
		}

		// Word-wrapped grey text inside a box, returns the height used
		float Para(ImDrawList* dl, ImFont* f, float x, float y, float wrap, ImU32 col, const std::string& s)
		{
			const ImVec2 sz = f->CalcTextSizeA(f->FontSize, FLT_MAX, wrap, s.c_str());
			dl->AddText(f, f->FontSize, ImVec2(x, y), col, s.c_str(), nullptr, wrap);
			return sz.y;
		}

		void HelpBox(float W)
		{
			const float w = S(480), x = W * 0.5f - w * 0.5f, y = S(112);
			const float h = S(250);
			BeginArea("##help", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
			TextC(dl, F.menu, W * 0.5f, y + S(22), kWhite, "YACHT", 0.26f);
			float yy = y + S(64);
			const float px = x + S(32), pw = w - S(64);
			yy += Para(dl, F.sans, px, yy, pw, kGrey, "Hold the left button and move to lift the cup and shake it. Let go to flick the dice out the way you last moved. A die that leaves the board means throw again.") + S(9);
			yy += Para(dl, F.sans, px, yy, pw, kGrey, "Click a die to set it on the rack. Drag one back into the cup to roll it again. The cup waits until the board is clear.") + S(9);
			yy += Para(dl, F.sans, px, yy, pw, kGrey, "Any cup you carry can be your cup at the table. Every ten wins, innkeepers sell you a finer set of dice.");
			ImGui::End();
		}

		// The rank and the cup and dice, bottom left, like the inventory's skill line.
		void Dock(ImDrawList* dl, float W, float H)
		{
			(void)W;
			const float w = S(420), h = S(150);
			const ImVec2 a(0, H - h), b(w, H);
			dl->AddRectFilledMultiColor(a, ImVec2(w * 0.62f, H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 214), IM_COL32(0, 0, 0, 214));
			dl->AddRectFilledMultiColor(ImVec2(w * 0.62f, a.y), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 184));
			const auto& cup = PlayerCup();
			const float x0 = S(30), x1 = w - S(44);
			float       y = a.y + S(78);
			auto line = [&](const char* label, const std::string& value) {
				Text(dl, F.tiny, ImVec2(x0, y + S(3)), kGrey, label, 0.32f);
				Text(dl, F.sans, ImVec2(x0 + S(90), y), kWhite, Upper(value), 0.08f, 0.7f);
				dl->AddRectFilledMultiColor(ImVec2(x0, y + S(24)), ImVec2(x1, y + S(25)), Alpha(kWhite, 0.62f), Alpha(kWhite, 0.0f), Alpha(kWhite, 0.0f),
					Alpha(kWhite, 0.62f));
				y += S(30);
			};
			line("CUP", cup.name);
			const int di = (std::max)(0, DiceIndex(PlayerDice()));
			line("DICE", kDiceInfo[di].name);
		}

		void DrawPlay()
		{
			const auto& io = ImGui::GetIO();
			const float W = io.DisplaySize.x, H = io.DisplaySize.y;
			auto*       bg = ImGui::GetBackgroundDrawList();
			Vignette(bg, W, H, 0.0f);
			Compass(bg, W);
			Dock(bg, W, H);
			Ledger(1, true);
			Ledger(0, false);
			if (g_phase != Phase::Over) {
				Hint(bg, W, H);
				KeyBar(W, H);
				if (g_soundMenu) {
					SoundMenu(W, H);
				}
			} else {
				g_soundMenu = false;
			}
			auto* fg = ImGui::GetForegroundDrawList();
			Words(fg, W, H);
			ToastBanner(bg, W, H);
			if (g_help && g_phase != Phase::Over) {
				HelpBox(W);
			}
			if (g_confirmForfeit && g_phase != Phase::Over) {
				const float w = S(520), h = S(170), x = W * 0.5f - w * 0.5f, y = H * 0.5f - h * 0.5f;
				Vignette(bg, W, H, 0.8f);
				BeginArea("##forfeit", ImVec2(x, y), ImVec2(w, h));
				auto* dl = ImGui::GetWindowDrawList();
				MsgBox(dl, ImVec2(x, y), ImVec2(x + w, y + h));
				TextC(dl, F.menu, W * 0.5f, y + S(24), kWhite, "FORFEIT THIS GAME?", 0.26f);
				TextC(dl, F.tiny, W * 0.5f, y + S(62), kGrey, Upper(fmt::format("It counts as a loss. {} keeps your {} septims", g_oppName, g_betHeld)), 0.2f);
				switch (MenuButtons("fb", { { "Forfeit", true }, { "Keep playing", false } }, W * 0.5f, y + S(100))) {
				case 0:
					Post(UiAct::ForfeitYes);
					break;
				case 1:
					Post(UiAct::ForfeitNo);
					break;
				default:
					break;
				}
				ImGui::End();
			}
			if (Config::Get().debugOverlay) {
				auto*      dl = ImGui::GetForegroundDrawList();
				const Vec3 corners[4] = { { -kX, -kYh, 0 }, { kX, -kYh, 0 }, { kX, kYh, 0 }, { -kX, kYh, 0 } };
				ImVec2     pts[4];
				bool       ok = true;
				for (int k = 0; k < 4; ++k) {
					ok &= Scene::Project(corners[k], pts[k].x, pts[k].y);
				}
				if (ok) {
					dl->AddPolyline(pts, 4, IM_COL32(80, 255, 120, 220), ImDrawFlags_Closed, 2.0f);
				}
				for (int i = 0; i < 5; ++i) {
					float sx = 0, sy = 0;
					if (Scene::Project(g_drawPos[i], sx, sy)) {
						dl->AddCircle(ImVec2(sx, sy), Scene::PixelsPerUnit(g_drawPos[i]) * 0.9f, IM_COL32(255, 220, 80, 220));
					}
				}
				float mx = 0, my = 0;
				Overlay::MousePos(mx, my);
				double px = 0, py = 0;
				Scene::Unproject(mx, my, 0.0, px, py);
				dl->AddText(ImVec2(mx + 16, my + 16), IM_COL32(255, 255, 255, 255), fmt::format("{:.1f}, {:.1f}  {}", px, py, Scene::Status()).c_str());
			}
		}

		// A chooser line: label, < value >, the arrows step through what's allowed
		template <class Ok>
		void Chooser(ImDrawList* dl, const char* a_id, float cx, float y, float w, const char* a_label, const std::string& a_value, int a_cur, int a_count,
			Ok a_ok, UiAct a_act)
		{
			ImGui::PushID(a_id);
			const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
			Text(dl, F.tiny, ImVec2(x0, y + S(5)), kGrey, a_label, 0.32f);
			int prev = -1, next = -1;
			for (int k = a_cur - 1; k >= 0 && prev < 0; --k) {
				if (a_ok(k)) {
					prev = k;
				}
			}
			for (int k = a_cur + 1; k < a_count && next < 0; ++k) {
				if (a_ok(k)) {
					next = k;
				}
			}
			const float ax0 = x0 + S(110), ax1 = x1 - S(24);
			bool        h0 = false, h1 = false;
			if (Clicked("prev", ImVec2(ax0 - S(4), y - S(2)), ImVec2(ax0 + S(24), y + S(28)), &h0, prev >= 0)) {
				Post(a_act, prev);
			}
			if (Clicked("next", ImVec2(ax1 - S(4), y - S(2)), ImVec2(ax1 + S(24), y + S(28)), &h1, next >= 0)) {
				Post(a_act, next);
			}
			auto arrow = [&](float x, float dir, bool hov, bool on) {
				const ImU32 c = !on ? kDimC : hov ? IM_COL32(255, 255, 255, 255) : kGrey;
				const float m = x + S(10);
				dl->AddTriangleFilled(ImVec2(m + dir * S(6), y + S(13)), ImVec2(m - dir * S(4), y + S(6)), ImVec2(m - dir * S(4), y + S(20)), c);
			};
			arrow(ax0, -1, h0, prev >= 0);
			arrow(ax1, 1, h1, next >= 0);
			TextC(dl, F.turn, (ax0 + ax1 + S(20)) * 0.5f, y, kWhite, Upper(a_value), 0.12f, 0.8f);
			ImGui::PopID();
		}

		// The wager, before the first throw: a full-screen prompt over the darkened room.
		void DrawWager()
		{
			const auto& io = ImGui::GetIO();
			const float W = io.DisplaySize.x, H = io.DisplaySize.y, cx = W * 0.5f;
			auto*       bg = ImGui::GetBackgroundDrawList();
			Vignette(bg, W, H, 0.5f);
			const float w = S(720), h = S(560), x = cx - w * 0.5f, y = H * 0.44f - h * 0.5f;
			BeginArea("##wager", ImVec2(x, y), ImVec2(w, h));
			auto*       dl = ImGui::GetWindowDrawList();
			const auto& cfg = Config::Get();
			const int   gold = GoldHeld();
			const int   top = std::clamp(gold - gold % 5, cfg.minBet, cfg.maxBet);
			const int   bet = std::clamp(g_bet, cfg.minBet, top);
			float       yy = y;
			TextC(dl, F.tiny, cx, yy, kGrey, Upper(fmt::format("A game of Yacht with {}", g_oppName)), 0.6f, 0.8f);
			yy += S(30);
			// the stake, large, with arrows either side
			const std::string stake = fmt::format("{}", bet);
			const float       sw = Measure(F.titleMid, stake, 0.12f);
			TextC(dl, F.titleMid, cx, yy, IM_COL32(255, 255, 255, 255), stake, 0.12f);
			bool h0 = false, h1 = false;
			const float ay = yy + F.titleMid->FontSize * 0.5f;
			if (Clicked("less", ImVec2(cx - sw * 0.5f - S(80), ay - S(24)), ImVec2(cx - sw * 0.5f - S(30), ay + S(24)), &h0, bet > cfg.minBet)) {
				g_bet = (std::max)(cfg.minBet, bet - 5);
			}
			if (Clicked("more", ImVec2(cx + sw * 0.5f + S(30), ay - S(24)), ImVec2(cx + sw * 0.5f + S(80), ay + S(24)), &h1, bet < top)) {
				g_bet = (std::min)(top, bet + 5);
			}
			auto tri = [&](float tx, float dir, bool hov, bool on) {
				const ImU32 c = !on ? kDimC : hov ? IM_COL32(255, 255, 255, 255) : kGrey;
				dl->AddTriangleFilled(ImVec2(tx + dir * S(10), ay), ImVec2(tx - dir * S(8), ay - S(14)), ImVec2(tx - dir * S(8), ay + S(14)), c);
			};
			tri(cx - sw * 0.5f - S(55), -1, h0, bet > cfg.minBet);
			tri(cx + sw * 0.5f + S(55), 1, h1, bet < top);
			yy += F.titleMid->FontSize + S(6);
			TextC(dl, F.tiny, cx, yy, kGrey, Upper(fmt::format("Septims wagered. Win and you take {}", bet * 2)), 0.34f, 0.8f);
			yy += S(22);
			TextC(dl, F.tiny, cx, yy, kDimC, Upper(fmt::format("Your purse: {} septims", gold)), 0.34f, 0.8f);
			yy += S(34);
			DiamondRule(dl, cx, yy, S(560));
			yy += S(28);
			// what you play with
			int cupIdx = 0;
			for (int k = 0; k < kNumCups; ++k) {
				if (PlayerCup().id == std::string(kCups[k].id)) {
					cupIdx = k;
				}
			}
			Chooser(dl, "cup", cx, yy, S(520), "CUP", kCups[cupIdx].name, cupIdx, kNumCups,
				[](int k) { return CupUnlocked(k); }, UiAct::ChooseCup);
			yy += S(40);
			const int diceIdx = (std::max)(0, DiceIndex(PlayerDice()));
			Chooser(dl, "dice", cx, yy, S(520), "DICE", kDiceInfo[diceIdx].name, diceIdx, 15, [](int k) { return DiceOwned(k); }, UiAct::ChooseDice);
			yy += S(40);
			// the item card: what these dice are
			FadeRule(dl, cx - S(260), cx + S(260), yy);
			yy += S(10);
			const ImVec2 bsz = F.sans->CalcTextSizeA(F.sans->FontSize, FLT_MAX, S(480), kDiceInfo[diceIdx].blurb);
			dl->AddText(F.sans, F.sans->FontSize, ImVec2(cx - bsz.x * 0.5f, yy), kGrey, kDiceInfo[diceIdx].blurb, nullptr, S(480));
			yy += bsz.y + S(10);
			FadeRule(dl, cx - S(260), cx + S(260), yy);
			yy += S(12);
			yy += S(34);
			switch (MenuButtons("wb", { { "Place bet", false }, { "Leave", false } }, cx, yy)) {
			case 0:
				Post(UiAct::PlaceBet, bet);
				break;
			case 1:
				Post(UiAct::Leave);
				break;
			default:
				break;
			}
			yy += S(54);
			if (!g_toast.empty() && g_clock - g_toastTime < 2.2) {
				TextC(dl, F.sans, cx, yy, IM_COL32(240, 182, 166, 255), Upper(g_toast), 0.14f, 0.9f);
			}
			ImGui::End();
		}

		// The innkeeper's dice: a list like the inventory's, an item card, and the price.
		int g_shopSel = -1;
		void DrawShop()
		{
			const auto& io = ImGui::GetIO();
			const float W = io.DisplaySize.x, H = io.DisplaySize.y, cx = W * 0.5f;
			auto*       bg = ImGui::GetBackgroundDrawList();
			Vignette(bg, W, H, 0.5f);
			const float w = S(860), h = S(700), x = cx - w * 0.5f, y = H * 0.48f - h * 0.5f;
			BeginArea("##shop", ImVec2(x, y), ImVec2(w, h));
			auto* dl = ImGui::GetWindowDrawList();
			float yy = y;
			TextC(dl, F.tiny, cx, yy, kGrey, Upper(fmt::format("{}'s stock", g_shopName)), 0.6f, 0.8f);
			yy += S(26);
			TextC(dl, F.title, cx, yy, IM_COL32(255, 255, 255, 255), "DICE", 0.16f, 0.9f);
			yy += F.title->FontSize + S(4);
			TextC(dl, F.tiny, cx, yy, kGrey, Upper(fmt::format("Your purse: {} septims  -  {} wins", GoldHeld(), g_totalWins)), 0.34f, 0.8f);
			yy += S(30);
			DiamondRule(dl, cx, yy, S(760));
			yy += S(18);
			// the list (left) and the card (right)
			const float lx0 = x + S(40), lx1 = cx + S(10), rowH = S(29);
			const std::string inUse = PlayerDice();
			if (g_shopSel < 0) {
				g_shopSel = (std::max)(0, DiceIndex(inUse));
				for (int k = 1; k < 15; ++k) {
					if (DiceOnSale(k)) {
						g_shopSel = k;
						break;
					}
				}
			}
			for (int k = 0; k < 15; ++k) {
				const auto&  d = kDiceInfo[k];
				const bool   owned = DiceOwned(k), sale = DiceOnSale(k);
				const ImVec2 ra(lx0, yy + k * rowH), rb(lx1, yy + (k + 1) * rowH);
				bool         hov = false;
				ImGui::PushID(k);
				if (Clicked("d", ra, rb, &hov)) {
					g_shopSel = k;
				}
				ImGui::PopID();
				const bool  sel = g_shopSel == k;
				const ImU32 col = sel || hov ? kWhite : owned || sale ? kGrey : kDimC;
				if (sel) {
					dl->AddRectFilledMultiColor(ra, rb, Alpha(kWhite, 0.1f), Alpha(kWhite, 0.0f), Alpha(kWhite, 0.0f), Alpha(kWhite, 0.1f));
					dl->AddTriangleFilled(ImVec2(ra.x + S(2), ra.y + rowH * 0.5f - S(4)), ImVec2(ra.x + S(8), ra.y + rowH * 0.5f), ImVec2(ra.x + S(2), ra.y + rowH * 0.5f + S(4)),
						kWhite);
				}
				Text(dl, F.sans, ImVec2(ra.x + S(16), ra.y + (rowH - F.sans->FontSize) * 0.5f), col, Upper(d.name), 0.08f, 0.7f);
				const std::string tag = inUse == kDiceSets[k] ? "In use" : owned ? "Owned" : sale ? fmt::format("{} septims", d.gold) : fmt::format("{} wins", d.wins);
				const ImU32 tc = inUse == kDiceSets[k] ? kWhite : sale ? IM_COL32(230, 200, 140, 255) : kDimC;
				Text(dl, F.tiny, ImVec2(rb.x - S(8) - Measure(F.tiny, Upper(tag), 0.14f), ra.y + (rowH - F.tiny->FontSize) * 0.5f), tc, Upper(tag), 0.14f);
			}
			// the card
			const int   k = std::clamp(g_shopSel, 0, 14);
			const auto& d = kDiceInfo[k];
			const float rx0 = cx + S(50), rx1 = x + w - S(30), rcx = (rx0 + rx1) * 0.5f;
			float       ry = yy + S(30);
			TextC(dl, F.turn, rcx, ry, kWhite, Upper(d.name), 0.16f, 0.8f);
			ry += F.turn->FontSize + S(10);
			FadeRule(dl, rx0, rx1, ry);
			ry += S(10);
			const std::string stats = k == 0 ? "STARTING DICE" : fmt::format("PRICE  {}     WINS  {}", d.gold, d.wins);
			TextC(dl, F.tiny, rcx, ry, kGrey, stats, 0.22f);
			ry += S(26);
			const ImVec2 bsz = F.sans->CalcTextSizeA(F.sans->FontSize, FLT_MAX, rx1 - rx0 - S(20), d.blurb);
			dl->AddText(F.sans, F.sans->FontSize, ImVec2(rcx - bsz.x * 0.5f, ry), kGrey, d.blurb, nullptr, rx1 - rx0 - S(20));
			ry += bsz.y + S(12);
			FadeRule(dl, rx0, rx1, ry);
			ry += S(24);
			const bool owned = DiceOwned(k), sale = DiceOnSale(k);
			if (owned && inUse != kDiceSets[k]) {
				if (MenuButton("use", "Use these", rcx, ry)) {
					Post(UiAct::ChooseDice, k);
				}
			} else if (sale) {
				const bool afford = GoldHeld() >= d.gold;
				if (MenuButton("buy", fmt::format("Buy  {}", d.gold), rcx, ry, false, afford)) {
					Post(UiAct::BuyDice, k);
				}
				if (!afford) {
					TextC(dl, F.tiny, rcx, ry + S(48), IM_COL32(240, 182, 166, 255), "NOT ENOUGH GOLD", 0.22f);
				}
			} else if (!owned) {
				TextC(dl, F.tiny, rcx, ry + S(8), kDimC, Upper(fmt::format("For sale after {} wins", d.wins)), 0.22f);
			} else {
				TextC(dl, F.tiny, rcx, ry + S(8), kGrey, "THE DICE YOU PLAY WITH", 0.22f);
			}
			if (!g_toast.empty() && g_clock - g_toastTime < 2.2) {
				TextC(dl, F.sans, rcx, ry + S(80), kWhite, Upper(g_toast), 0.14f, 0.9f);
			}
			// done
			const float by = yy + 15 * rowH + S(26);
			if (MenuButton("done", "Done", cx, by)) {
				Post(UiAct::CloseShop);
			}
			ImGui::End();
		}

		// The result, as Skyrim's big notices: a kicker, the verdict huge, a rule with a diamond, the score.
		void DrawOver()
		{
			const auto& io = ImGui::GetIO();
			const float W = io.DisplaySize.x, H = io.DisplaySize.y, cx = W * 0.5f;
			auto*       bg = ImGui::GetBackgroundDrawList();
			Vignette(bg, W, H, 0.5f);
			const float w = S(820), h = S(440), x = cx - w * 0.5f, y = H * 0.42f - h * 0.5f;
			BeginArea("##over", ImVec2(x, y), ImVec2(w, h));
			auto*       dl = ImGui::GetWindowDrawList();
			const bool  win = g_outcome == "win";
			const bool  lose = g_outcome == "lose" || g_outcome == "forfeit";
			const char* title = win ? "YOU WIN" : g_outcome == "draw" ? "A DRAW" : g_outcome == "forfeit" ? "FORFEIT" : "YOU LOSE";
			float       yy = y;
			TextC(dl, F.tiny, cx, yy, kGrey, "GAME OVER", 0.6f, 0.8f);
			yy += S(26);
			const ImU32 tc = lose ? IM_COL32(240, 195, 182, 255) : IM_COL32(255, 255, 255, 255);
			const float tw = Measure(F.title, title, 0.16f) - 0.16f * F.title->FontSize;
			Glow(dl, F.title, ImVec2(cx - tw * 0.5f, yy), Alpha(lose ? IM_COL32(208, 106, 82, 255) : kWhite, 0.05f), title, 0.16f, S(8));
			TextC(dl, F.title, cx, yy, tc, title, 0.16f, 0.9f);
			yy += F.title->FontSize + S(14);
			DiamondRule(dl, cx, yy, S(560));
			yy += S(20);
			if (g_outcome != "forfeit") {
				TextC(dl, F.score, cx, yy, kWhite, fmt::format("{}   -   {}", g_cards[0].Total(), g_cards[1].Total()), 0.18f, 0.8f);
				yy += F.score->FontSize + S(10);
			}
			TextC(dl, F.tiny, cx, yy, kGrey, Upper(g_payoutText), 0.34f, 0.8f);
			yy += S(24);
			const auto& r = g_records[g_oppId];
			TextC(dl, F.tiny, cx, yy, kGrey,
				Upper(fmt::format("Your record against {}: {}-{}{}", g_oppName, r.w, r.l, r.d ? fmt::format(", {} drawn", r.d) : "")), 0.34f, 0.8f);
			yy += S(24);
			TextC(dl, F.tiny, cx, yy, kDimC, Upper(fmt::format("Your cup: the {}", PlayerCup().name)), 0.34f, 0.8f);
			yy += S(44);
			switch (MenuButtons("ob", { { "Rematch", false }, { "Leave", false } }, cx, yy)) {
			case 0:
				Post(UiAct::Rematch);
				break;
			case 1:
				Post(UiAct::Leave);
				break;
			default:
				break;
			}
			if (!g_toast.empty() && g_clock - g_toastTime < 2.2) {
				TextC(dl, F.sans, cx, yy + S(60), kWhite, Upper(g_toast), 0.14f, 0.9f);
			}
			ImGui::End();
		}

		// Skyrim's pointer: a small off-white arrow with a dark edge.
		void Cursor()
		{
			auto&        io = ImGui::GetIO();
			auto*        dl = ImGui::GetForegroundDrawList();
			const ImVec2 m = io.MousePos;
			const float  s = S(1.0f);
			const ImVec2 p[7] = { { m.x, m.y }, { m.x, m.y + 22 * s }, { m.x + 5.5f * s, m.y + 17 * s }, { m.x + 9.5f * s, m.y + 25 * s },
				{ m.x + 12.5f * s, m.y + 23.5f * s }, { m.x + 8.5f * s, m.y + 15.5f * s }, { m.x + 15.5f * s, m.y + 15.5f * s } };
			// the head and the tail, each convex
			const ImU32 fill = IM_COL32(241, 238, 232, 245);
			dl->AddTriangleFilled(p[0], p[1], p[6], fill);
			dl->AddQuadFilled(p[2], p[3], p[4], p[5], fill);
			dl->AddPolyline(p, 7, IM_COL32(20, 16, 12, 255), ImDrawFlags_Closed, 1.4f * s);
		}
	}

	// Diagnostics: is our line in the conversation's topic list, and does each response's condition pass for this speaker?
	void LogDialogue(RE::Actor* a_speaker)
	{
		auto* mtm = RE::MenuTopicManager::GetSingleton();
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!a_speaker || !mtm || !dh) {
			return;
		}
		std::size_t listed = 0;
		bool        offered = false;
		std::string names;
		if (mtm->dialogueList) {
			for (auto* dlg : *mtm->dialogueList) {
				++listed;
				if (dlg) {
					names += fmt::format(" '{}'", dlg->topicText.c_str());
				}
				if (dlg && dlg->parentTopic && dlg->parentTopic->GetFormID() == dh->LookupFormID(EspIds::kTopicPlay, EspIds::kPlugin)) {
					offered = true;
				}
			}
		}
		auto* topic = dh->LookupForm<RE::TESTopic>(EspIds::kTopicPlay, EspIds::kPlugin);
		auto* branch = dh->LookupForm<RE::BGSDialogueBranch>(EspIds::kBranchPlay, EspIds::kPlugin);
		if (!topic) {
			spdlog::info("  dialogue: YA_TopicPlay missing ({} topics in menu)", listed);
			return;
		}
		spdlog::info("  dialogue: {} topics in menu, ours {}; topic quest {:08X} running {}, branch {:08X}, infos {}, type {}, subtype {}",
			listed, offered ? "SHOWN" : "hidden", topic->ownerQuest ? topic->ownerQuest->GetFormID() : 0,
			topic->ownerQuest && topic->ownerQuest->IsRunning(), topic->ownerBranch ? topic->ownerBranch->GetFormID() : 0, topic->numTopicInfos,
			static_cast<int>(topic->data.type.get()), static_cast<int>(topic->data.subtype.get()));
		spdlog::info("  menu:{}", names);
		if (branch) {
			spdlog::info("  branch: quest {:08X}, start topic {:08X}, flags {:X}, type {}", branch->quest ? branch->quest->GetFormID() : 0,
				branch->startingTopic ? branch->startingTopic->GetFormID() : 0, branch->flags.underlying(), static_cast<int>(branch->type));
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
			auto* info = topic->topicInfos ? topic->topicInfos[i] : nullptr;
			if (!info) {
				continue;
			}
			std::string items;
			for (auto* c = info->objConditions.head; c; c = c->next) {
				RE::ConditionCheckParams params(a_speaker, player);
				items += fmt::format(" f{}={}", static_cast<int>(c->data.functionData.function.get()), c->IsTrue(params) ? 1 : 0);
			}
			spdlog::info("  info {:08X}: all {}{}", info->GetFormID(), info->objConditions.IsTrue(a_speaker, player) ? 1 : 0, items);
		}
	}

	namespace
	{
		// The dice sets go in every innkeeper's shop chest itself (the chest's own list, so the game's restocks keep
		// them): done once, as the game loads its data
		void DiceIntoShops()
		{
			if (!g_innkeeper) {
				return;
			}
			std::set<RE::TESObjectCONT*> chests;
			std::set<RE::BGSListForm*>   lists;
			int                          everything = 0;
			auto* kw = RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSKeyword>(EspIds::kVendorItemDice, EspIds::kPlugin);
			for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>()) {
				if (!npc) {
					continue;
				}
				bool keeper = false;
				for (const auto& fr : npc->factions) {
					keeper |= fr.faction == g_innkeeper;
				}
				if (!keeper) {
					continue;
				}
				for (const auto& fr : npc->factions) {
					auto* ref = fr.faction && fr.faction->IsVendor() ? fr.faction->vendorData.merchantContainer : nullptr;
					// a shop only shows what its buy/sell list names: add the dice's keyword to it
					if (ref) {
						const auto& vd = fr.faction->vendorData;
						if (vd.vendorSellBuyList && !vd.vendorValues.notBuySell) {
							lists.insert(vd.vendorSellBuyList);
						} else if (!vd.vendorSellBuyList) {
							++everything;
						}
					}
					auto* base = ref ? ref->GetBaseObject() : nullptr;
					if (auto* cont = base ? base->As<RE::TESObjectCONT>() : nullptr) {
						chests.insert(cont);
					}
				}
			}
			for (auto* cont : chests) {
				for (int k = 1; k < 15; ++k) {
					if (g_diceItem[k] && cont->CountObjectsInContainer(g_diceItem[k]) == 0) {
						cont->AddObjectToContainer(g_diceItem[k], 1, nullptr);
					}
				}
			}
			int added = 0;
			for (auto* list : lists) {
				if (kw && !list->HasForm(kw)) {
					list->AddForm(kw);
					++added;
				}
			}
			spdlog::info("Dice: in {} innkeepers' shop chests; their keyword added to {} buy/sell lists ({} shops deal in anything)",
				chests.size(), added, everything);
		}

		// Other mods hand out keywords by name (KID): "Dragonbone" picked up a dragon-bone icon. What they give every set
		// alike stays (the inventory's "Clutter" type comes that way); what only some sets got by their name comes off.
		void KeepDiceKeywords()
		{
			static bool s_done = false;
			if (s_done) {
				return;
			}
			s_done = true;
			std::map<RE::BGSKeyword*, int> seen;
			int                            sets = 0;
			for (int k = 1; k < 15; ++k) {
				if (auto* item = g_diceItem[k]) {
					++sets;
					for (std::uint32_t i = 0; i < item->numKeywords; ++i) {
						if (item->keywords[i]) {
							++seen[item->keywords[i]];
						}
					}
				}
			}
			for (int k = 1; k < 15; ++k) {
				auto* item = g_diceItem[k];
				if (!item) {
					continue;
				}
				std::vector<RE::BGSKeyword*> odd;
				for (std::uint32_t i = 0; i < item->numKeywords; ++i) {
					if (auto* kw = item->keywords[i]; kw && seen[kw] < sets) {
						odd.push_back(kw);
					}
				}
				for (auto* kw : odd) {
					spdlog::info("Dice: {} had keyword {} from another mod, by its name: removed", kDiceInfo[k].name, kw->GetFormEditorID());
				}
				if (!odd.empty()) {
					item->RemoveKeywords(odd);
				}
			}
		}

		// A shop marks goods up by your Speech (about x3.3 at 0, x2 at 100): the dice's value is set so the price you
		// see is the set's own (Iron 50 ... Daedric 2500), whatever your Speech
		void PriceDice()
		{
			float hi = 3.3f, lo = 2.0f;
			if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
				if (const auto it = gs->settings.find("fBarterMax"); it != gs->settings.end() && it->second) {
					hi = it->second->GetFloat();
				}
				if (const auto it = gs->settings.find("fBarterMin"); it != gs->settings.end() && it->second) {
					lo = it->second->GetFloat();
				}
			}
			auto*       pc = RE::PlayerCharacter::GetSingleton();
			const float speech = pc ? std::clamp(pc->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech), 0.0f, 100.0f) : 15.0f;
			const float mark = (std::max)(1.0f, hi - (hi - lo) * speech / 100.0f);
			for (int k = 1; k < 15; ++k) {
				if (g_diceItem[k]) {
					g_diceItem[k]->value = (std::max)(1, static_cast<std::int32_t>(std::lround(kDiceInfo[k].gold / mark)));
				}
			}
			spdlog::info("Dice: priced for Speech {:.0f} (mark-up x{:.2f})", speech, mark);
		}

		// The sets not on sale yet (wins) or already yours come out of the shop as it opens
		void HideUnsold(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
			const auto counts = a_ref->GetInventoryCounts([](RE::TESBoundObject& a_obj) {
				for (int k = 1; k < 15; ++k) {
					if (g_diceItem[k] == &a_obj) {
						return true;
					}
				}
				return false;
			});
			std::lock_guard lk(g_mtx);
			int shown = 0;
			for (int k = 1; k < 15; ++k) {
				const auto it = counts.find(g_diceItem[k]);
				const int  have = it == counts.end() ? 0 : it->second;
				if (have > 0 && !DiceOnSale(k)) {
					a_ref->RemoveItem(g_diceItem[k], have, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				} else {
					shown += have > 0;
				}
			}
			spdlog::info("Dice: {} sets on show in {:08X}", shown, a_ref->GetFormID());
		}

		// After a trade: any set in your pack is yours for good (and in use at the next table)
		void ClaimBoughtDice()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc) {
				return;
			}
			auto counts = pc->GetInventoryCounts([](RE::TESBoundObject& a_obj) {
				for (int k = 1; k < 15; ++k) {
					if (g_diceItem[k] == &a_obj) {
						return true;
					}
				}
				return false;
			});
			std::lock_guard lk(g_mtx);
			for (int k = 1; k < 15; ++k) {
				const auto it = counts.find(g_diceItem[k]);
				if (!g_diceItem[k] || it == counts.end() || it->second <= 0 || DiceOwned(k)) {
					continue;
				}
				g_ownedDice |= 1u << k;
				g_diceSel = kDiceSets[k];
				RE::DebugNotification(fmt::format("{} dice: yours. You'll play with them at the next table.", kDiceInfo[k].name).c_str());
				spdlog::info("Bought {} dice", kDiceInfo[k].name);
			}
		}
	}

	// ==================================================================== public
	void OnDataLoaded()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		g_faction = dh->LookupForm<RE::TESFaction>(EspIds::kTableFaction, EspIds::kPlugin);
		g_canAfford = dh->LookupForm<RE::TESGlobal>(EspIds::kCanAfford, EspIds::kPlugin);
		g_infoPlay = dh->LookupFormID(EspIds::kInfoPlay, EspIds::kPlugin);
		g_innkeeper = RE::TESForm::LookupByID<RE::TESFaction>(0x0005091B);  // JobInnkeeperFaction
		{
			int found = 0;
			for (int k = 1; k < 15; ++k) {
				g_diceItem[k] = dh->LookupForm<RE::TESObjectMISC>(EspIds::kDiceIron + (k - 1), EspIds::kPlugin);
				found += g_diceItem[k] ? 1 : 0;
			}
			spdlog::info("Dice for sale: {} of 14 items, innkeeper faction {}", found, g_innkeeper != nullptr);
		}
		DiceIntoShops();
		g_diceShop = dh->LookupForm<RE::TESGlobal>(EspIds::kDiceShop, EspIds::kPlugin);
		g_gold = RE::TESForm::LookupByID<RE::TESObjectMISC>(0x0000000F);
		spdlog::info("Forms: faction {}, global {}, info {:X}, gold {}", g_faction != nullptr, g_canAfford != nullptr, g_infoPlay, g_gold != nullptr);
		if (!g_faction || !g_canAfford || !g_infoPlay) {
			spdlog::error("{} records missing (faction {}, global {}, info {:X}) - is the plugin enabled?", EspIds::kPlugin, g_faction != nullptr,
				g_canAfford != nullptr, g_infoPlay);
		}
		{
			auto* topic = dh->LookupForm<RE::TESTopic>(EspIds::kTopicPlay, EspIds::kPlugin);
			auto* play = dh->LookupForm<RE::TESTopicInfo>(EspIds::kInfoPlay, EspIds::kPlugin);
			auto* poor = dh->LookupForm<RE::TESTopicInfo>(EspIds::kInfoPoor, EspIds::kPlugin);
			spdlog::info("Dialogue records: topic {}, responses loaded {}, play line {}, poor line {}", topic != nullptr,
				topic ? topic->numTopicInfos : 0, play != nullptr, poor != nullptr);
		}
		Scene::Register();
	}

	namespace
	{
		enum class QuestFix { Idle, Stopping, Starting };
		QuestFix                              g_questFix = QuestFix::Idle;
		std::chrono::steady_clock::time_point g_questFixAt{};

		void QuestFixTick()
		{
			if (g_questFix == QuestFix::Idle) {
				return;
			}
			auto*      dh = RE::TESDataHandler::GetSingleton();
			auto*      quest = dh->LookupForm<RE::TESQuest>(EspIds::kQuest, EspIds::kPlugin);
			auto*      topic = dh->LookupForm<RE::TESTopic>(EspIds::kTopicPlay, EspIds::kPlugin);
			const auto now = std::chrono::steady_clock::now();
			if (!quest) {
				g_questFix = QuestFix::Idle;
				return;
			}
			if (g_questFix == QuestFix::Stopping) {
				if (!quest->IsRunning() && !quest->IsStopping()) {
					Call(quest, "Start");
					g_questFix = QuestFix::Starting;
					g_questFixAt = now;
				} else if (now - g_questFixAt > 10s) {
					spdlog::warn("Dialogue quest: didn't stop");
					g_questFix = QuestFix::Idle;
				}
				return;
			}
			if ((quest->IsRunning() && !quest->IsStarting()) || now - g_questFixAt > 10s) {
				spdlog::info("Dialogue quest: {}, {} responses loaded, play line {}", quest->IsRunning() ? "running" : "NOT running",
					topic ? topic->numTopicInfos : 0, dh->LookupForm<RE::TESTopicInfo>(EspIds::kInfoPlay, EspIds::kPlugin) != nullptr);
				g_questFix = QuestFix::Idle;
			}
		}
	}

	void OnGameLoaded()
	{
		// The dialogue's quest starts with the game; in a save made before the mod was added it may not be running yet.
		auto* quest = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(EspIds::kQuest, EspIds::kPlugin);
		if (!quest) {
			spdlog::error("YA_Quest is missing - no dialogue. Is {} enabled?", EspIds::kPlugin);
		} else {
			// A quest the game started by itself (Start Game Enabled, added to an existing save) can be running
			// without its dialogue loaded - the responses only come in when the quest is started properly.
			// So if they're missing, stop it and start it again (FrameTick waits for the stop to finish).
			auto*      topic = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESTopic>(EspIds::kTopicPlay, EspIds::kPlugin);
			const auto loaded = topic ? topic->numTopicInfos : 0;
			const bool running = quest->IsRunning();
			spdlog::info("Dialogue quest: {}, {} responses loaded", running ? "running" : "not running", loaded);
			if (!running) {
				Call(quest, "Start");
				g_questFix = QuestFix::Starting;
			} else if (loaded == 0) {
				spdlog::info("Dialogue quest: restarting it so its dialogue loads");
				Call(quest, "Stop");
				g_questFix = QuestFix::Stopping;
			}
			g_questFixAt = std::chrono::steady_clock::now();
		}
		g_tagged.clear();
		if (Scene::IsOpen()) {
			Scene::Close();
		}
		g_phase = Phase::Idle;
	}

	void FrameTick()
	{
		{
			static auto s_prev = std::chrono::steady_clock::now();
			const auto  now = std::chrono::steady_clock::now();
			Audio::Tick(std::clamp(std::chrono::duration<float>(now - s_prev).count(), 0.0f, 0.1f));
			s_prev = now;
			Audio::SetMusic(Scene::IsOpen());
		}
		Scene::TeardownTick();
		QuestFixTick();
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}
		// the opponent said yes ("Pull up a chair..."): note who, while the line is being spoken
		if (ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
			auto* mtm = RE::MenuTopicManager::GetSingleton();
			auto* info = mtm ? mtm->currentTopicInfo : nullptr;
			if (info && g_infoPlay && info->GetFormID() == g_infoPlay && !g_pendingSpeaker.load()) {
				auto ref = mtm->speaker.get();
				if (ref) {
					g_pendingSpeaker = ref->GetFormID();
					spdlog::info("{} agreed to a game", ref->GetDisplayFullName());
				}
			}
		}
		// the innkeeper's "Have a look": the dice shop, once the conversation has closed
		if (ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
			auto* mtm = RE::MenuTopicManager::GetSingleton();
			auto* info = mtm ? mtm->currentTopicInfo : nullptr;
			if (info && g_infoDiceSale && info->GetFormID() == g_infoDiceSale && !g_pendingShop.load()) {
				if (auto ref = mtm->speaker.get()) {
					g_pendingShop = ref->GetFormID();
				}
			}
		}
		if (const auto id = g_pendingShop.load(); id && !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
			g_pendingShop = 0;
			std::lock_guard lk(g_mtx);
			if (!Scene::IsOpen()) {
				auto* keeper = RE::TESForm::LookupByID<RE::Actor>(id);
				g_shopName = keeper && keeper->GetDisplayFullName() ? keeper->GetDisplayFullName() : "The innkeeper";
				g_phase = Phase::Shop;
				g_shopSel = -1;
				Scene::Open(false);
				spdlog::info("Dice shop with {}", g_shopName);
			}
		}
		// ...and open the table once the conversation has closed
		if (const auto id = g_pendingSpeaker.load(); id && !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
			g_pendingSpeaker = 0;
			spdlog::info("Opening the table");
			std::lock_guard lk(g_mtx);
			StartWith(RE::TESForm::LookupByID<RE::Actor>(id));
		}
		// what the dialogue sees, each time a conversation starts (to tell why the topic shows or doesn't)
		{
			static bool s_talking = false;
			static bool s_logged = true;
			static auto s_openedAt = std::chrono::steady_clock::now();
			const bool  talking = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
			if (talking && !s_talking) {
				auto* mtm = RE::MenuTopicManager::GetSingleton();
				auto  ref = mtm ? mtm->speaker.get() : nullptr;
				auto* a = ref ? ref->As<RE::Actor>() : nullptr;
				auto* quest = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(EspIds::kQuest, EspIds::kPlugin);
				if (a) {
					auto* st = a->AsActorState();
					spdlog::info("Talking to {} ({:08X}): in table faction {}, sitting {}, tagged {}, can afford {}, quest running {}",
						a->GetDisplayFullName(), a->GetFormID(), g_faction && a->IsInFaction(g_faction),
						st ? static_cast<int>(st->GetSitSleepState()) : -1, g_tagged.contains(a->GetFormID()), g_canAfford ? g_canAfford->value : -1.0f,
						quest && quest->IsRunning());
					LogDialogue(a);
				}
				s_openedAt = std::chrono::steady_clock::now();
				s_logged = false;
			}
			// once more after the menu has settled, in case the topic list fills in late
			if (talking && !s_logged && std::chrono::steady_clock::now() - s_openedAt > 700ms) {
				s_logged = true;
				auto* mtm = RE::MenuTopicManager::GetSingleton();
				auto  ref = mtm ? mtm->speaker.get() : nullptr;
				LogDialogue(ref ? ref->As<RE::Actor>() : nullptr);
			}
			s_talking = talking;
		}
		// a trade just closed: claim any dice bought
		{
			static bool s_barter = false;
			static RE::RefHandle s_barterTarget = 0;
			const bool  barter = ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
			if (s_barter && !barter) {
				ClaimBoughtDice();
				s_barterTarget = 0;
			}
			if (barter && !s_barter) {
				auto* mtm = RE::MenuTopicManager::GetSingleton();
				auto  ref = mtm ? mtm->speaker.get() : nullptr;
				auto* a = ref ? ref->As<RE::Actor>() : nullptr;
				auto* vendor = a ? a->GetVendorFaction() : nullptr;
				auto* chest = vendor ? vendor->vendorData.merchantContainer : nullptr;
				int   dice = 0;
				if (chest) {
					for (const auto& [obj, n] : chest->GetInventoryCounts()) {
						for (int k = 1; k < 15; ++k) {
							dice += g_diceItem[k] == obj ? n : 0;
						}
					}
				}
				auto target = RE::BarterMenu::GetTargetRefHandle();
				s_barterTarget = target;
				auto tref = RE::TESObjectREFR::LookupByHandle(target);
				spdlog::info("Shop opened: {} (chest {:08X}, menu shows {:08X}): {} dice sets in the chest", a ? a->GetDisplayFullName() : "?",
					chest ? chest->GetFormID() : 0, tref ? tref->GetFormID() : 0, dice);
				// what isn't on sale yet comes out (wherever the shop's goods are while you trade), then the list reloads
				if (a && g_innkeeper && a->IsInFaction(g_innkeeper)) {
					KeepDiceKeywords();
					PriceDice();
					HideUnsold(chest);
					HideUnsold(tref.get());
					SKSE::GetTaskInterface()->AddUITask([]() {
						if (auto menu = RE::UI::GetSingleton()->GetMenu<RE::BarterMenu>()) {
							if (auto* list = menu->GetRuntimeData().itemList) {
								list->Update();
								spdlog::info("Shop: list refreshed with the dice");
							}
						}
					});
				}
			}
			s_barter = barter;
		}
		if (ui->GameIsPaused() || Scene::IsOpen()) {
			return;
		}
		static auto s_last = std::chrono::steady_clock::now() - 10s;
		const auto  now = std::chrono::steady_clock::now();
		if (now - s_last < 1s) {
			return;
		}
		s_last = now;
		if (g_canAfford) {
			g_canAfford->value = GoldHeld() >= Config::Get().minBet ? 1.0f : 0.0f;
		}
		if (g_diceShop) {
			std::lock_guard lk(g_mtx);
			g_diceShop->value = g_games == 0 && g_totalWins == 0 ? 0.0f : AnyDiceOnSale() ? 2.0f : 1.0f;
		}
		TagSeated();
	}

	void Tick(float a_dt)
	{
		std::lock_guard lk(g_mtx);
		const double    dt = std::clamp(static_cast<double>(a_dt), 0.001, 0.05);
		if (g_phase == Phase::Shop) {
			for (int key : Overlay::TakeKeys()) {
				if (key == 0x01 || key == 0x0F) {  // Esc, Tab
					Post(UiAct::CloseShop);
				}
			}
		}
		if (!Scene::Ready()) {
			g_clock += dt;    // the interface's notices and fades keep time
			ProcessUiActs();  // the wager can be placed (or the table left) while it loads
			return;
		}
		Step(dt);
	}

	void Abort()
	{
		std::lock_guard lk(g_mtx);
		spdlog::error("The table hit a problem and was closed; your wager was returned");
		CloseGame();
	}

	void SetupUi(float a_screenH)
	{
		g_ui = (std::max)(0.6f, a_screenH / 1080.0f) * Config::Get().uiScale;
		auto& io = ImGui::GetIO();
		char  windir[MAX_PATH]{};
		::GetWindowsDirectoryA(windir, MAX_PATH);
		const std::string ours = "Data\\SKSE\\Plugins\\SkyrimYacht\\Fonts\\";
		const std::string sys = std::string(windir) + "\\Fonts\\";
		std::error_code   ec;
		static const ImWchar kCaps[] = { 0x0020, 0x007E, 0 };  // the huge display faces only need plain ASCII
		auto load = [&](const char* a_file, const char* a_fallback, float a_size, const ImWchar* a_ranges = nullptr) -> ImFont* {
			std::string p = ours + a_file;
			if (!std::filesystem::exists(p, ec)) {
				p = sys + a_fallback;
				if (!std::filesystem::exists(p, ec)) {
					return nullptr;
				}
			}
			ImFontConfig fc;
			fc.OversampleH = 2;
			fc.OversampleV = 1;
			return io.Fonts->AddFontFromFileTTF(p.c_str(), a_size * g_ui, &fc, a_ranges);
		};
		F.sans = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 20.0f);
		F.sansSemi = load("BarlowCondensed-SemiBold.ttf", "arialnb.ttf", 13.0f);
		F.tiny = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 15.0f);
		F.turn = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 21.0f);
		F.menu = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 24.0f);
		F.toastLong = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 26.0f, kCaps);
		F.skill = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 30.0f, kCaps);
		F.score = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 32.0f, kCaps);
		F.toast = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 44.0f, kCaps);
		F.titleMid = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 76.0f, kCaps);
		F.words = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 92.0f, kCaps);
		F.title = load("BarlowCondensed-Medium.ttf", "arialn.ttf", 100.0f, kCaps);
		F.book = load("IMFellEnglish-Regular.ttf", "georgia.ttf", 18.0f);
		F.bookIt = load("IMFellEnglish-Italic.ttf", "georgiai.ttf", 16.0f);
		F.bookSC = load("IMFellEnglishSC-Regular.ttf", "georgia.ttf", 27.0f);
		F.bookSCsm = load("IMFellEnglishSC-Regular.ttf", "georgia.ttf", 17.0f);
		F.hand = load("Fondamento-Regular.ttf", "georgiai.ttf", 21.0f);
		F.handBig = load("Fondamento-Regular.ttf", "georgiai.ttf", 40.0f);
		ImFont* any = F.sans ? F.sans : io.Fonts->AddFontDefault();
		for (ImFont** f : { &F.sans, &F.sansSemi, &F.tiny, &F.turn, &F.menu, &F.toastLong, &F.skill, &F.score, &F.toast, &F.titleMid, &F.words,
				 &F.title, &F.book, &F.bookIt, &F.bookSC, &F.bookSCsm, &F.hand, &F.handBig }) {
			if (!*f) {
				*f = any;
			}
		}
		io.FontDefault = F.sans;
		spdlog::info("Interface: scale {:.2f}, fonts {}", g_ui, std::filesystem::exists(ours + "BarlowCondensed-Medium.ttf", ec) ? "ours" : "Windows'");
		auto& st = ImGui::GetStyle();
		st.WindowRounding = 0.0f;
		st.WindowBorderSize = 0.0f;
		st.WindowPadding = ImVec2(0, 0);
		st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
		st.Colors[ImGuiCol_Text] = ImVec4(0.945f, 0.933f, 0.91f, 1.0f);
	}

	void DrawUi()
	{
		std::lock_guard lk(g_mtx);
		if (!Scene::IsOpen()) {
			return;
		}
		if (!g_artTried) {
			g_artTried = true;
			for (int k = 0; k < 2; ++k) {
				std::ifstream f(k == 0 ? "Data\\SKSE\\Plugins\\SkyrimYacht\\UI\\parchment_b.rgba" : "Data\\SKSE\\Plugins\\SkyrimYacht\\UI\\parchment_a.rgba",
					std::ios::binary);
				std::uint32_t wh[2]{};
				if (f && f.read(reinterpret_cast<char*>(wh), 8) && wh[0] > 0 && wh[0] <= 4096 && wh[1] > 0 && wh[1] <= 4096) {
					std::vector<char> px(static_cast<std::size_t>(wh[0]) * wh[1] * 4);
					if (f.read(px.data(), static_cast<std::streamsize>(px.size()))) {
						g_parchment[k] = Overlay::CreateTexture(static_cast<int>(wh[0]), static_cast<int>(wh[1]), px.data());
					}
				}
			}
			spdlog::info("Interface: parchment {}", g_parchment[0] && g_parchment[1] ? "loaded" : "missing (plain sheets)");
		}
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = false;
		const float W = io.DisplaySize.x, H = io.DisplaySize.y;
		if (!Scene::Ready() && g_phase != Phase::Shop && g_phase != Phase::Wager) {
			// like the game's own loading prompt, bottom right
			auto*       dl = ImGui::GetForegroundDrawList();
			const auto  t = std::string("SETTING UP THE TABLE");
			const float tw = Measure(F.sans, t, 0.4f);
			Text(dl, F.sans, ImVec2(W - S(60) - S(30) - tw, H - S(50) - F.sans->FontSize), kGrey, t, 0.4f, 0.9f);
			const float a = static_cast<float>(Now() * 2.0);
			const ImVec2 c(W - S(60) - S(7), H - S(50) - F.sans->FontSize * 0.5f);
			ImVec2       q[4];
			for (int k = 0; k < 4; ++k) {
				q[k] = ImVec2(c.x + std::cos(a + k * 1.5708f) * S(10), c.y + std::sin(a + k * 1.5708f) * S(10));
			}
			dl->AddQuad(q[0], q[1], q[2], q[3], kWhite, 1.5f * g_ui);
		}
		switch (g_phase) {
		case Phase::Wager:
			DrawWager();
			break;
		case Phase::Over:
			DrawPlay();
			DrawOver();
			break;
		case Phase::Shop:
			DrawShop();
			break;
		case Phase::Idle:
			break;
		default:
			DrawPlay();
			break;
		}
		Cursor();
	}

	// ---------------------------------------------------------------- co-save: records per opponent, last score, wins
	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (!a_intfc->OpenRecord('YREC', 2)) {
			return;
		}
		const std::uint32_t n = static_cast<std::uint32_t>(g_records.size());
		a_intfc->WriteRecordData(g_lastScore);
		a_intfc->WriteRecordData(g_totalWins);
		a_intfc->WriteRecordData(n);
		for (const auto& [id, r] : g_records) {
			a_intfc->WriteRecordData(id);
			a_intfc->WriteRecordData(r.w);
			a_intfc->WriteRecordData(r.l);
			a_intfc->WriteRecordData(r.d);
		}
		// version 2: the cups and dice
		a_intfc->WriteRecordData(g_bestScore);
		a_intfc->WriteRecordData(g_games);
		a_intfc->WriteRecordData(g_ownedDice);
		for (const std::string* str : { &g_cupSel, &g_diceSel }) {
			const std::uint32_t len = static_cast<std::uint32_t>(str->size());
			a_intfc->WriteRecordData(len);
			if (len) {
				a_intfc->WriteRecordData(str->data(), len);
			}
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		std::uint32_t   type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != 'YREC') {
				continue;
			}
			std::uint32_t n = 0;
			a_intfc->ReadRecordData(g_lastScore);
			a_intfc->ReadRecordData(g_totalWins);
			a_intfc->ReadRecordData(n);
			g_records.clear();
			for (std::uint32_t k = 0; k < n; ++k) {
				RE::FormID id = 0, fixed = 0;
				Record     r;
				a_intfc->ReadRecordData(id);
				a_intfc->ReadRecordData(r.w);
				a_intfc->ReadRecordData(r.l);
				a_intfc->ReadRecordData(r.d);
				if (a_intfc->ResolveFormID(id, fixed)) {
					g_records[fixed] = r;
				}
			}
			if (version >= 2) {
				a_intfc->ReadRecordData(g_bestScore);
				a_intfc->ReadRecordData(g_games);
				a_intfc->ReadRecordData(g_ownedDice);
				for (std::string* str : { &g_cupSel, &g_diceSel }) {
					std::uint32_t len = 0;
					a_intfc->ReadRecordData(len);
					str->assign((std::min)(len, 64u), '\0');
					if (len) {
						a_intfc->ReadRecordData(str->data(), (std::min)(len, 64u));
					}
				}
			} else {
				// saves from the test builds: the sets you'd already been given, and your last score as your best
				g_bestScore = g_lastScore;
				g_games = static_cast<int>(g_records.size());
				for (int k = 0; k <= (std::min)(14, g_totalWins / 10); ++k) {
					g_ownedDice |= 1u << k;
				}
			}
			g_ownedDice |= 1u;
		}
		spdlog::info("Loaded: last score {}, {} wins, {} opponents", g_lastScore, g_totalWins, g_records.size());
	}

	void Revert(SKSE::SerializationInterface*)
	{
		std::lock_guard lk(g_mtx);
		g_records.clear();
		g_lastScore = 0;
		g_totalWins = 0;
		g_bestScore = 0;
		g_games = 0;
		g_ownedDice = 1;
		g_cupSel.clear();
		g_diceSel.clear();
	}
}
