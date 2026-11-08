#include "Dice.h"

namespace yacht
{
	namespace
	{
		constexpr double kSubStep = 1.0 / 960.0;
		constexpr int    kMaxSubSteps = 96;
		constexpr int    kIterations = 12;
		constexpr double kSlop = 0.004;           // penetration left alone, so resting contacts stay touching
		constexpr double kCorrection = 0.6;       // fraction of the remaining penetration removed per substep
		constexpr double kBounceThreshold = 1.5;  // closing speeds below this don't bounce (stops resting jitter)
		constexpr double kSoundThreshold = 6.0;
		constexpr double kSoundCooldown = 0.035;

		// Unit offsets of the eight corner spheres and the twelve edge-middle spheres.
		constexpr double kCorners[8][3] = {
			{ -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 },
			{ -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 }
		};
		constexpr double kEdges[12][3] = {
			{ 0, -1, -1 }, { 0, 1, -1 }, { 0, -1, 1 }, { 0, 1, 1 },
			{ -1, 0, -1 }, { 1, 0, -1 }, { -1, 0, 1 }, { 1, 0, 1 },
			{ -1, -1, 0 }, { 1, -1, 0 }, { -1, 1, 0 }, { 1, 1, 0 }
		};

		Vec3 Offset(const double (&a_unit)[3], double a_half) { return { a_unit[0] * a_half, a_unit[1] * a_half, a_unit[2] * a_half }; }

		void Tangents(const Vec3& a_n, Vec3& a_t1, Vec3& a_t2)
		{
			const Vec3 ref = m::Abs(a_n.z) < 0.9 ? Vec3{ 0.0, 0.0, 1.0 } : Vec3{ 1.0, 0.0, 0.0 };
			a_t1 = a_n.Cross(ref).Normalized();
			a_t2 = a_n.Cross(a_t1);
		}

		int TypeIndex(DiceEvent::Type a_type) { return static_cast<int>(a_type); }
	}

	DiceSim::DiceSim(const DiceSpec& a_spec, unsigned int a_seed) :
		_spec(a_spec),
		_rng(a_seed)
	{
		_cupPos = { 0.0, 0.0, 8.0 };
	}

	double DiceSim::Inertia() const { return _spec.dieMass * _spec.dieSize * _spec.dieSize / 6.0; }

	int DiceSim::FaceOnAxis(int a_axis, bool a_positive)
	{
		static constexpr int faces[3][2] = { { 6, 1 }, { 5, 2 }, { 4, 3 } };
		return faces[a_axis][a_positive ? 1 : 0];
	}

	Quat DiceSim::FaceUp(int a_face)
	{
		const double q = m::kPi * 0.5;
		switch (a_face) {
		case 1: return Quat::FromAxisAngle({ 0, 1, 0 }, -q);
		case 6: return Quat::FromAxisAngle({ 0, 1, 0 }, q);
		case 2: return Quat::FromAxisAngle({ 1, 0, 0 }, q);
		case 5: return Quat::FromAxisAngle({ 1, 0, 0 }, -q);
		case 4: return Quat::FromAxisAngle({ 1, 0, 0 }, m::kPi);
		default: return {};
		}
	}

	void DiceSim::PlaceCup(const Vec3& a_pos, const Quat& a_rot)
	{
		_cupPos = a_pos;
		_cupRot = a_rot;
		_cupRot.Normalize();
	}

