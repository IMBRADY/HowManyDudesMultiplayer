// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Payload sanitisation.
//
// Deliberately depends on nothing but Json.h - no YYTK, no sockets - so every
// rule in here is exercised by the offline harness rather than only in-game.
//
// Two independent jobs:
//
//   1. Deciding whether a string really is a custom-matchup export. This is
//      used in BOTH directions, for different reasons:
//
//        - Outbound, because the export path works through the clipboard. If
//          the game's exporter does not actually write to the clipboard, the
//          value read back is whatever the player had copied there - which must
//          never be handed to a peer.
//
//        - Inbound, because the receiving side feeds that string to the game's
//          own parser, which was written for text a player pastes in, not for
//          bytes arriving off a socket.
//
//   2. Bounding every numeric field that arrives from a peer, so a modified
//      client cannot spawn a unit with unreachable stats.
//
#include "Json.h"

#include <string>

namespace hmd::sanitize
{
	// Structural limits. Far below what the transport allows on purpose: the
	// transport's 4 MB frame cap exists to stop memory exhaustion, these exist
	// to stop nonsense that is technically well-formed.
	inline constexpr size_t kMaxUnits = 256;
	inline constexpr size_t kMaxMatchupBytes = 256u * 1024u;
	inline constexpr size_t kMaxTextField = 64;

	// A matchup payload must carry at least this many of the keys the game's
	// own exporter writes. Three is comfortably more than clipboard junk will
	// hit by accident, and comfortably fewer than a real export contains.
	inline constexpr int kMinMatchupKeys = 3;

	// Stat bounds. Generous enough that legitimate late-act values pass through
	// untouched, tight enough that an injected unit stays killable.
	inline constexpr double kMaxLevel = 1000.0;
	inline constexpr double kMaxHealth = 1.0e7;
	inline constexpr double kMaxAttack = 1.0e6;
	inline constexpr double kMaxSpeed = 1.0e4;
	inline constexpr double kMaxRange = 1.0e5;
	inline constexpr double kMaxCoordinate = 1.0e6;
	inline constexpr double kMaxCritChance = 1.0;
	inline constexpr double kMaxCritDamage = 100.0;

	// Clamp to [Minimum, Maximum]. NaN and both infinities collapse to Minimum,
	// so a hostile payload cannot smuggle a non-finite value into the runtime -
	// GameMaker will happily accept one and then behave unpredictably.
	double ClampNumber(double Value, double Minimum, double Maximum);

	// Truncate to kMaxTextField and drop control characters, so a peer-supplied
	// name cannot corrupt console output or carry embedded newlines.
	std::string ClampText(const std::string& Value);

	// True when Candidate is a plausible custom-matchup export: non-empty, no
	// larger than kMaxMatchupBytes, parses as a JSON object, and carries at
	// least kMinMatchupKeys of the exporter's own top-level keys.
	bool IsMatchupPayload(const std::string& Candidate);
}
