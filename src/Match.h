// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Phase 3: duel interception, payload exchange, and the shared 3-lives system.
//
// The state machine is driven from the frame callback rather than from inside
// a hook. That ordering matters: a hook fires on the game's own thread in the
// middle of the game's own call stack, which is the worst place to block on a
// network round-trip. The hook's only job is to *notice* something and raise a
// flag; all the work happens on the next frame tick.
//
// Duels replace boss fights. Every DuelInterval rounds - 20 by default, which
// is the length of an act and therefore the round that would otherwise be a
// boss - the two players fight each other's armies instead of the game's boss.
// Because the two runs are not step-locked, whoever arrives first waits: the
// arena is held empty behind a banner until the opponent reaches the same
// round, and only then are armies exchanged.
//
#include "Roster.h"

#include <YYToolkit/YYTK_Shared.hpp>

#include <string>

namespace hmd::match
{
	enum class Phase
	{
		Offline,           // No peer session.
		Idle,              // Connected, nothing pending.
		AwaitingPeerRound, // At a duel round; waiting for the opponent to reach it.
		Capturing,         // Both at the duel round; snapshotting the local army.
		AwaitingPeer,      // Local payload sent; waiting for the peer's.
		Injecting,         // Peer payload received; building the opponent wave.
		Fighting,          // Opponent wave is live.
		Resolving,         // Battle finished; settling lives.
		GameOver           // A player has run out of lives.
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
	// to reach round 20" rather than appearing to hang, and it is what places
	// their marker on the round track.
	struct PeerPresence
	{
		bool known = false;   // False until the first status beacon arrives.
		bool in_run = false;  // In rm_gameplay with a live army.
		int act = 0;
		int round = 0;        // 0 when their client could not resolve one.

		// How far through their run they are, in rounds. Always populated -
		// their real round when they know it, an act-derived estimate when they
		// do not - so the two clients can always compare progress even if only
		// one of them managed to resolve a round number.
		int progress = 0;

		std::string name;     // Their Steam persona name, if they have one.
	};

	bool Initialize(Aurie::AurieModule* Module);
	void Shutdown(Aurie::AurieModule* Module);

	// Called once per rendered frame. Drives the whole exchange.
	void Tick();

	// Raised by the end-of-round hook. No longer starts an exchange - duels are
	// triggered by reaching a duel round, not by finishing an act - but it is
	// still the most reliable signal that the run has moved on, so it is used
	// to re-read the round and to clear the completed-duel guard.
	void SignalActEnded();

	// Whether one player pressing "start run" pulls the other into a run too.
	void SetSyncRunStart(bool Enabled);

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

	// Log what the duel machinery resolved to.
	void Report();

	// -----------------------------------------------------------------------
	// Solo duel - the duel path with the network taken out
	// -----------------------------------------------------------------------
	//
	// Fights a synthetic army built from a config string instead of one that
	// arrived from a peer. Everything after that point is the REAL path:
	// ClearDefaultEnemyWave, InjectOpponentArmy, SpawnOneInjectedUnit, both
	// counter corrections, EvaluateBattle, ApplyOutcome, RemoveInjectedWave.
	// Only the socket is absent.
	//
	// This exists because every duel question so far has cost a two-machine
	// session to ask, and none of them was actually about the network. The
	// project's own first process lesson says to build this before the
	// two-player test rather than after it; this is that, late.
	//
	// TURNING IT OFF: set `solo_duel = false` in the ini, which is the default.
	// The hotkey then declines with a message and nothing else in the mod
	// behaves differently - every solo-specific branch is behind
	// SoloDuelEnabled() or the per-duel active flag, and a real duel is
	// refused while one is running rather than being silently altered.
	void SetSoloDuel(bool Enabled, const std::string& Army);

	// Whether the ini turned it on. False in ordinary multiplayer use.
	bool SoloDuelEnabled();

	// True only between the start of a solo duel and its resolution. This is
	// what makes the peer-result wait and the result broadcast fall away, and
	// it is cleared on every exit path including the failure ones.
	bool SoloDuelActive();

	// Begin one. Declines, with a logged reason, if solo duels are off, if a
	// peer is connected, if the player is not in a live round, or if a duel of
	// either kind is already in progress.
	void StartSoloDuel();
}