	void DiceSim::LoadCup(unsigned int a_mask)
	{
		const double h = _spec.dieSize * 0.5;
		// Three on the base, two above them.
		const double spots[kDice][3] = {
			{ 90.0, 1.15, h + 0.04 },
			{ 210.0, 1.15, h + 0.04 },
			{ 330.0, 1.15, h + 0.04 },
			{ 30.0, 0.95, h * 3.0 + 0.15 },
			{ 210.0, 0.95, h * 3.0 + 0.15 }
		};

		int slot = 0;
		for (int i = 0; i < kDice; ++i) {
			if (!(a_mask & (1u << i))) {
				continue;
			}
			const double a = spots[slot][0] * m::kPi / 180.0;
			const Vec3   local{ m::Cos(a) * spots[slot][1], m::Sin(a) * spots[slot][1], spots[slot][2] };
			++slot;

			Die& d = _dice[i];
			d.state = DieState::kActive;
			d.pos = _cupPos + _cupRot.Rotate(local);
			d.vel = {};
			d.spin = {};
			const int  face = 1 + static_cast<int>(_rng.Uniform() * 6.0);
			const Quat yaw = Quat::FromAxisAngle({ 0, 0, 1 }, _rng.Range(0.0, 2.0 * m::kPi));
			d.rot = _cupRot * yaw * FaceUp(face > 6 ? 6 : face);
			d.rot.Normalize();
			d.restTime = 0.0;
			d.settled = false;
			d.nudges = 0;
		}
	}

	void DiceSim::SetActive(int a_index, bool a_active)
	{
		Die& d = _dice[a_index];
		d.state = a_active ? DieState::kActive : DieState::kOff;
		d.vel = {};
		d.spin = {};
	}

	void DiceSim::PlaceDie(int a_index, const Vec3& a_pos, int a_face)
	{
		Die& d = _dice[a_index];
		d.state = DieState::kActive;
		d.pos = a_pos;
		d.rot = FaceUp(a_face);
		d.vel = {};
		d.spin = {};
		d.restTime = 0.0;
		d.settled = false;
		d.nudges = 0;
	}

	bool DiceSim::InCup(int a_index) const
	{
		const Die& d = _dice[a_index];
		if (d.state != DieState::kActive) {
			return false;
		}
		const Vec3 c = _cupRot.InverseRotate(d.pos - _cupPos);
		return c.x * c.x + c.y * c.y < _spec.cupRadius * _spec.cupRadius && c.z > -_spec.dieSize * 0.5 && c.z < _spec.cupHeight;
	}

	bool DiceSim::AnyInCup() const
	{
		for (int i = 0; i < kDice; ++i) {
			if (InCup(i)) {
				return true;
			}
		}
		return false;
	}

	bool DiceSim::AllSettled() const
	{
		for (int i = 0; i < kDice; ++i) {
			const Die& d = _dice[i];
			if (d.state != DieState::kActive) {
				continue;
			}
			if (InCup(i) || !d.settled) {
				return false;
			}
		}
		return true;
	}

	int DiceSim::TopFace(int a_index) const
	{
		const Vec3   u = _dice[a_index].rot.InverseRotate({ 0.0, 0.0, 1.0 });
		const double c[3] = { u.x, u.y, u.z };
		int          axis = 0;
		for (int k = 1; k < 3; ++k) {
			if (m::Abs(c[k]) > m::Abs(c[axis])) {
				axis = k;
			}
		}
		if (m::Abs(c[axis]) < _spec.cockedDot) {
			return 0;
		}
		return FaceOnAxis(axis, c[axis] > 0.0);
	}

	int DiceSim::Step(double a_dt, const Vec3& a_cupPos, const Quat& a_cupRot)
	{
		_eventCount = 0;
		if (a_dt <= 0.0) {
			return 0;
		}

		int steps = static_cast<int>(a_dt / kSubStep + 0.999);
		if (steps < 1) {
			steps = 1;
		}
		if (steps > kMaxSubSteps) {
			steps = kMaxSubSteps;
		}
		const double h = a_dt / steps;

		const Vec3 fromPos = _cupPos;
		const Quat fromRot = _cupRot;
		Quat       target = a_cupRot;
		target.Normalize();

		for (int s = 1; s <= steps; ++s) {
			const double t = static_cast<double>(s) / steps;
			const Vec3   pos = fromPos + (a_cupPos - fromPos) * t;
			const Quat   rot = Quat::Nlerp(fromRot, target, t);
			const Vec3   cupVel = (pos - _cupPos) * (1.0 / h);
			const Vec3   cupSpin = Quat::AngularVelocity(_cupRot, rot, h);
			_cupPos = pos;
			_cupRot = rot;
			SubStep(h, cupVel, cupSpin);
		}
		return _eventCount;
	}

