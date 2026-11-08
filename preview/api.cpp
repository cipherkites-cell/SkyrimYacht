// WebAssembly entry points for the browser preview. Wraps the same DiceSim and Scorecard the SKSE plugin uses.
#include "../src/sim/Dice.h"
#include "../src/sim/Yacht.h"

using namespace yacht;

extern "C"
{
	// The compiler may emit calls to these for struct copies; there's no libc in the wasm build.
	void* memset(void* a_dst, int a_value, unsigned long a_count)
	{
		auto* d = static_cast<unsigned char*>(a_dst);
		for (unsigned long i = 0; i < a_count; ++i) {
			d[i] = static_cast<unsigned char>(a_value);
		}
		return a_dst;
	}

	void* memcpy(void* a_dst, const void* a_src, unsigned long a_count)
	{
		auto*       d = static_cast<unsigned char*>(a_dst);
		const auto* s = static_cast<const unsigned char*>(a_src);
		for (unsigned long i = 0; i < a_count; ++i) {
			d[i] = s[i];
		}
		return a_dst;
	}

	void* memmove(void* a_dst, const void* a_src, unsigned long a_count)
	{
		auto*       d = static_cast<unsigned char*>(a_dst);
		const auto* s = static_cast<const unsigned char*>(a_src);
		if (d < s) {
			for (unsigned long i = 0; i < a_count; ++i) {
				d[i] = s[i];
			}
		} else {
			for (unsigned long i = a_count; i > 0; --i) {
				d[i - 1] = s[i - 1];
			}
		}
		return a_dst;
	}
}

namespace
{
	DiceSim   g_sim;
	Scorecard g_card;

	// Per die: pos xyz, rot wxyz, active, inCup, settled, face. Then the cup: pos xyz, rot wxyz.
	constexpr int kPerDie = 11;
	double        g_state[DiceSim::kDice * kPerDie + 7];
	double        g_events[DiceSim::kMaxEvents * 3];
	double        g_spec[8];

	int g_dice[5];
}

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT void y_init(unsigned int a_seed)
{
	g_sim = DiceSim(DiceSpec{}, a_seed);
	g_card.Clear();
}

EXPORT double* y_spec()
{
	const DiceSpec& s = g_sim.Spec();
	g_spec[0] = s.dieSize;
	g_spec[1] = s.dieRound;
	g_spec[2] = s.trayHalfX;
	g_spec[3] = s.trayHalfY;
	g_spec[4] = s.cupRadius;
	g_spec[5] = s.cupWall;
	g_spec[6] = s.cupHeight;
	g_spec[7] = s.gravity;
	return g_spec;
}

EXPORT void y_place_cup(double px, double py, double pz, double qw, double qx, double qy, double qz)
{
	g_sim.PlaceCup({ px, py, pz }, { qw, qx, qy, qz });
}

EXPORT void y_set_lid(int a_closed) { g_sim.SetLid(a_closed != 0); }
EXPORT void y_load(unsigned int a_mask) { g_sim.LoadCup(a_mask); }
EXPORT void y_set_active(int a_index, int a_active) { g_sim.SetActive(a_index, a_active != 0); }
EXPORT void y_drag(int a_index, double x, double y, double z) { g_sim.Drag(a_index, { x, y, z }); }
EXPORT void y_drag_end() { g_sim.EndDrag(); }
EXPORT void y_place_die(int a_index, double x, double y, double z, int a_face) { g_sim.PlaceDie(a_index, { x, y, z }, a_face); }

EXPORT int y_step(double a_dt, double px, double py, double pz, double qw, double qx, double qy, double qz)
{
	const int n = g_sim.Step(a_dt, { px, py, pz }, { qw, qx, qy, qz });
	const DiceEvent* ev = g_sim.Events();
	for (int i = 0; i < n; ++i) {
		g_events[i * 3 + 0] = static_cast<double>(ev[i].type);
		g_events[i * 3 + 1] = ev[i].die;
		g_events[i * 3 + 2] = ev[i].speed;
	}
	return n;
}

EXPORT double* y_events() { return g_events; }

EXPORT double* y_state()
{
	for (int i = 0; i < DiceSim::kDice; ++i) {
		const Die& d = g_sim.DieAt(i);
		double*    o = g_state + i * kPerDie;
		o[0] = d.pos.x;
		o[1] = d.pos.y;
		o[2] = d.pos.z;
		o[3] = d.rot.w;
		o[4] = d.rot.x;
		o[5] = d.rot.y;
		o[6] = d.rot.z;
		o[7] = d.state == DieState::kActive ? 1.0 : 0.0;
		o[8] = g_sim.InCup(i) ? 1.0 : 0.0;
		o[9] = d.settled ? 1.0 : 0.0;
		o[10] = g_sim.TopFace(i);
	}
	double* c = g_state + DiceSim::kDice * kPerDie;
	c[0] = g_sim.CupPos().x;
	c[1] = g_sim.CupPos().y;
	c[2] = g_sim.CupPos().z;
	c[3] = g_sim.CupRot().w;
	c[4] = g_sim.CupRot().x;
	c[5] = g_sim.CupRot().y;
	c[6] = g_sim.CupRot().z;
	return g_state;
}

EXPORT int y_all_settled() { return g_sim.AllSettled() ? 1 : 0; }
EXPORT int y_any_in_cup() { return g_sim.AnyInCup() ? 1 : 0; }

// Scorecard. Dice values are passed in with y_dice first.
EXPORT void y_dice(int a, int b, int c, int d, int e)
{
	g_dice[0] = a;
	g_dice[1] = b;
	g_dice[2] = c;
	g_dice[3] = d;
	g_dice[4] = e;
}

EXPORT void y_card_reset() { g_card.Clear(); }
EXPORT int  y_card_preview(int a_category) { return g_card.Preview(a_category, g_dice); }
EXPORT int  y_card_fill(int a_category) { return g_card.Fill(a_category, g_dice); }
EXPORT int  y_card_box(int a_category) { return g_card.boxes[a_category]; }
EXPORT int  y_card_upper() { return g_card.UpperSum(); }
EXPORT int  y_card_bonus() { return g_card.UpperBonus(); }
EXPORT int  y_card_yacht_bonus() { return g_card.yachtBonuses * kYachtBonus; }
EXPORT int  y_card_total() { return g_card.Total(); }
EXPORT int  y_card_full() { return g_card.Full() ? 1 : 0; }
