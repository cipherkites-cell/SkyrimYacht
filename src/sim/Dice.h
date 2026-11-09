#pragma once

// Dice physics: five rounded dice, a felt tray and a dice cup the player shakes and tips.
//
// Plain C++ with no game or standard-library dependencies (tests/dice_test.cpp, preview/ builds it to WebAssembly).
//
// World space: z up, the tray floor at z = 0, its centre at the origin. Units are game units (about 70 per metre)
// and seconds. The cup is kinematic: the caller moves it (mouse, animation) and the dice react to it.
//
// Each die is a rounded box: a box of half-size (h - r) swept by a sphere of radius r. Contacts come from the eight
// corner spheres (and the twelve edge-middle spheres against other dice), so the dice roll off their edges instead
// of sticking, and two dice can never pass through each other edge-first.

#include "Math.h"

namespace yacht
{
	struct DiceSpec
	{
		double dieSize{ 1.2 };           // edge length (1.2 = about 17 mm)
		double dieRound{ 0.14 };         // corner radius, as a fraction of the edge
		double dieMass{ 1.0 };
		double gravity{ 686.0 };         // 9.81 m/s^2 in game units

		double trayHalfX{ 11.0 };        // inside of the tray walls
		double trayHalfY{ 7.5 };
		double trayWallHeight{ 1.5 };    // walls stop dice below this; higher up only dice already over the tray
		double trayWallThick{ 0.8 };

		double dragSpeed{ 45.0 };        // fastest a die follows the player's hand when moved on the table

		double cupRadius{ 1.9 };         // inside radius
		double cupWall{ 0.22 };          // wall thickness
		double cupHeight{ 4.0 };         // base to rim, inside

		double feltFriction{ 0.6 };      // grippy felt tips sliding dice over their edges
		double feltRestitution{ 0.4 };
		double wallRestitution{ 0.45 };
		double dieFriction{ 0.30 };
		double dieRestitution{ 0.5 };
		double cupFriction{ 0.35 };
		double cupRestitution{ 0.40 };

		double maxSpeed{ 260.0 };        // units/s
		double maxSpin{ 70.0 };          // rad/s
		double rollDamping{ 0.3 };       // per second, spin lost while touching the felt

		double settleSpeed{ 1.5 };       // a die below this speed (and spin) ...
		double settleSpin{ 2.0 };
		double settleTime{ 0.3 };        // ... for this long has settled
		double cockedDot{ 0.94 };        // a settled die whose top face is tilted more than ~20 degrees gets a nudge
	};

	enum class DieState : unsigned char
	{
		kOff,     // not simulated (held on the rack, or not in the game yet)
		kActive   // in the cup or on the tray
	};

	struct Die
	{
		Vec3     pos;
		Vec3     vel;
		Quat     rot;
		Vec3     spin;            // angular velocity, world frame
		DieState state{ DieState::kOff };
		double   restTime{ 0.0 };
		bool     settled{ false };
		bool     frozen{ false };  // a kept die: nothing moves it (other dice bounce off it)
		int      nudges{ 0 };
	};

	struct DiceEvent
	{
		enum class Type : unsigned char
		{
			kDieDie,
			kDieFelt,
			kDieWall,
			kDieCup
		};

		Type   type;
		int    die{ -1 };
		double speed{ 0.0 };  // closing speed, for the sound
	};

	class DiceSim
	{
	public:
		static constexpr int kDice = 5;
		static constexpr int kMaxEvents = 64;

		explicit DiceSim(const DiceSpec& a_spec = {}, unsigned int a_seed = 1);

		const DiceSpec& Spec() const { return _spec; }
		const Die&      DieAt(int a_index) const { return _dice[a_index]; }

		// The cup: position of the centre of its inside base, and orientation (its axis is local +z).
		const Vec3& CupPos() const { return _cupPos; }
		const Quat& CupRot() const { return _cupRot; }
		// Moves the cup instantly, with no velocity (use at the start of a turn).
		void PlaceCup(const Vec3& a_pos, const Quat& a_rot);
		// Closes the mouth of the cup (the player's palm over it while shaking) so dice can't jump out.
		void SetLid(bool a_closed) { _lid = a_closed; }
		bool Lid() const { return _lid; }