	void DiceSim::SubStep(double a_dt, const Vec3& a_cupVel, const Vec3& a_cupSpin)
	{
		for (auto& row : _soundCooldown) {
			for (double& c : row) {
				c = m::Max(0.0, c - a_dt);
			}
		}

		for (auto& d : _dice) {
			if (d.state == DieState::kActive) {
				d.vel.z -= _spec.gravity * a_dt;
			}
		}

		Collect(a_cupVel, a_cupSpin);
		Prepare(a_dt);
		Solve();
		Correct();
		Integrate(a_dt);
		UpdateSettle(a_dt);
	}

	void DiceSim::Collect(const Vec3& a_cupVel, const Vec3& a_cupSpin)
	{
		_contactCount = 0;
		for (int i = 0; i < kDice; ++i) {
			_onFelt[i] = false;
			if (_dice[i].state != DieState::kActive) {
				continue;
			}
			CollideStatic(i);
			CollideCup(i, a_cupVel, a_cupSpin);
		}
		for (int a = 0; a < kDice; ++a) {
			if (_dice[a].state != DieState::kActive) {
				continue;
			}
			for (int b = a + 1; b < kDice; ++b) {
				if (_dice[b].state != DieState::kActive) {
					continue;
				}
				const double reach = _spec.dieSize * 1.8;
				if ((_dice[a].pos - _dice[b].pos).LengthSq() > reach * reach) {
					continue;
				}
				CollidePair(a, b);
				CollidePair(b, a);
			}
		}
	}

	void DiceSim::AddContact(int a_a, int a_b, const Vec3& a_p, const Vec3& a_n, double a_depth, double a_friction, double a_restitution,
		const Vec3& a_surfVel, DiceEvent::Type a_kind)
	{
		if (_contactCount >= kMaxContacts) {
			return;
		}
		Contact& c = _contacts[_contactCount++];
		c.a = a_a;
		c.b = a_b;
		c.p = a_p;
		c.n = a_n;
		c.depth = a_depth;
		c.friction = a_friction;
		c.bias = a_restitution;  // stored here until Prepare turns it into a target speed
		c.surfVel = a_surfVel;
		c.kind = a_kind;
		c.pn = c.pt1 = c.pt2 = 0.0;
	}

	void DiceSim::CollideStatic(int a_index)
	{
		const Die&   d = _dice[a_index];
		const double e = HalfInner();
		const double r = Radius();
		const double X = _spec.trayHalfX;
		const double Y = _spec.trayHalfY;

		for (const auto& corner : kCorners) {
			const Vec3 s = d.pos + d.rot.Rotate(Offset(corner, e));

			if (s.z < r) {
				const Vec3 n{ 0.0, 0.0, 1.0 };
				AddContact(a_index, -1, s - n * r, n, r - s.z, _spec.feltFriction, _spec.feltRestitution, {}, DiceEvent::Type::kDieFelt);
				_onFelt[a_index] = true;
			}

			const struct
			{
				double gap;
				Vec3   n;
			} walls[4] = {
				{ X - s.x, { -1.0, 0.0, 0.0 } },
				{ X + s.x, { 1.0, 0.0, 0.0 } },
				{ Y - s.y, { 0.0, -1.0, 0.0 } },
				{ Y + s.y, { 0.0, 1.0, 0.0 } }
			};
			for (const auto& w : walls) {
				if (w.gap < r) {
					AddContact(a_index, -1, s - w.n * r, w.n, r - w.gap, _spec.feltFriction * 0.7, _spec.wallRestitution, {},
						DiceEvent::Type::kDieWall);
				}
			}
		}
	}

