// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Phase 3: end-of-act interception, payload exchange, and the shared 3-lives
// system.
//
// The state machine is driven from the frame callback rather than from inside
// a hook. That ordering matters: a hook fires on the game's own thread in the
// middle of the game's own call stack, which is the worst place to block on a
// network round-trip. The hook's only job is to *notice* the act boundary and
// raise a flag; all the work happens on the next frame tick.
//
#include "Roster.h"

#include <YYToolkit/YYTK_Shared.hpp>

#include <string>

namespace hmd::match
{
	enum class Phase
	{
		Offline,       // No peer session.
		Idle,          // Connected, nothing pending.
		Capturing,     // Act ended; snapshotting the local army.
		AwaitingPeer,  // Local payload sent; waiting for the peer's.
		Injecting,     // Peer payload received; building the opponent wave.
		Fighting,      // Opponent wave is live.
		Resolving,     // Battle finished; settling lives.
		GameOver       // A player has run out of lives.
	};

	// Lives are tracked for both sides so either client can render the score
	// and detect game over without a round trip.
	//
	// A match never survives a disconnect: dropping the link ends it, and the
	// next connection starts a fresh one at 3 lives each.
	struct Lives
	{
		int local = 3;
		int remote = 3;
	};

	// What the opponent last told us they were doing. The two runs are not
	// step-locked, so this is what lets a client say "waiting for your opponent
	// to start a run" rather than appearing to hang.
	struct PeerPresence
	{
		bool known = false;   // False until the first status beacon arrives.
		bool in_run = false;  // In rm_gameplay with a live army.
		int act = 0;
	};

	bool Initialize(Aurie::AurieModule* Module);
	void Shutdown(Aurie::AurieModule* Module);

	// Called once per rendered frame. Drives the whole exchange.
	void Tick();

	// Raised by the end-of-act hook (or by the poller, if the hook is not
	// available) to signal that the act has finished.
	void SignalActEnded();

	// True while the mod wants the game's default wave generation suppressed.
	bool ShouldSuppressDefaultWave();

	Phase CurrentPhase();
	const char* PhaseName();
	Lives CurrentLives();

	// The peer's last reported state. `known` is false if nothing has arrived
	// recently enough to trust, so callers can distinguish "in the menu" from
	// "we have not heard from them".
	PeerPresence CurrentPeerPresence();

	// True when this client has an army in a live run.
	bool LocalInRun();

	// Reset the session state without dropping the network link. Used when a
	// new run starts.
	void ResetMatchState();
}
