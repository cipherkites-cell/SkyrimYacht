#include "Yacht.h"

namespace yacht
{
	namespace
	{
		void Count(const int (&a_dice)[5], int (&a_counts)[7])
		{
			for (int& c : a_counts) {
				c = 0;
			}
			for (int v : a_dice) {
				if (v >= 1 && v <= 6) {
					++a_counts[v];
				}
			}
		}

		int Sum(const int (&a_dice)[5])
		{
			int s = 0;
			for (int v : a_dice) {
				s += v;
			}
			return s;
		}

		int MaxCount(const int (&a_counts)[7])
		{
			int best = 0;
			for (int v = 1; v <= 6; ++v) {
				best = a_counts[v] > best ? a_counts[v] : best;
			}
			return best;
		}

		int LongestRun(const int (&a_counts)[7])
		{
			int best = 0;
			int run = 0;
			for (int v = 1; v <= 6; ++v) {
				run = a_counts[v] > 0 ? run + 1 : 0;
				best = run > best ? run : best;
			}
			return best;
		}

		bool FiveOfAKind(const int (&a_dice)[5])
		{
			for (int v : a_dice) {
				if (v != a_dice[0] || v < 1 || v > 6) {
					return false;
				}
			}
			return true;
		}
	}

	const char* CategoryName(int a_category)
	{
		static const char* names[kCategories] = { "一点", "二点", "三点", "四点", "五点", "六点", "三条",
			"四条", "葫芦", "小顺子", "大顺子", "快艇", "机会" };
		return a_category >= 0 && a_category < kCategories ? names[a_category] : "";
	}

	int Score(int a_category, const int (&a_dice)[5])
	{
		int counts[7];
		Count(a_dice, counts);

		switch (a_category) {
		case kOnes:
		case kTwos:
		case kThrees:
		case kFours:
		case kFives:
		case kSixes:
			{
				const int face = a_category + 1;
				return counts[face] * face;
			}
		case kThreeKind:
			return MaxCount(counts) >= 3 ? Sum(a_dice) : 0;
		case kFourKind:
			return MaxCount(counts) >= 4 ? Sum(a_dice) : 0;
		case kFullHouse:
			{
				bool three = false;
				bool two = false;
				for (int v = 1; v <= 6; ++v) {
					three = three || counts[v] == 3;
					two = two || counts[v] == 2;
				}
				return three && two ? 25 : 0;
			}
		case kSmallStraight:
			return LongestRun(counts) >= 4 ? 30 : 0;
		case kLargeStraight:
			return LongestRun(counts) >= 5 ? 40 : 0;
		case kYacht:
			return MaxCount(counts) == 5 ? kYachtScore : 0;
		case kChance:
			return Sum(a_dice);
		default:
			return 0;
		}
	}

	void Scorecard::Clear()
	{
		for (int& b : boxes) {
			b = -1;
		}
		yachtBonuses = 0;
	}

	bool Scorecard::Full() const
	{
		for (int b : boxes) {
			if (b < 0) {
				return false;
			}
		}
		return true;
	}

	bool Scorecard::Joker(const int (&a_dice)[5]) const
	{
		return FiveOfAKind(a_dice) && Filled(kYacht);
	}

	int Scorecard::Preview(int a_category, const int (&a_dice)[5]) const
	{
		if (a_category < 0 || a_category >= kCategories || Filled(a_category)) {
			return -1;
		}
		if (!Joker(a_dice)) {
			return Score(a_category, a_dice);
		}

		// Joker: the matching upper box must be used if it's open; otherwise any lower box at full value, and an
		// upper box only once every lower box is full (scoring zero unless it matches).
		const int upper = a_dice[0] - 1;
		if (!Filled(upper)) {
			return a_category == upper ? Score(a_category, a_dice) : -1;
		}
		switch (a_category) {
		case kFullHouse:
			return 25;
		case kSmallStraight:
			return 30;
		case kLargeStraight:
			return 40;
		case kThreeKind:
		case kFourKind:
		case kChance:
			return Score(a_category, a_dice);
		default:
			break;
		}
		if (a_category <= kSixes) {
			for (int c = kThreeKind; c < kCategories; ++c) {
				if (c != kYacht && !Filled(c)) {
					return -1;
				}
			}
			return Score(a_category, a_dice);
		}
		return -1;
	}

	int Scorecard::Fill(int a_category, const int (&a_dice)[5])
	{
		const int points = Preview(a_category, a_dice);
		if (points < 0) {
			return -1;
		}
		int bonus = 0;
		if (FiveOfAKind(a_dice) && Filled(kYacht) && boxes[kYacht] == kYachtScore) {
			++yachtBonuses;
			bonus = kYachtBonus;
		}
		boxes[a_category] = points;
		return points + bonus;
	}

	int Scorecard::UpperSum() const
	{
		int s = 0;
		for (int c = kOnes; c <= kSixes; ++c) {
			s += boxes[c] > 0 ? boxes[c] : 0;
		}
		return s;
	}

	int Scorecard::Total() const
	{
		int s = UpperBonus() + yachtBonuses * kYachtBonus;
		for (int b : boxes) {
			s += b > 0 ? b : 0;
		}
		return s;
	}
}
