// Native tests for Yacht scoring.
#include "../src/sim/Yacht.h"

#include <cstdio>

using namespace yacht;

namespace
{
	int g_failures = 0;

	void Expect(int a_got, int a_want, const char* a_what)
	{
		if (a_got != a_want) {
			std::printf("FAIL: %s: got %d, want %d\n", a_what, a_got, a_want);
			++g_failures;
		}
	}
}

int main()
{
	const int fullHouse[5] = { 2, 2, 3, 3, 3 };
	const int small[5] = { 1, 3, 4, 5, 6 };
	const int large[5] = { 2, 3, 4, 5, 6 };
	const int fours[5] = { 4, 4, 4, 4, 1 };
	const int sixes[5] = { 6, 6, 6, 6, 6 };
	const int junk[5] = { 1, 1, 3, 5, 6 };

	Expect(Score(kThrees, fullHouse), 9, "threes");
	Expect(Score(kFullHouse, fullHouse), 25, "full house");
	Expect(Score(kFullHouse, sixes), 0, "five of a kind is not a full house (plain rules)");
	Expect(Score(kSmallStraight, small), 30, "small straight with a gap at 2");
	Expect(Score(kSmallStraight, large), 30, "large straight also counts as small");
	Expect(Score(kLargeStraight, small), 0, "1 3 4 5 6 is not a large straight");
	Expect(Score(kLargeStraight, large), 40, "large straight");
	Expect(Score(kThreeKind, fours), 17, "three of a kind sums all dice");
	Expect(Score(kFourKind, fours), 17, "four of a kind");
	Expect(Score(kFourKind, fullHouse), 0, "no four of a kind");
	Expect(Score(kYacht, sixes), 50, "yacht");
	Expect(Score(kChance, junk), 16, "chance");

	Scorecard card;
	Expect(card.Fill(kYacht, sixes), 50, "first yacht");
	Expect(card.Fill(kYacht, sixes), -1, "box already filled");
	Expect(card.Preview(kFullHouse, sixes), -1, "joker must use the open sixes box first");
	Expect(card.Fill(kSixes, sixes), 130, "second yacht into sixes: 30 + 100 bonus");
	Expect(card.Preview(kFullHouse, sixes), 25, "joker full house");
	Expect(card.Preview(kLargeStraight, sixes), 40, "joker large straight");
	Expect(card.Preview(kOnes, sixes), -1, "joker can't go upper while lower boxes are open");
	Expect(card.Fill(kLargeStraight, sixes), 140, "third yacht as a large straight with bonus");

	Scorecard upper;
	const int ones[5] = { 1, 1, 1, 2, 2 };
	const int twos[5] = { 2, 2, 2, 1, 1 };
	const int threes[5] = { 3, 3, 3, 1, 1 };
	const int fs[5] = { 4, 4, 4, 1, 1 };
	const int fives[5] = { 5, 5, 5, 1, 1 };
	const int six[5] = { 6, 6, 6, 1, 1 };
	upper.Fill(kOnes, ones);
	upper.Fill(kTwos, twos);
	upper.Fill(kThrees, threes);
	upper.Fill(kFours, fs);
	upper.Fill(kFives, fives);
	upper.Fill(kSixes, six);
	Expect(upper.UpperSum(), 63, "three of each face = 63");
	Expect(upper.UpperBonus(), 35, "upper bonus at 63");
	Expect(upper.Total(), 98, "total with bonus");

	Scorecard zero;
	Expect(zero.Fill(kYacht, junk), 0, "scratching the yacht box");
	Expect(zero.Fill(kFives, sixes), -1, "joker still forces the open sixes box after a scratched yacht");
	Expect(zero.Fill(kSixes, sixes), 30, "no bonus after a scratched yacht");

	std::printf(g_failures ? "%d failure(s)\n" : "yacht tests passed\n", g_failures);
	return g_failures ? 1 : 0;
}
