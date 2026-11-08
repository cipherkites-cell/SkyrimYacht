#pragma once

// Small vector and quaternion types for the dice simulation.
//
// The simulation has no game or standard-library dependencies, so the same code builds for the SKSE plugin, the
// native tests and the WebAssembly preview. sin and cos are our own, so every build gets the same numbers.

namespace yacht
{
	namespace m
	{
		inline double Sqrt(double a_x) { return a_x > 0.0 ? __builtin_sqrt(a_x) : 0.0; }
		inline double Abs(double a_x) { return a_x < 0.0 ? -a_x : a_x; }
		inline double Min(double a_a, double a_b) { return a_a < a_b ? a_a : a_b; }
		inline double Max(double a_a, double a_b) { return a_a > a_b ? a_a : a_b; }
		inline double Clamp(double a_x, double a_lo, double a_hi) { return a_x < a_lo ? a_lo : (a_x > a_hi ? a_hi : a_x); }

		constexpr double kPi = 3.14159265358979323846;

		// Sine by range reduction to [-pi/2, pi/2] and a Taylor series; accurate to about 1e-12.
		inline double Sin(double a_x)
		{
			const double twoPi = 2.0 * kPi;
			double       k = a_x / twoPi;
			k = static_cast<double>(static_cast<long long>(k < 0.0 ? k - 0.5 : k + 0.5));
			double x = a_x - k * twoPi;  // [-pi, pi]
			if (x > kPi * 0.5) {
				x = kPi - x;
			} else if (x < -kPi * 0.5) {
				x = -kPi - x;
			}
			const double x2 = x * x;
			double       term = x;
			double       sum = x;
			for (int i = 1; i < 12; ++i) {
				term *= -x2 / ((2.0 * i) * (2.0 * i + 1.0));
				sum += term;
			}
			return sum;
		}

		inline double Cos(double a_x) { return Sin(a_x + kPi * 0.5); }
	}

	struct Vec3
	{
		double x{ 0.0 };
		double y{ 0.0 };
		double z{ 0.0 };

		Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Vec3 operator-() const { return { -x, -y, -z }; }
		Vec3 operator*(double s) const { return { x * s, y * s, z * s }; }
		Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
		Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
		Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }

		double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
		Vec3   Cross(const Vec3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
		double LengthSq() const { return x * x + y * y + z * z; }
		double Length() const { return m::Sqrt(LengthSq()); }
		Vec3   Normalized() const
		{
			const double l = Length();
			return l > 1e-12 ? *this * (1.0 / l) : Vec3{ 0.0, 0.0, 1.0 };
		}
	};

	// Unit quaternion. Rotate(v) takes a vector from the body's frame to the world.
	struct Quat
	{
		double w{ 1.0 };
		double x{ 0.0 };
		double y{ 0.0 };
		double z{ 0.0 };

		Quat operator*(const Quat& o) const
		{
			return {
				w * o.w - x * o.x - y * o.y - z * o.z,
				w * o.x + x * o.w + y * o.z - z * o.y,
				w * o.y - x * o.z + y * o.w + z * o.x,
				w * o.z + x * o.y - y * o.x + z * o.w
			};
		}

		Quat Conjugate() const { return { w, -x, -y, -z }; }

		void Normalize()
		{
			const double l = m::Sqrt(w * w + x * x + y * y + z * z);
			if (l < 1e-12) {
				*this = {};
				return;
			}
			const double s = 1.0 / l;
			w *= s;
			x *= s;
			y *= s;
			z *= s;
		}

		Vec3 Rotate(const Vec3& v) const
		{
			// v + 2w(q x v) + 2 q x (q x v)
			const Vec3 q{ x, y, z };
			const Vec3 t = q.Cross(v) * 2.0;
			return v + t * w + q.Cross(t);
		}

		Vec3 InverseRotate(const Vec3& v) const { return Conjugate().Rotate(v); }

		static Quat FromAxisAngle(const Vec3& a_axis, double a_angle)
		{
			const Vec3   n = a_axis.Normalized();
			const double s = m::Sin(a_angle * 0.5);
			return { m::Cos(a_angle * 0.5), n.x * s, n.y * s, n.z * s };
		}

		// Advances the orientation by angular velocity a_omega (world frame, rad/s) over a_dt.
		void Integrate(const Vec3& a_omega, double a_dt)
		{
			const Quat spin{ 0.0, a_omega.x, a_omega.y, a_omega.z };
			const Quat d = spin * *this;
			w += 0.5 * a_dt * d.w;
			x += 0.5 * a_dt * d.x;
			y += 0.5 * a_dt * d.y;
			z += 0.5 * a_dt * d.z;
			Normalize();
		}

		// The angular velocity that turns a_from into a_to over a_dt.
		static Vec3 AngularVelocity(const Quat& a_from, const Quat& a_to, double a_dt)
		{
			Quat d = a_to * a_from.Conjugate();
			if (d.w < 0.0) {
				d = { -d.w, -d.x, -d.y, -d.z };
			}
			// Small-angle: omega ~ 2 * vector part / dt
			return Vec3{ d.x, d.y, d.z } * (2.0 / a_dt);
		}

		static Quat Nlerp(const Quat& a_a, const Quat& a_b, double a_t)
		{
			const double dot = a_a.w * a_b.w + a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z;
			const double s = dot < 0.0 ? -1.0 : 1.0;
			Quat         q{
                a_a.w + (a_b.w * s - a_a.w) * a_t,
                a_a.x + (a_b.x * s - a_a.x) * a_t,
                a_a.y + (a_b.y * s - a_a.y) * a_t,
                a_a.z + (a_b.z * s - a_a.z) * a_t
			};
			q.Normalize();
			return q;
		}
	};

	// xorshift32: small, deterministic, the same on every platform.
	struct Rng
	{
		unsigned int state{ 0x9E3779B9u };

		explicit Rng(unsigned int a_seed = 1) : state(a_seed ? a_seed : 0x9E3779B9u) {}

		unsigned int Next()
		{
			unsigned int s = state;
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			state = s;
			return s;
		}

		// [0, 1)
		double Uniform() { return (Next() >> 8) * (1.0 / 16777216.0); }
		double Range(double a_lo, double a_hi) { return a_lo + (a_hi - a_lo) * Uniform(); }
	};
}
