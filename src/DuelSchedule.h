// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// When duels happen.
//
// Header-only and free of any dependency on the runtime, deliberately: this is
// the arithmetic that decides which round replaces its boss fight with a duel,
// and getting it wrong is the difference between duelling every twentieth
// round and duelling permanently. Keeping it here means the offline harness can
// test it, which nothing else in the duel path can be.
//
namespace hmd::duel
{
	// The default gap between duels. An act is this many rounds and ends in a
	// boss, so the duel lands exactly where that boss would have been.
	inline constexpr int kDefaultInterval = 20;

	// Intervals outside this range are refused rather than clamped, so a typo
	// in the config is reported instead of silently changing the pacing.
	inline constexpr int kMinInterval = 1;
	inline constexpr int kMaxInterval = 500;

	inline bool IsValidInterval(int Interval)
	{
		return Interval >= kMinInterval && Interval <= kMaxInterval;
	}

	// True if this round is one where the boss is replaced by a duel.
	//
	// Round 0 is not a duel round. That matters more than it looks: 0 is what
	// the round tracker reports when it does not know, and treating "unknown"
	// as a duel would fire the gate the moment a run started.
	inline bool IsDuelRound(int Round, int Interval)
	{
		if (Round <= 0 || !IsValidInterval(Interval))
			return false;

		return (Round % Interval) == 0;
	}

	// The first duel round at or after Round. Answers with the interval itself
	// for any round at or below the first one, so "when is my next duel" has an
	// answer before the run has started.
	inline int NextDuelRound(int Round, int Interval)
	{
		if (!IsValidInterval(Interval))
			return 0;

		if (Round <= 0)
			return Interval;

		return ((Round + Interval - 1) / Interval) * Interval;
	}

	// How far through a run a player is, in rounds, when the real round number
	// could not be resolved. An act is exactly one interval of rounds, so
	// having finished act N means N * Interval rounds played. Exact at the act
	// boundary, which is the only place the two clients compare it.
	inline int ProgressFromAct(int Act, int Interval)
	{
		if (Act <= 0 || !IsValidInterval(Interval))
			return 0;

		return Act * Interval;
	}

	// The inverse: how many acts a player has got through by the time they are
	// standing on Round.
	//
	// This exists because the act used to be read out of the game, by calling
	// gml_Script_get_act_number. That script calls gold_star_round internally,
	// and gold_star_round aborts the game with "I32 argument is undefined" when
	// it is entered from anywhere but the game's own progression flow - which is
	// exactly what a mod calling it from a tick is. It crashed a tester twice in
	// twenty seconds. The act is only ever displayed or logged, never used to
	// gate anything, so deriving it from the round we already know is both safe
	// and free.
	//
	// Floor, not ceiling, so that it round-trips with ProgressFromAct: round 20
	// is the end of act 1, and ProgressFromAct(1) is 20.
	inline int ActFromRound(int Round, int Interval)
	{
		if (Round <= 0 || !IsValidInterval(Interval))
			return 0;

		return Round / Interval;
	}
}
