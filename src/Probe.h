// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Runtime discovery.
//
// This build is YYC, which means data.win carries no CODE and no VARI chunk:
// asset and script *names* survive, but no bytecode and no instance variable
// layout. Several things the UI features need - which code entries run inside
// the draw pipeline, what the current round number is called, what shape a
// run-map cell is - are simply not knowable from the file. They are knowable
// from a running game, and this is what asks.
//
// Everything here is diagnostic. Nothing in the mod's behaviour depends on the
// probe having run, and the probe never writes to game state.
//
#include <YYToolkit/YYTK_Shared.hpp>

namespace hmd::probe
{
	// Record a code entry seen by the EVENT_OBJECT_CALL callback. Cheap enough
	// to call unconditionally: after the census is closed this is a single
	// atomic load and a return.
	void NoteCodeEntry(YYTK::CCode* Code);

	// True while the census is still accepting new names.
	bool CensusOpen();

	// Dump everything discovered so far: the code-entry census, the global
	// variable listing, and the resolution state of the things the features
	// depend on. Bound to a hotkey and also fired once automatically a few
	// seconds after the first tick.
	void Report();

	// Called every tick. Closes the census once it has had long enough to see a
	// representative sample, and fires the automatic report.
	void Tick();
}
