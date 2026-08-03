// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Where the local player is in their run, and where the run map draws it.
//
// Two things live here because they are discovered the same way. YYC leaves no
// bytecode and no variable layout, so neither "what round am I on" nor "where
// is round 17 drawn" can be answered from data.win. Both are answered by
// hooking the scripts that already handle them and reading what the game hands
// in:
//
//   round_number_frame(...)  is called with the round it is about to display.
//   run_map_cell(...)        is called once per cell, with that cell's round,
//                            and returns the cakeframe it built.
//
// Neither hook changes anything. They observe, record, and call the original.
//
#include <Aurie/shared.hpp>
#include <YYToolkit/YYTK_Shared.hpp>

namespace hmd::runstate
{
	// A rectangle in whatever coordinate space the cakeframe system is drawing
	// in. Read back from the game rather than assumed, so the overlay lands in
	// the same space the UI it annotates is drawn in.
	struct Box
	{
		double left = 0.0;
		double top = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		bool valid = false;

		double Width() const { return right - left; }
		double Height() const { return bottom - top; }
		double CenterX() const { return (left + right) * 0.5; }
	};

	bool Initialize(Aurie::AurieModule* Module);
	void Shutdown(Aurie::AurieModule* Module);

	// Called once per tick. Ages out stale run-map cells.
	void Tick();

	// Ask the cakeframe system for a frame's on-screen rectangle. Returns false
	// if the value is not a frame or the accessor is unavailable.
	bool ReadCakeframeBox(const YYTK::RValue& Frame, Box& Out);

	// The round the local player is currently on, or 0 if it is not known.
	//
	// Strict on purpose. Under YYC the round number is only discoverable from a
	// running game, and it may not be discoverable at all - so this reports "I
	// do not know" rather than guessing. A guess here would be worse than
	// useless: an estimate derived from the act number is always an exact
	// multiple of the duel interval, which would read as "every moment is a
	// duel round".
	int CurrentRound();

	// True once a real round number has been observed, so callers can pick
	// between round-accurate behaviour and the act-based fallback.
	bool RoundTrackingAvailable();

	// --- Counting rounds --------------------------------------------------
	//
	// The round number could not be read out of the game: round_number_frame
	// turns out to take no arguments (it reads the round itself), and the
	// global holding it could not be found by name. So the mod counts instead.
	//
	// victory_ui_round_finish fires once per round - the name says round, and
	// the logs confirm it fires after round 1 - and new_run_start fires when a
	// run begins. Those two are enough to know the round exactly, and both are
	// hooks already proven to work on this build.

	// A run has begun: the player is on round 1.
	void NoteRunStarted();

	// A round has been completed: the player is moving to the next one.
	void NoteRoundFinished();

	// The run is over or abandoned; stop reporting a round.
	void NoteRunEnded();

	// How far through the run the player is, in rounds, always answerable.
	// The real round when it is known, and act * DuelInterval when it is not.
	// This is what the two clients compare to decide whether they have both
	// reached the same duel, because it is defined on both of them whatever
	// each was able to resolve.
	int CurrentProgress();

	// The act number, straight from the game's own accessor. 0 if unavailable.
	int CurrentAct();

	// Rounds between duels. Defaults to 20 - the length of an act, which is the
	// round that would otherwise be a boss fight.
	int DuelInterval();
	void SetDuelInterval(int Rounds);

	// True if this round is one where the boss fight is replaced by a duel.
	bool IsDuelRound(int Round);

	// The next round at or after Round that is a duel round.
	int NextDuelRound(int Round);

	// The only round whose cell position is worth measuring - the one the
	// opponent is on. Measuring a cell means calling back into the game to ask
	// for its rectangle, and run_map_cell runs for every cell on the map every
	// frame, so measuring all of them would be hundreds of script calls a
	// second to place one sprite. 0 disables the measurement entirely.
	void SetTrackedRound(int Round);

	// The on-screen rectangle of a run-map cell, if that round was drawn
	// recently. False if the run map is not up, that cell is not on it, or it
	// is not the tracked round.
	bool CellBox(int Round, Box& Out);

	// The largest rectangle the cakeframe system has drawn recently, which is
	// how the overlay finds the edges of the screen without having to know
	// whether the UI is being drawn in GUI space or world space.
	bool DrawSpace(Box& Out);

	// Record a frame that is about to be drawn. Called from the cf_draw hook.
	void NoteDrawnFrame(const Box& Frame);

	// Log what resolved and what did not.
	void Report();
}