		// The player moving a die by hand: it slides towards a_target on the felt without turning over, pushing
		// other dice out of the way. EndDrag lets go.
		void Drag(int a_index, const Vec3& a_target);
		void EndDrag() { _drag = -1; }
		int  Dragged() const { return _drag; }

		// Puts every die in a_mask (bit i = die i) into the cup, randomly turned, and makes it active.
		void LoadCup(unsigned int a_mask);
		// Takes a die out of play (to the rack) or back in.
		void SetActive(int a_index, bool a_active);
		// Locks a die where it lies (the dice the player keeps while the rest are rolled), or unlocks it.
		void SetFrozen(int a_index, bool a_frozen);
		// Drops a die into the cup through its mouth.
		void AddToCup(int a_index);
		// Puts a die at rest at a_pos, face a_face (1..6) up.
		void PlaceDie(int a_index, const Vec3& a_pos, int a_face);

		// Advances by a_dt while the cup moves (straight-line) to a_cupPos / a_cupRot over that time.
		// Returns the number of events written to Events().
		int Step(double a_dt, const Vec3& a_cupPos, const Quat& a_cupRot);
		const DiceEvent* Events() const { return _events; }

		// Whether die i's centre is inside the cup.
		bool InCup(int a_index) const;
		bool AnyInCup() const;
		// Every active die outside the cup has settled flat; none is in the cup.
		bool AllSettled() const;
		// The face pointing up (1..6), or 0 if the die isn't lying flat.
		int TopFace(int a_index) const;

		// Pip layout: which face is on which local axis. Opposite faces add to 7.
		//   +x 1, -x 6, +y 2, -y 5, +z 3, -z 4
		static int  FaceOnAxis(int a_axis, bool a_positive);
		static Quat FaceUp(int a_face);

	private:
		struct Contact
		{
			int    a{ -1 };
			int    b{ -1 };     // other die, or -1 for the felt, walls or cup
			Vec3   p;           // world contact point
			Vec3   n;           // unit normal, pushing a away from b
			double depth{ 0.0 };
			double friction{ 0.0 };
			double bias{ 0.0 }; // target closing speed after the solve (restitution)
			Vec3   surfVel;     // velocity of the other surface when b == -1 (moving cup)
			Vec3   t1;
			Vec3   t2;
			double massN{ 0.0 };
			double massT1{ 0.0 };
			double massT2{ 0.0 };
			double pn{ 0.0 };
			double pt1{ 0.0 };
			double pt2{ 0.0 };
			DiceEvent::Type kind{ DiceEvent::Type::kDieFelt };
		};

		static constexpr int kMaxContacts = 768;

		void SubStep(double a_dt, const Vec3& a_cupVel, const Vec3& a_cupSpin);
		void Collect(const Vec3& a_cupVel, const Vec3& a_cupSpin);
		void AddContact(int a_a, int a_b, const Vec3& a_p, const Vec3& a_n, double a_depth, double a_friction, double a_restitution,
			const Vec3& a_surfVel, DiceEvent::Type a_kind);
		void CollideStatic(int a_index);
		void CollideCup(int a_index, const Vec3& a_cupVel, const Vec3& a_cupSpin);
		void CollidePair(int a_a, int a_b);
		void Prepare(double a_dt);
		void Solve();
		void Correct();
		void Integrate(double a_dt);
		void UpdateSettle(double a_dt);
		void Emit(DiceEvent::Type a_type, int a_die, double a_speed);

		Vec3 VelocityAt(int a_index, const Vec3& a_p) const;
		double Inertia() const;
		double InvMass(int a_index) const { return _dice[a_index].frozen ? 0.0 : 1.0 / _spec.dieMass; }
		double InvInertia(int a_index) const { return _dice[a_index].frozen ? 0.0 : 1.0 / Inertia(); }
		double HalfInner() const { return _spec.dieSize * 0.5 - Radius(); }
		double Radius() const { return _spec.dieSize * _spec.dieRound; }

		DiceSpec _spec;
		Die      _dice[kDice];
		Rng      _rng;

		Vec3 _cupPos;
		Quat _cupRot;
		bool _lid{ false };
		int  _drag{ -1 };
		Vec3 _dragTarget;

		Contact _contacts[kMaxContacts];
		int     _contactCount{ 0 };
		bool    _onFelt[kDice]{};

		DiceEvent _events[kMaxEvents];
		int       _eventCount{ 0 };
		double    _soundCooldown[kDice][4]{};
	};
}