	void DiceSim::CollideCup(int a_index, const Vec3& a_cupVel, const Vec3& a_cupSpin)
	{
		const Die&   d = _dice[a_index];
		const double e = HalfInner();
		const double r = Radius();
		const double R = _spec.cupRadius;
		const double H = _spec.cupHeight;
		const double W = _spec.cupWall;
		const double Rout = R + W;

		const Vec3   c = _cupRot.InverseRotate(d.pos - _cupPos);
		const double cr = m::Sqrt(c.x * c.x + c.y * c.y);
		const bool   inside = cr < R && c.z > -_spec.dieSize * 0.5 && c.z < H;
		const bool   below = cr < Rout && c.z <= -_spec.dieSize * 0.5;

		// Skip dice nowhere near the cup.
		if (!inside && (cr > Rout + _spec.dieSize || c.z > H + _spec.dieSize || c.z < -W - _spec.dieSize * 1.5)) {
			return;
		}

		auto add = [&](const Vec3& a_sphere, const Vec3& a_nLocal, double a_depth) {
			const Vec3 n = _cupRot.Rotate(a_nLocal);
			const Vec3 p = a_sphere - n * r;
			const Vec3 v = a_cupVel + a_cupSpin.Cross(p - _cupPos);
			AddContact(a_index, -1, p, n, a_depth, _spec.cupFriction, _spec.cupRestitution, v, DiceEvent::Type::kDieCup);
		};

		for (const auto& corner : kCorners) {
			const Vec3   sw = d.pos + d.rot.Rotate(Offset(corner, e));
			const Vec3   s = _cupRot.InverseRotate(sw - _cupPos);
			const double sr = m::Sqrt(s.x * s.x + s.y * s.y);

			if (inside) {
				// Inside wall, below the rim.
				if (s.z < H + r * 0.5 && sr + r > R && sr > 1e-9) {
					add(sw, { -s.x / sr, -s.y / sr, 0.0 }, sr + r - R);
				}
				// Base.
				if (s.z < r) {
					add(sw, { 0.0, 0.0, 1.0 }, r - s.z);
				}
				// Palm over the mouth.
				if (_lid && s.z + r > H) {
					add(sw, { 0.0, 0.0, -1.0 }, s.z + r - H);
				}
			} else if (below) {
				// Underside of the base.
				if (s.z + r > -W && sr < Rout) {
					add(sw, { 0.0, 0.0, -1.0 }, s.z + r + W);
				}
			} else {
				// Outside wall.
				if (s.z > -W && s.z < H && sr - r < Rout && sr > 1e-9) {
					add(sw, { s.x / sr, s.y / sr, 0.0 }, Rout - (sr - r));
				}
			}
		}
	}

	void DiceSim::CollidePair(int a_a, int a_b)
	{
		const Die&   A = _dice[a_a];
		const Die&   B = _dice[a_b];
		const double e = HalfInner();
		const double r = Radius();

		auto test = [&](const double (&a_unit)[3]) {
			const Vec3 sw = A.pos + A.rot.Rotate(Offset(a_unit, e));
			const Vec3 l = B.rot.InverseRotate(sw - B.pos);
			const Vec3 q{ m::Clamp(l.x, -e, e), m::Clamp(l.y, -e, e), m::Clamp(l.z, -e, e) };
			const Vec3 diff = l - q;
			const double dist = diff.Length();
			if (dist >= 2.0 * r) {
				return;
			}
			Vec3   nl;
			double depth;
			if (dist > 1e-9) {
				nl = diff * (1.0 / dist);
				depth = 2.0 * r - dist;
			} else {
				// The sphere's centre is inside B's inner box: push out along the nearest face.
				const double gaps[3] = { e - m::Abs(l.x), e - m::Abs(l.y), e - m::Abs(l.z) };
				int          k = 0;
				for (int j = 1; j < 3; ++j) {
					if (gaps[j] < gaps[k]) {
						k = j;
					}
				}
				const double comps[3] = { l.x, l.y, l.z };
				const double sign = comps[k] >= 0.0 ? 1.0 : -1.0;
				nl = { k == 0 ? sign : 0.0, k == 1 ? sign : 0.0, k == 2 ? sign : 0.0 };
				depth = 2.0 * r + gaps[k];
			}
			const Vec3 n = B.rot.Rotate(nl);
			AddContact(a_a, a_b, sw - n * r, n, depth, _spec.dieFriction, _spec.dieRestitution, {}, DiceEvent::Type::kDieDie);
		};

		for (const auto& corner : kCorners) {
			test(corner);
		}
		for (const auto& edge : kEdges) {
			test(edge);
		}
	}

