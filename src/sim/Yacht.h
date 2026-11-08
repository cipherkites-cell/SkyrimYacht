#pragma once

// Yacht scoring: the thirteen-box scorecard with the upper-section bonus, five-of-a-kind bonuses and joker rule.
// Plain C++ with no dependencies (tests/yacht_test.cpp).

namespace yacht
{
	enum Category : int
	{
		kOnes,
		kTwos,
		kThrees,
		kFours,
		kFives,
		kSixes,
		kThreeKind,
		kFourKind,
		kFullHouse,
		kSmallStraight,
		kLargeStraight,
		kYacht,  // five of a kind
		kChance,
		kCategories
	};

	constexpr int kUpperBonusAt = 63;
	constexpr int kUpperBonus = 35;
	constexpr int kYachtScore = 50;
	constexpr int kYachtBonus = 100;

	const char* CategoryName(int a_category);

	// What a_dice (five values 1..6) score in a category, by the plain rules (no joker).
	int Score(int a_category, const int (&a_dice)[5]);

	struct Scorecard
	{
		int boxes[kCategories];
		int yachtBonuses{ 0 };

		Scorecard() { Clear(); }
		void Clear();

		bool Filled(int a_category) const { return boxes[a_category] >= 0; }
		bool Full() const;

		// Whether a_dice is an extra five-of-a-kind that plays as a joker in the lower section.
		bool Joker(const int (&a_dice)[5]) const;
		// What writing a_dice into a_category would score, including the joker rule. -1 if the box can't be used
		// (already filled, or the joker rule forces the matching upper box).
		int Preview(int a_category, const int (&a_dice)[5]) const;
		// Writes the roll into a box. Returns the points scored (box plus any five-of-a-kind bonus), or -1 if not allowed.
		int Fill(int a_category, const int (&a_dice)[5]);

		int UpperSum() const;
		int UpperBonus() const { return UpperSum() >= kUpperBonusAt ? kUpperBonus : 0; }
		int Total() const;
	};
}
