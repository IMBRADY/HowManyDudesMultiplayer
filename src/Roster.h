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
}