	Vec3 DiceSim::VelocityAt(int a_index, const Vec3& a_p) const
	{
		const Die& d = _dice[a_index];
		return d.vel + d.spin.Cross(a_p - d.pos);
	}

	void DiceSim::Prepare([[maybe_unused]] double a_dt)
	{
		const double invM = 1.0 / _spec.dieMass;
		const double invI = 1.0 / Inertia();

		for (int i = 0; i < _contactCount; ++i) {
			Contact& c = _contacts[i];
			Tangents(c.n, c.t1, c.t2);

			const Vec3 rA = c.p - _dice[c.a].pos;
			const Vec3 rB = c.b >= 0 ? c.p - _dice[c.b].pos : Vec3{};

			auto mass = [&](const Vec3& a_dir) {
				double k = invM + rA.Cross(a_dir).LengthSq() * invI;
				if (c.b >= 0) {
					k += invM + rB.Cross(a_dir).LengthSq() * invI;
				}
				return k > 0.0 ? 1.0 / k : 0.0;
			};
			c.massN = mass(c.n);
			c.massT1 = mass(c.t1);
			c.massT2 = mass(c.t2);

			const Vec3   vB = c.b >= 0 ? VelocityAt(c.b, c.p) : c.surfVel;
			const double vn = (VelocityAt(c.a, c.p) - vB).Dot(c.n);
			const double restitution = c.bias;
			c.bias = vn < -kBounceThreshold ? -restitution * vn : 0.0;

			if (vn < -kSoundThreshold) {
				Emit(c.kind, c.a, -vn);
			}
		}
	}

	void DiceSim::Solve()
	{
		const double invM = 1.0 / _spec.dieMass;
		const double invI = 1.0 / Inertia();

		auto apply = [&](const Contact& a_c, const Vec3& a_impulse) {
			Die& A = _dice[a_c.a];
			A.vel += a_impulse * invM;
			A.spin += (a_c.p - A.pos).Cross(a_impulse) * invI;
			if (a_c.b >= 0) {
				Die& B = _dice[a_c.b];
				B.vel -= a_impulse * invM;
				B.spin -= (a_c.p - B.pos).Cross(a_impulse) * invI;
			}
		};

		for (int it = 0; it < kIterations; ++it) {
			for (int i = 0; i < _contactCount; ++i) {
				Contact& c = _contacts[i];

				Vec3 vB = c.b >= 0 ? VelocityAt(c.b, c.p) : c.surfVel;
				Vec3 vr = VelocityAt(c.a, c.p) - vB;

				const double vn = vr.Dot(c.n);
				const double newPn = m::Max(c.pn + c.massN * (c.bias - vn), 0.0);
				const double dPn = newPn - c.pn;
				c.pn = newPn;
				apply(c, c.n * dPn);

				vB = c.b >= 0 ? VelocityAt(c.b, c.p) : c.surfVel;
				vr = VelocityAt(c.a, c.p) - vB;
				const double limit = c.friction * c.pn;

				const double newT1 = m::Clamp(c.pt1 - c.massT1 * vr.Dot(c.t1), -limit, limit);
				const double newT2 = m::Clamp(c.pt2 - c.massT2 * vr.Dot(c.t2), -limit, limit);
				const Vec3   dPt = c.t1 * (newT1 - c.pt1) + c.t2 * (newT2 - c.pt2);
				c.pt1 = newT1;
				c.pt2 = newT2;
				apply(c, dPt);
			}
		}
	}

