// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Game.h"

#include "Config.h"
#include "EspIds.h"
#include "Overlay.h"
#include "Scene.h"

#include "../sim/Ai.h"
#include "../sim/Dice.h"
#include "../sim/Yacht.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

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
			const char* rank;
			int         need;    // last finished game's score
			double      height;  // inside depth (the vanilla meshes, scaled)
		};
		constexpr CupInfo kCups[] = {
			{ "tankard", "Tankard", "Novice", 0, 4.21 },
			{ "goblet", "Goblet", "Apprentice", 150, 3.82 },
			{ "dwemer", "Dwemer Cup", "Adept", 200, 5.10 },
			{ "silver", "Silver Goblet", "Expert", 250, 4.62 },
			{ "golden", "Golden Goblet", "Master", 300, 6.01 },
		};
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
			Over
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

		// remembered in the save
		std::unordered_map<RE::FormID, Record> g_records;
		int g_lastScore = 0;
		int g_totalWins = 0;

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
			Shout
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

		const CupInfo& CupById(const std::string& a_id)
		{
			for (const auto& c : kCups) {
				if (a_id == c.id) {
					return c;
				}
			}
			return kCups[0];
		}
		const CupInfo& PlayerCup()
		{
			const CupInfo* best = &kCups[0];
			for (const auto& c : kCups) {
				if (g_lastScore >= c.need) {
					best = &c;
				}
			}
			return *best;
		}
		std::string PlayerDice()
		{
			if (!Config::Get().diceSet.empty()) {
				return Config::Get().diceSet;
			}
			// one set more every ten wins (innkeepers selling them comes later; for now you just have them)
			const int k = std::clamp(g_totalWins / 10, 0, 14);
			return kDiceSets[k];
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
			Scene::UseDice(a_owner == 0 ? PlayerDice() : "bone");
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
		}
		void PlaceOnPad(int i, double x, double y, bool a_drop)
		{
			TweenFrom(i, a_drop ? 0.12 : 0.25);
			g_kept[i] = false;
			g_rackSlot[i] = -1;
			g_sim.PlaceDie(i, { Clamp(x, -kX + kHalf, kX - kHalf), Clamp(y, -kYh + kHalf, kYh - kHalf), kHalf + (a_drop ? 0.5 : 0.02) },
				g_faces[i] ? g_faces[i] : 1);
		}
		void PutInCup(int i)
		{
			TweenFrom(i, 0.18);
			g_kept[i] = false;
			g_rackSlot[i] = -1;
			g_sim.AddToCup(i);
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
				Sound("ITMGoldUp");
				if (g_totalWins % 10 == 0 && g_totalWins / 10 < 15) {
					Toast(fmt::format("Innkeepers now sell {} dice", PlayerDice()));
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
					Toast(fmt::format("{} scores {}: {}", g_oppName, yacht::CategoryName(cat), pts));
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
			if (pts < 0) {
				return;
			}
			Toast(pts > 0 ? fmt::format("+{}", pts) : "Scratched");
			g_phase = Phase::Scored;
			g_scoredTime = 0;
		}

		void Shout()
		{
			if (g_turn != 0 || g_phase != Phase::Choose || g_carry.die >= 0) {
				return;
			}
			if (g_rollsUsed >= 3) {
				Toast("No rolls left");
				return;
			}
			bool any = false;
			for (int i = 0; i < 5; ++i) {
				if (Active(i) && !InCup(i)) {
					TweenFrom(i, 0.8, 6);
					g_anim[i].spins = true;
					g_anim[i].spin = Vec3{ Rand() - 0.5, Rand() - 0.5, Rand() - 0.5 }.Normalized();
					g_sim.AddToCup(i);
					any = true;
				}
			}
			Toast(any ? "FUS RO DAH!" : "The board is already clear");
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
						Sound("ITMGoldDown");
						spdlog::info("Wager {} against {}", bet, g_oppName);
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
				}
			}
		}

		// ---------------------------------------------------------------- per frame
		void Step(double dt)
		{
			g_clock += dt;
			ProcessUiActs();
			if (g_phase == Phase::Idle || g_phase == Phase::Wager) {
				// the table sits ready behind the wager: cups at rest, dice in the cup
				for (int k = 0; k < 2; ++k) {
					Scene::SetCup(k, g_cups[k].p, g_cups[k].q);
				}
				return;
			}
			if (!g_confirmForfeit) {
				HandleInput();
			} else {
				Overlay::TakePresses();
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

			g_sim.Step(dt, c.p, c.q);

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
		ImFont* g_fBody = nullptr;
		ImFont* g_fBig = nullptr;
		ImFont* g_fTitle = nullptr;
		float   g_ui = 1.0f;

		const ImVec4 kText{ 0.94f, 0.93f, 0.91f, 1.0f };
		const ImVec4 kDim{ 0.64f, 0.62f, 0.58f, 1.0f };
		const ImVec4 kInk{ 0.11f, 0.07f, 0.04f, 1.0f };
		const ImVec4 kInkSoft{ 0.30f, 0.21f, 0.13f, 1.0f };
		const ImVec4 kRubric{ 0.55f, 0.14f, 0.07f, 1.0f };

		bool Button(const char* a_label, float a_w = 0.0f)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
			const bool r = ImGui::Button(a_label, ImVec2(a_w, 0));
			ImGui::PopStyleColor(3);
			return r;
		}

		void Centered(const char* a_text, ImFont* a_font, const ImVec4& a_col)
		{
			ImGui::PushFont(a_font);
			const float w = ImGui::CalcTextSize(a_text).x;
			ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
			ImGui::TextColored(a_col, "%s", a_text);
			ImGui::PopFont();
		}

		void Rule()
		{
			auto*        dl = ImGui::GetWindowDrawList();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float  w = ImGui::GetContentRegionAvail().x;
			dl->AddRectFilledMultiColor(ImVec2(p.x, p.y), ImVec2(p.x + w * 0.5f, p.y + 1), IM_COL32(240, 238, 232, 0), IM_COL32(240, 238, 232, 160),
				IM_COL32(240, 238, 232, 160), IM_COL32(240, 238, 232, 0));
			dl->AddRectFilledMultiColor(ImVec2(p.x + w * 0.5f, p.y), ImVec2(p.x + w, p.y + 1), IM_COL32(240, 238, 232, 160), IM_COL32(240, 238, 232, 0),
				IM_COL32(240, 238, 232, 0), IM_COL32(240, 238, 232, 160));
			ImGui::Dummy(ImVec2(w, 8 * g_ui));
		}

		void Card(int a_card, bool a_left)
		{
			const auto&  io = ImGui::GetIO();
			const float  w = 360 * g_ui;
			ImGui::SetNextWindowPos(ImVec2(a_left ? 20 * g_ui : io.DisplaySize.x - w - 20 * g_ui, 20 * g_ui));
			ImGui::SetNextWindowSize(ImVec2(w, 0));
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.86f, 0.78f, 0.62f, a_card == g_turn ? 0.97f : 0.8f));
			ImGui::PushStyleColor(ImGuiCol_Text, kInk);
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.22f, 0.10f, 0.9f));
			ImGui::Begin(a_card == 0 ? "##yours" : "##theirs", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
			ImGui::PushFont(g_fBig);
			ImGui::TextUnformatted(a_card == 0 ? "Yours" : g_oppName.c_str());
			ImGui::SameLine(w - ImGui::CalcTextSize("000").x - 16 * g_ui);
			ImGui::Text("%d", g_cards[a_card].Total());
			ImGui::PopFont();
			const auto& r = g_records[g_oppId];
			ImGui::TextColored(kInkSoft, "Record %u-%u%s", a_card == 0 ? r.w : r.l, a_card == 0 ? r.l : r.w, r.d ? fmt::format(", {} drawn", r.d).c_str() : "");
			int        dice[5];
			Dice(dice);
			const bool pick = a_card == 0 && g_turn == 0 && g_phase == Phase::Choose && g_carry.die < 0;
			auto row = [&](int cat) {
				const auto& card = g_cards[a_card];
				const bool  filled = card.Filled(cat);
				const int   prev = pick && !filled ? card.Preview(cat, dice) : -1;
				const float valX = w * 0.5f - 52 * g_ui;
				ImGui::PushID(cat);
				if (prev >= 0) {
					// a box you can write this roll into: click the row
					ImGui::PushStyleColor(ImGuiCol_Text, kInk);
					if (ImGui::Selectable(yacht::CategoryName(cat), false, 0, ImVec2(valX + 30 * g_ui, 0))) {
						Post(UiAct::Score, cat);
					}
					ImGui::PopStyleColor();
				} else {
					ImGui::TextColored(filled ? kInkSoft : kInk, "%s", yacht::CategoryName(cat));
				}
				ImGui::SameLine(valX);
				if (filled) {
					ImGui::Text("%d", card.boxes[cat]);
				} else if (prev >= 0) {
					ImGui::TextColored(kRubric, "%d", prev);
				} else {
					ImGui::TextUnformatted(" ");
				}
				ImGui::PopID();
			};
			ImGui::Columns(2, nullptr, false);
			for (int c = yacht::kOnes; c <= yacht::kSixes; ++c) {
				row(c);
			}
			ImGui::TextColored(kInkSoft, "Bonus %d", g_cards[a_card].UpperBonus());
			ImGui::NextColumn();
			for (int c = yacht::kThreeKind; c <= yacht::kChance; ++c) {
				row(c);
			}
			ImGui::Columns(1);
			ImGui::End();
			ImGui::PopStyleColor(3);
		}

		void DrawPlay()
		{
			const auto& io = ImGui::GetIO();
			Card(1, true);
			Card(0, false);
			// top centre: whose turn, the rolls
			{
				ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 18 * g_ui), 0, ImVec2(0.5f, 0));
				ImGui::SetNextWindowBgAlpha(0.55f);
				ImGui::Begin("##turn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
				const std::string t = g_phase == Phase::Over ? "Game over" : g_turn == 0 ? "Your turn" : g_oppName + "'s turn";
				ImGui::PushFont(g_fBig);
				ImGui::TextColored(kText, "%s", t.c_str());
				ImGui::PopFont();
				ImGui::TextColored(kDim, "Roll %d of 3   -   Wager %d septims", (std::min)(3, g_rollsUsed + (g_phase == Phase::Ready ? 1 : 0)), g_bet);
				ImGui::End();
			}
			// bottom centre: what to do now
			{
				std::string hint;
				if (g_turn == 1) {
					hint = g_oppName + " is playing...";
				} else if (g_phase == Phase::Ready) {
					hint = "Hold the left button and move to shake the cup. Let go to throw.";
				} else if (g_phase == Phase::Shaking) {
					hint = "Let go to throw";
				} else if (g_phase == Phase::Pouring || g_phase == Phase::Rolling) {
					hint = "Rolling...";
				} else if (g_phase == Phase::Choose) {
					hint = g_rollsUsed < 3 ? "Click dice to keep them, drag the rest into the cup and shake again - or pick a box on your card"
					                       : "Pick a box on your card";
				}
				if (!hint.empty()) {
					ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24 * g_ui), 0, ImVec2(0.5f, 1));
					ImGui::SetNextWindowBgAlpha(0.5f);
					ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
					ImGui::TextColored(kText, "%s", hint.c_str());
					ImGui::End();
				}
			}
			// bottom right: the buttons, as Skyrim's key prompts
			{
				ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 20 * g_ui, io.DisplaySize.y - 70 * g_ui), 0, ImVec2(1, 1));
				ImGui::SetNextWindowBgAlpha(0.0f);
				ImGui::Begin("##keys", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.84f, 0.95f, 1));
				if (Button("[Z]  Fus Ro Dah")) {
					Post(UiAct::Shout);
				}
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (Button("[Tab]  Forfeit")) {
					Post(UiAct::Forfeit);
				}
				ImGui::End();
			}
			// a notice in the middle
			if (!g_toast.empty() && g_clock - g_toastTime < 2.2) {
				ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.33f), 0, ImVec2(0.5f, 0.5f));
				ImGui::SetNextWindowBgAlpha(0.0f);
				ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
				ImGui::PushFont(g_fTitle);
				ImGui::TextColored(kText, "%s", g_toast.c_str());
				ImGui::PopFont();
				ImGui::End();
			}
			if (g_confirmForfeit && g_phase != Phase::Over) {
				ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), 0, ImVec2(0.5f, 0.5f));
				ImGui::SetNextWindowSize(ImVec2(460 * g_ui, 0));
				ImGui::SetNextWindowBgAlpha(0.9f);
				ImGui::Begin("##forfeit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
				Centered("FORFEIT THIS GAME?", g_fBig, kText);
				Centered(fmt::format("You lose your {} septims", g_betHeld).c_str(), g_fBody, kDim);
				Rule();
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 170 * g_ui);
				if (Button("Forfeit", 160 * g_ui)) {
					Post(UiAct::ForfeitYes);
				}
				ImGui::SameLine();
				if (Button("Keep playing", 160 * g_ui)) {
					Post(UiAct::ForfeitNo);
				}
				ImGui::End();
			}
			if (Config::Get().debugOverlay) {
				auto* dl = ImGui::GetForegroundDrawList();
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

		void DrawWager()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f), 0, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(560 * g_ui, 0));
			ImGui::SetNextWindowBgAlpha(0.88f);
			ImGui::Begin("##wager", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
			Centered("A GAME OF YACHT", g_fTitle, kText);
			Centered(fmt::format("with {}", g_oppName).c_str(), g_fBody, kDim);
			Rule();
			const auto& cfg = Config::Get();
			const int   gold = GoldHeld();
			const int   top = std::clamp(gold - gold % 5, cfg.minBet, cfg.maxBet);
			static int  s_bet = 25;
			s_bet = std::clamp(g_bet, cfg.minBet, top);
			Centered(fmt::format("Your purse: {} septims", gold).c_str(), g_fBody, kDim);
			ImGui::Spacing();
			ImGui::PushFont(g_fBig);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::SliderInt("##bet", &s_bet, cfg.minBet, top, "Wager  %d septims")) {
				s_bet = std::clamp(s_bet - s_bet % 5, cfg.minBet, top);
				g_bet = s_bet;
			}
			ImGui::PopFont();
			Centered(fmt::format("Win and you take {} septims", s_bet * 2).c_str(), g_fBody, kDim);
			Rule();
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 170 * g_ui);
			ImGui::PushFont(g_fBig);
			if (Button("Place bet", 160 * g_ui)) {
				Post(UiAct::PlaceBet, s_bet);
			}
			ImGui::SameLine();
			if (Button("Leave", 160 * g_ui)) {
				Post(UiAct::Leave);
			}
			ImGui::PopFont();
			if (!g_toast.empty() && g_clock - g_toastTime < 2.2) {
				Centered(g_toast.c_str(), g_fBody, kRubric);
			}
			ImGui::End();
		}

		void DrawOver()
		{
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), 0, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(720 * g_ui, 0));
			ImGui::SetNextWindowBgAlpha(0.85f);
			ImGui::Begin("##over", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
			Centered("GAME OVER", g_fBody, kDim);
			const char* title = g_outcome == "win" ? "YOU WIN!" : g_outcome == "draw" ? "A DRAW" : "YOU LOSE!";
			Centered(title, g_fTitle, g_outcome == "win" ? kText : ImVec4(0.94f, 0.76f, 0.71f, 1));
			Rule();
			Centered(g_outcome == "forfeit" ? "You forfeited" : fmt::format("{}  -  {}", g_cards[0].Total(), g_cards[1].Total()).c_str(), g_fBig, kText);
			Centered(g_payoutText.c_str(), g_fBody, kDim);
			const auto& r = g_records[g_oppId];
			Centered(fmt::format("Your record against {}: {}-{}{}", g_oppName, r.w, r.l, r.d ? fmt::format(", {} drawn", r.d) : "").c_str(), g_fBody, kDim);
			Centered(fmt::format("Your cup: the {} ({})", PlayerCup().name, PlayerCup().rank).c_str(), g_fBody, kDim);
			Rule();
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 170 * g_ui);
			ImGui::PushFont(g_fBig);
			if (Button("Rematch", 160 * g_ui)) {
				Post(UiAct::Rematch);
			}
			ImGui::SameLine();
			if (Button("Leave", 160 * g_ui)) {
				Post(UiAct::Leave);
			}
			ImGui::PopFont();
			ImGui::End();
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

	// ==================================================================== public
	void OnDataLoaded()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		g_faction = dh->LookupForm<RE::TESFaction>(EspIds::kTableFaction, EspIds::kPlugin);
		g_canAfford = dh->LookupForm<RE::TESGlobal>(EspIds::kCanAfford, EspIds::kPlugin);
		g_infoPlay = dh->LookupFormID(EspIds::kInfoPlay, EspIds::kPlugin);
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
		if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESTopicInfoEvent>(&g_sinks);
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
		Scene::TeardownTick();
		QuestFixTick();
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}
		// the opponent said yes: open the table once the conversation has closed
		if (const auto id = g_pendingSpeaker.load(); id && !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
			g_pendingSpeaker = 0;
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
		TagSeated();
	}

	void Tick(float a_dt)
	{
		std::lock_guard lk(g_mtx);
		const double    dt = std::clamp(static_cast<double>(a_dt), 0.001, 0.05);
		if (!Scene::Ready()) {
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
		auto&       io = ImGui::GetIO();
		char        windir[MAX_PATH]{};
		::GetWindowsDirectoryA(windir, MAX_PATH);
		const std::string fonts = std::string(windir) + "\\Fonts\\";
		std::error_code   ec;
		auto              load = [&](const char* a_file, float a_size) -> ImFont* {
            const std::string p = fonts + a_file;
            if (!std::filesystem::exists(p, ec)) {
                return nullptr;
            }
            ImFontConfig fc;
            fc.OversampleH = 2;
            return io.Fonts->AddFontFromFileTTF(p.c_str(), a_size * g_ui, &fc);
		};
		g_fBody = load("georgia.ttf", 19.0f);
		g_fBig = load("georgia.ttf", 26.0f);
		g_fTitle = load("georgiab.ttf", 52.0f);
		if (!g_fBody) {
			g_fBody = g_fBig = g_fTitle = io.Fonts->AddFontDefault();
		}
		io.FontDefault = g_fBody;
		auto& st = ImGui::GetStyle();
		st.WindowRounding = 0.0f;
		st.WindowBorderSize = 1.0f;
		st.FrameRounding = 2.0f;
		st.WindowPadding = ImVec2(18 * g_ui, 14 * g_ui);
		st.ItemSpacing = ImVec2(10 * g_ui, 6 * g_ui);
		st.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.86f);
		st.Colors[ImGuiCol_Border] = ImVec4(0.94f, 0.93f, 0.91f, 0.45f);
		st.Colors[ImGuiCol_Text] = kText;
		st.Colors[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0.5f);
		st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.08f);
		st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.93f, 0.91f, 0.9f);
		st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
		st.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.14f, 0.07f, 0.15f);
		st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.14f, 0.07f, 0.25f);
		st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.14f, 0.07f, 0.35f);
	}

	void DrawUi()
	{
		std::lock_guard lk(g_mtx);
		if (!Scene::IsOpen()) {
			return;
		}
		if (!Scene::Ready()) {
			const auto& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.85f), 0, ImVec2(0.5f, 0.5f));
			ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
			ImGui::TextColored(kDim, "Setting up the table...");
			ImGui::End();
		}
		switch (g_phase) {
		case Phase::Wager:
			DrawWager();
			break;
		case Phase::Over:
			DrawPlay();
			DrawOver();
			break;
		case Phase::Idle:
			break;
		default:
			DrawPlay();
			break;
		}
	}

	// ---------------------------------------------------------------- co-save: records per opponent, last score, wins
	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lk(g_mtx);
		if (!a_intfc->OpenRecord('YREC', 1)) {
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
		}
		spdlog::info("Loaded: last score {}, {} wins, {} opponents", g_lastScore, g_totalWins, g_records.size());
	}

	void Revert(SKSE::SerializationInterface*)
	{
		std::lock_guard lk(g_mtx);
		g_records.clear();
		g_lastScore = 0;
		g_totalWins = 0;
	}
}
