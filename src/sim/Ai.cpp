#include "Ai.h"

namespace yacht
{
	namespace
	{
		// What each box is usually worth to a fair player; scoring well above this is good, below is a loss.
		constexpr double kUsual[kCategories] = { 2.1, 4.2, 6.3, 8.4, 10.5, 12.6, 15.0, 7.0, 10.0, 13.0, 9.0, 6.0, 22.0 };

		int Roll(Rng& a_rng)
		{
			int v = 1 + static_cast<int>(a_rng.Uniform() * 6.0);
			return v > 6 ? 6 : v;
		}
	}

	double Opponent::Value(int a_category, const int (&a_dice)[5], const Scorecard& a_card)
	{
		const int points = a_card.Preview(a_category, a_dice);
		if (points < 0) {
			return -1e9;
		}
		double v = points - kUsual[a_category];
		if (a_category <= kSixes && a_card.UpperBonus() == 0) {
			// Three of a face keeps the upper bonus on track; more is a cushion, fewer puts it at risk.
			const int face = a_category + 1;
			const double par = 3.0 * face;
			v += (points - par) * 0.6;
			if (points >= par) {
				v += 3.0;
			}
		}
		if (a_category == kYacht && points > 0) {
			v += 20.0;  // a yacht is also the key to every later yacht bonus
		}
		if (a_category == kChance) {
			v -= 3.0;  // keep Chance as a late safety net
		}
		return v;
	}

	double Opponent::BestValue(const int (&a_dice)[5], const Scorecard& a_card)
	{
		double best = -1e9;
		for (int c = 0; c < kCategories; ++c) {
			const double v = Value(c, a_dice, a_card);
			best = v > best ? v : best;
		}
		return best;
	}

	int Opponent::ChooseCategory(const int (&a_dice)[5], const Scorecard& a_card) const
	{
		int    best = -1;
		double bestValue = -1e18;
		for (int c = 0; c < kCategories; ++c) {
			if (a_card.Preview(c, a_dice) < 0) {
				continue;
			}
			const double v = Value(c, a_dice, a_card);
			if (v > bestValue) {
				bestValue = v;
				best = c;
			}
		}
		return best;
	}

	unsigned int Opponent::ChooseKeep(const int (&a_dice)[5], int a_rollsLeft, const Scorecard& a_card)
	{
		if (a_rollsLeft <= 0) {
			return 0x1F;
		}
		const double stay = BestValue(a_dice, a_card);
		unsigned int bestMask = 0x1F;
		double       bestScore = stay;

		for (unsigned int mask = 0; mask < 0x1F; ++mask) {
			// Skip keep choices that only differ by which of two equal dice is kept.
			bool dup = false;
			for (int i = 0; i < 5 && !dup; ++i) {
				for (int j = i + 1; j < 5 && !dup; ++j) {
					if (a_dice[i] == a_dice[j] && !(mask & (1u << i)) && (mask & (1u << j))) {
						dup = true;
					}
				}
			}
			if (dup) {
				continue;
			}

			double total = 0.0;
			for (int s = 0; s < samples; ++s) {
				int d[5];
				for (int i = 0; i < 5; ++i) {
					d[i] = (mask & (1u << i)) ? a_dice[i] : Roll(_rng);
				}
				// With another roll after this one, a reroll that lands badly can still be improved: count the
				// better of what it is and a modest share of what is left to play for.
				double v = BestValue(d, a_card);
				if (a_rollsLeft >= 2) {
					v = v * 0.85 + (v > stay ? v : stay) * 0.15 + 1.0;
				}
				total += v;
			}
			const double avg = total / samples;
			if (avg > bestScore) {
				bestScore = avg;
				bestMask = mask;
			}
		}
		return bestMask;
	}
}