	void DiceSim::Correct()
	{
		Vec3 push[kDice]{};
		int  count[kDice]{};

		for (int i = 0; i < _contactCount; ++i) {
			const Contact& c = _contacts[i];
			const double   excess = c.depth - kSlop;
			if (excess <= 0.0) {
				continue;
			}
			if (c.b >= 0) {
				const Vec3 half = c.n * (excess * 0.5 * kCorrection);
				push[c.a] += half;
				push[c.b] -= half;
				++count[c.a];
				++count[c.b];
			} else {
				push[c.a] += c.n * (excess * kCorrection);
				++count[c.a];
			}
		}

		for (int i = 0; i < kDice; ++i) {
			if (count[i] > 0) {
				// Several corners pushing the same way would push several times over; the largest single push is
				// about right, and averaging keeps pushes from opposite sides (a die squeezed by the cup) honest.
				_dice[i].pos += push[i] * (1.0 / count[i]) * (count[i] > 1 ? 1.6 : 1.0);
			}
		}
	}

	void DiceSim::Integrate(double a_dt)
	{
		for (int i = 0; i < kDice; ++i) {
			Die& d = _dice[i];
			if (d.state != DieState::kActive) {
				continue;
			}

			const double speed = d.vel.Length();
			if (speed > _spec.maxSpeed) {
				d.vel *= _spec.maxSpeed / speed;
			}
			const double spin = d.spin.Length();
			if (spin > _spec.maxSpin) {
				d.spin *= _spec.maxSpin / spin;
			}

			if (_onFelt[i]) {
				d.spin *= m::Max(0.0, 1.0 - _spec.rollDamping * a_dt);
				if (d.settled && d.vel.Length() < _spec.settleSpeed && d.spin.Length() < _spec.settleSpin) {
					d.vel = {};
					d.spin = {};
				}
			}

			d.pos += d.vel * a_dt;
			d.rot.Integrate(d.spin, a_dt);
		}
	}

	void DiceSim::UpdateSettle(double a_dt)
	{
		for (int i = 0; i < kDice; ++i) {
			Die& d = _dice[i];
			if (d.state != DieState::kActive) {
				continue;
			}
			if (InCup(i)) {
				d.restTime = 0.0;
				d.settled = false;
				continue;
			}

			const bool slow = d.vel.Length() < _spec.settleSpeed && d.spin.Length() < _spec.settleSpin && _onFelt[i];
			if (!slow) {
				if (d.vel.Length() > _spec.settleSpeed * 2.0 || d.spin.Length() > _spec.settleSpin * 2.0) {
					d.restTime = 0.0;
					d.settled = false;
				}
				continue;
			}
			if (d.settled) {
				continue;
			}
			d.restTime += a_dt;
			if (d.restTime < _spec.settleTime) {
				continue;
			}

			if (TopFace(i) == 0 && d.nudges < 8) {
				// Leaning on a wall or another die: knock it over.
				d.vel.z += 18.0;
				d.vel.x += _rng.Range(-6.0, 6.0) - d.pos.x * 0.5;
				d.vel.y += _rng.Range(-6.0, 6.0) - d.pos.y * 0.5;
				d.spin += Vec3{ _rng.Range(-10.0, 10.0), _rng.Range(-10.0, 10.0), _rng.Range(-4.0, 4.0) };
				++d.nudges;
				d.restTime = 0.0;
				continue;
			}
			d.settled = true;
			d.vel = {};
			d.spin = {};
		}
	}

	void DiceSim::Emit(DiceEvent::Type a_type, int a_die, double a_speed)
	{
		double& cooldown = _soundCooldown[a_die][TypeIndex(a_type)];
		if (cooldown > 0.0 || _eventCount >= kMaxEvents) {
			return;
		}
		cooldown = kSoundCooldown;
		_events[_eventCount++] = { a_type, a_die, a_speed };
	}
}
