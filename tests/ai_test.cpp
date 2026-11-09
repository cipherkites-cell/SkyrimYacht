// The NPC opponent: plays whole games legally and to a reasonable standard.
#include "../src/sim/Ai.h"

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

	int PlayGame(Opponent& a_ai, Rng& a_rng, int& a_yachts)
	{
		Scorecard card;
		for (int turn = 0; turn < kCategories; ++turn) {
			int dice[5];
			for (int& d : dice) {
				d = 1 + static_cast<int>(a_rng.Uniform() * 6.0);
			}
			for (int rollsLeft = 2; rollsLeft > 0; --rollsLeft) {
				const unsigned int keep = a_ai.ChooseKeep(dice, rollsLeft, card);
				if (keep == 0x1F) {
					break;
				}
				for (int i = 0; i < 5; ++i) {
					if (!(keep & (1u << i))) {
						dice[i] = 1 + static_cast<int>(a_rng.Uniform() * 6.0);
					}
				}
			}
			const int cat = a_ai.ChooseCategory(dice, card);
			Check(cat >= 0 && card.Preview(cat, dice) >= 0, "the opponent always picks a legal box");
			if (Score(kYacht, dice) == 50) {
				++a_yachts;
			}
			card.Fill(cat, dice);
		}
		Check(card.Full(), "the opponent fills the whole card");
		return card.Total();
	}
}

int main()
{
	// Sensible single decisions.
	{
		Opponent  ai;
		Scorecard card;
		const int fiveSixes[5] = { 6, 6, 6, 6, 6 };
		Check(ai.ChooseKeep(fiveSixes, 2, card) == 0x1F, "stops rolling on a yacht");
		Check(ai.ChooseCategory(fiveSixes, card) == kYacht, "scores a yacht as a yacht");
		const int large[5] = { 2, 3, 4, 5, 6 };
		Check(ai.ChooseKeep(large, 2, card) == 0x1F, "stops on a large straight");
		const int fourSixes[5] = { 6, 6, 1, 6, 6 };
		Check(ai.ChooseKeep(fourSixes, 2, card) == 0x1B, "keeps four of a kind and rolls the odd one");
	}

	Opponent ai(7);
	Rng      rng(99);
	const int games = 300;
	int       total = 0, low = 1000, high = 0, yachts = 0;
	for (int g = 0; g < games; ++g) {
		const int s = PlayGame(ai, rng, yachts);
		total += s;
		low = s < low ? s : low;
		high = s > high ? s : high;
	}
	const double avg = static_cast<double>(total) / games;
	std::printf("opponent: average %.1f over %d games (low %d, high %d), yachts rolled %d\n", avg, games, low, high, yachts);
	Check(avg > 190.0, "the opponent plays well (averages over 190)");

	std::printf(g_failures ? "%d failure(s)\n" : "ai tests passed\n", g_failures);
	return g_failures ? 1 : 0;
}
