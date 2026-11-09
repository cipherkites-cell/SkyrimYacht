#pragma once

// The NPC opponent's decisions: which dice to keep between rolls, and which box to score.
// Plain C++ with no dependencies (tests/ai_test.cpp).
//
// Keeping: every one of the 32 keep choices is tried against a few hundred simulated rerolls, and the one whose
// outcomes score best (by the same box-value judgement used for scoring) wins. Scoring: the box whose points beat
// that box's usual worth by the most, so a weak roll is spent on the box that loses least.

#include "Math.h"
#include "Yacht.h"

namespace yacht
{
	class Opponent
	{
	public:
		explicit Opponent(unsigned int a_seed = 1) : _rng(a_seed) {}

		// Bit i set = keep die i. With a_rollsLeft == 0, or when rolling can't help, returns 0x1F (keep all: stop).
		unsigned int ChooseKeep(const int (&a_dice)[5], int a_rollsLeft, const Scorecard& a_card);
		// The box to write this roll into (always one the card accepts).
		int ChooseCategory(const int (&a_dice)[5], const Scorecard& a_card) const;

		// How good writing a_dice into a_category would be for this card: points over the box's usual worth, plus
		// progress toward the upper bonus.
		static double Value(int a_category, const int (&a_dice)[5], const Scorecard& a_card);
		// The best Value over every box the card can still take.
		static double BestValue(const int (&a_dice)[5], const Scorecard& a_card);

		int samples{ 160 };  // simulated rerolls per keep choice

	private:
		Rng _rng;
	};
}
