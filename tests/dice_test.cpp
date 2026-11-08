// Native tests for the dice simulation. Build: see tests/build.sh (or CI).
#include "../src/sim/Dice.h"

#include <cmath>
#include <cstdio>

using namespace yacht;

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (!a_ok) {
			std::printf("FAIL: %s\n", a_what);
			++g_failures;
		}
	}

	constexpr double kFrame = 1.0 / 60.0;

	Quat Tilt(double a_degrees)  // tips the cup's mouth towards +x
	{
		return Quat::FromAxisAngle({ 0, 1, 0 }, a_degrees * m::kPi / 180.0);
	}

	bool Overlapping(const DiceSim& a_sim)
	{
		for (int a = 0; a < DiceSim::kDice; ++a) {
			for (int b = a + 1; b < DiceSim::kDice; ++b) {
				if (a_sim.DieAt(a).state != DieState::kActive || a_sim.DieAt(b).state != DieState::kActive) {
					continue;
				}
				if ((a_sim.DieAt(a).pos - a_sim.DieAt(b).pos).Length() < a_sim.Spec().dieSize * 0.8) {
					return true;
				}
			}
		}
		return false;
	}

	void TestFaces()
	{
		DiceSim sim;
		for (int f = 1; f <= 6; ++f) {
			sim.PlaceDie(0, { 0, 0, 0.6 }, f);
			char msg[64];
			std::snprintf(msg, sizeof(msg), "PlaceDie face %d reads back", f);
			Check(sim.TopFace(0) == f, msg);
		}
		Check(DiceSim::FaceOnAxis(0, true) + DiceSim::FaceOnAxis(0, false) == 7, "opposite faces add to 7");
	}

	void TestDrop()
	{
		DiceSim sim;
		sim.PlaceCup({ 0, 0, 30 }, {});
		sim.PlaceDie(0, { 0, 0, 6 }, 3);
		Die tilted = sim.DieAt(0);
		(void)tilted;
		double t = 0;
		while (t < 4.0 && !sim.AllSettled()) {
			sim.Step(kFrame, sim.CupPos(), sim.CupRot());
			t += kFrame;
		}
		const Die& d = sim.DieAt(0);
		Check(sim.AllSettled(), "dropped die settles");
		Check(std::fabs(d.pos.z - sim.Spec().dieSize * 0.5) < 0.05, "dropped die rests on the felt");
		Check(sim.TopFace(0) != 0, "dropped die lies flat");
	}

	// Shakes the cup for a_seconds, then tips it out and waits. Returns false if anything went wrong.
	bool Throw(unsigned int a_seed, int (&a_faces)[5], double& a_settleTime, int& a_nudges)
	{
		DiceSim sim({}, a_seed);
		const Vec3 home{ -4.0, 0.0, 6.0 };
		sim.PlaceCup(home, {});
		sim.LoadCup(0x1F);
		sim.SetLid(true);

		// Settle in the cup.
		for (int i = 0; i < 20; ++i) {
			sim.Step(kFrame, home, {});
		}

		// Shake: a jerky circle with a bit of up and down.
		double t = 0.0;
		const double shake = 1.2 + (a_seed % 7) * 0.1;
		while (t < shake) {
			t += kFrame;
			const double a = t * 18.0;
			const Vec3   p = home + Vec3{ std::cos(a) * 1.1, std::sin(a * 1.3) * 1.1, std::sin(a * 2.1) * 0.6 };
			const Quat   r = Quat::FromAxisAngle({ std::sin(a), std::cos(a), 0 }, 0.18);
			sim.Step(kFrame, p, r);
			for (int i = 0; i < DiceSim::kDice; ++i) {
				if (!sim.InCup(i)) {
					std::printf("  seed %u: die %d left the cup while shaking\n", a_seed, i);
					return false;
				}
			}
			if (Overlapping(sim)) {
				std::printf("  seed %u: dice overlap while shaking\n", a_seed);
				return false;
			}
		}

		sim.SetLid(false);
		// Pour: tip 125 degrees over 0.35 s while sweeping forward, then lift away.
		const Vec3 start = sim.CupPos();
		const Vec3 pourAt{ -2.0, 0.0, 6.5 };
		double     p = 0.0;
		while (p < 0.35) {
			p += kFrame;
			const double k = p / 0.35;
			sim.Step(kFrame, start + (pourAt - start) * k, Tilt(125.0 * k));
		}
		for (int i = 0; i < 40; ++i) {
			sim.Step(kFrame, pourAt, Tilt(125.0));
		}
		const Vec3 away{ -9.0, 0.0, 16.0 };
		for (int i = 0; i < 20; ++i) {
			const double k = (i + 1) / 20.0;
			sim.Step(kFrame, pourAt + (away - pourAt) * k, Tilt(125.0 * (1.0 - k)));
		}

		double wait = 0.0;
		while (wait < 8.0 && !sim.AllSettled()) {
			sim.Step(kFrame, away, {});
			wait += kFrame;
			if (Overlapping(sim)) {
				std::printf("  seed %u: dice overlap on the tray\n", a_seed);
				return false;
			}
		}
		a_settleTime = wait;
		if (!sim.AllSettled()) {
			std::printf("  seed %u: dice still moving after 8 s (in cup: %d)\n", a_seed, sim.AnyInCup() ? 1 : 0);
			return false;
		}

		a_nudges = 0;
		const DiceSpec& s = sim.Spec();
		for (int i = 0; i < DiceSim::kDice; ++i) {
			const Die& d = sim.DieAt(i);
			a_faces[i] = sim.TopFace(i);
			a_nudges += d.nudges;
			if (a_faces[i] == 0) {
				std::printf("  seed %u: die %d settled cocked\n", a_seed, i);
				return false;
			}
			if (std::fabs(d.pos.x) > s.trayHalfX || std::fabs(d.pos.y) > s.trayHalfY || d.pos.z < 0.0 || d.pos.z > s.dieSize * 1.6) {
				std::printf("  seed %u: die %d ended at (%.2f %.2f %.2f)\n", a_seed, i, d.pos.x, d.pos.y, d.pos.z);
				return false;
			}
		}
		return true;
	}

	void TestThrows()
	{
		int    counts[7]{};
		int    bad = 0;
		double worst = 0.0;
		double total = 0.0;
		int    nudges = 0;
		const int runs = 200;
		for (unsigned int seed = 1; seed <= runs; ++seed) {
			int    faces[5];
			double settle = 0.0;
			int    n = 0;
			if (!Throw(seed, faces, settle, n)) {
				++bad;
				continue;
			}
			for (int f : faces) {
				++counts[f];
			}
			worst = settle > worst ? settle : worst;
			total += settle;
			nudges += n;
		}
		std::printf("throws: %d/%d good, settle avg %.2f s worst %.2f s, nudges %d\n", runs - bad, runs, total / (runs - bad), worst, nudges);
		std::printf("faces:");
		int good = 0;
		for (int f = 1; f <= 6; ++f) {
			std::printf(" %d:%d", f, counts[f]);
			good += counts[f];
		}
		std::printf("\n");
		Check(bad == 0, "every throw ends cleanly");
		for (int f = 1; f <= 6; ++f) {
			const double share = static_cast<double>(counts[f]) / good;
			Check(share > 0.12 && share < 0.215, "faces come up evenly");
		}
	}
}

int main()
{
	TestFaces();
	TestDrop();
	TestThrows();
	std::printf(g_failures ? "%d failure(s)\n" : "dice tests passed\n", g_failures);
	return g_failures ? 1 : 0;
}
