// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Phase 2: army serialisation and opponent injection.
//
// Two payload channels are captured, and the receiver prefers whichever it can
// actually use:
//
//   1. "matchup" - the string produced by the game's own custom-matchup export
//      path. This is the highest-fidelity representation available because the
//      game itself round-trips it, and it is validated on import by the game's
//      own parser.
//
//   2. "units"   - a per-instance snapshot walked out of live o_dude instances.
//      This is the independent representation required by the brief, and it is
//      the fallback when the export path is unavailable.
//
#include "Json.h"

#include <Aurie/shared.hpp>

#include <string>
#include <vector>

namespace hmd::roster
{
	// One serialised army member.
	struct Unit
	{
		std::string type;      // dude type identifier, when resolvable
		std::string name;      // display name, when resolvable
		double x = 0.0;
		double y = 0.0;
		double level = 0.0;
		double hp = 0.0;
		double max_hp = 0.0;
		double attack = 0.0;
		double speed = 0.0;
		double range = 0.0;
		double crit_chance = 0.0;
		double crit_damage = 0.0;
		bool knocked_out = false;

		json::Value ToJson() const;
		static Unit FromJson(const json::Value& Object);
	};

	// A full army payload plus the match state that travels with it.
	struct Snapshot
	{
		int protocol = 1;
		int act = 0;
		int lives = 0;
		std::string matchup;      // game's own export string; may be empty
		std::vector<Unit> units;

		std::string Serialize() const;
		static bool Deserialize(const std::string& Text, Snapshot& Out);
	};

	// Install observational detours on the game's own spawn routines.
	//
	// Purely passive: each detour logs the arguments the game passed the first
	// time it sees them and calls straight through. Nothing is altered.
	//
	// This exists because every other route is now closed. Enemy types are not
	// objects (asset_get_index finds nothing under any prefix), and every
	// custom-matchup routine parses without touching a live round. What is left
	// is the routine the game itself uses to put an enemy in a wave - and the
	// way to learn how to call it is to watch the game call it.
	//
	// Losing these costs a discovery, not the mod. A null trampoline is logged
	// and ignored.
	bool InstallSpawnObservers(Aurie::AurieModule* Module);
	void RemoveSpawnObservers(Aurie::AurieModule* Module);

	// Turn a native round export into what the opponent should fight: our dudes
	// moved into their enemies slot, our dudes cleared so they keep their own,
	// and the sender's boss dropped because a duel replaces it.
	//
	// Both maps are type-name to count, so this is a rename rather than a
	// translation. Empty string if the export cannot be transformed.
	std::string BuildDuelPayload(const std::string& Export);

	// Export the current round, then feed it to the game's own parser one
	// change at a time, on this machine.
	//
	// The single-payload version of this established that custom_matchup_parse
	// accepts the struct and then dies in matchup_calculate_difficulty_score.
	// It could not say which of the transform's four changes caused that, so
	// this runs them as separate stages against one export snapshot.
	//
	// A stage that kills the game is the result. The marker file next to the
	// DLL records which stage was in flight, so the next press resumes past it
	// rather than dying in the same place; RestartFromFirstStage ignores the
	// marker and runs the whole sequence again.
	void SelfTestDuelPayload(bool RestartFromFirstStage);

	// Run the game's own matchup exporter and report what it produced, without
	// sending anything anywhere.
	//
	// On F6 rather than only inside a duel, because the whole feature now rides
	// on this format and gating the one diagnostic that describes it behind "get
	// a second player to round 2" is how several sessions were already lost.
	void ProbeNativeExport();

	// Walk live o_dude instances and build a snapshot of the local army.
	// Returns false and logs if no army could be found (e.g. not in rm_gameplay).
	bool CaptureLocalArmy(Snapshot& Out);

	// Remove the wave the game would otherwise fight and mark default wave
	// generation as suppressed. Safe to call when no enemies exist.
	void ClearDefaultEnemyWave();

	// Instantiate the peer's army as the opponent wave. Returns the number of
	// units successfully spawned.
	int InjectOpponentArmy(const Snapshot& Peer);

	// True while the mod wants the game's own wave composition suppressed.
	bool IsDefaultWaveSuppressed();
	void SetDefaultWaveSuppressed(bool Suppressed);

	// How many enemy instances ClearDefaultEnemyWave has destroyed since the
	// counter was last reset.
	//
	// This is the difference between "the duel was called off and the round is
	// exactly as the game left it" and "the duel was called off after the mod
	// deleted the wave the player was supposed to fight". Those need different
	// things said to the player, and the mod cannot tell them apart after the
	// fact any other way.
	int EnemiesCleared();
	void ResetEnemiesCleared();
}
