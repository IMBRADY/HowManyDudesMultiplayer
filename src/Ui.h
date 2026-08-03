// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// In-game presentation.
//
// Everything the mod knew used to go to aurie.log, which meant a session that
// worked perfectly looked identical to one that had never connected. This is
// the channel that puts it on screen instead.
//
// Two mechanisms, chosen per message rather than one being forced to do both:
//
//   1. The game's own info stream (hmd_infostream_spawn) for things that
//      happen once - connected, opponent reached the duel, life lost. It is
//      already styled, already positioned, already in the game's font, and
//      costs one script call.
//
//   2. A drawn overlay for things that are true continuously - the opponent's
//      name in the menu, their marker on the round track. This hangs off
//      cf_draw, the cakeframe system's own draw entry point, so it renders in
//      the same coordinate space and at the same pipeline stage as the UI it
//      annotates.
//
// The overlay never reads match state directly. Match pushes what it wants
// shown through SetOverlay once a tick, which keeps the dependency one-way and
// keeps drawing off the network path.
//
#include <Aurie/shared.hpp>
#include <YYToolkit/YYTK_Shared.hpp>

#include <string>

namespace hmd::ui
{
	// What the overlay should be showing. Pushed by the match layer each tick.
	struct Overlay
	{
		// A peer is connected and the badge should be shown.
		bool connected = false;

		// The opponent's Steam persona name, or empty if we do not know it
		// (a direct TCP session with Steam unavailable on their end).
		std::string peer_name;

		// The round the opponent is on, or 0 if unknown. Drives the marker on
		// the round track.
		int peer_round = 0;

		// True when the opponent is actually in a run. A marker is only drawn
		// when they are.
		bool peer_in_run = false;

		// A short line to show across the top of the arena, or empty for none.
		// Used for the duel gate: "WAITING FOR <NAME> - ROUND 20".
		std::string banner;
	};

	bool Initialize(Aurie::AurieModule* Module);
	void Shutdown(Aurie::AurieModule* Module);

	// Resets the per-frame draw budget. Called once per tick.
	void Tick();

	// Replace what the overlay draws.
	void SetOverlay(const Overlay& NewOverlay);

	// Put a one-off message on screen through the game's own info stream, and
	// mirror it to the log. Formats like printf.
	void Notify(const char* Format, ...);

	// True if the info stream accepted at least one message, so callers can
	// tell whether the player is actually seeing any of this.
	bool NotificationsAvailable();

	// Turn the on-screen message stream off. It reaches the game through a
	// routine whose signature YYC did not preserve, so it gets a kill switch:
	// if it misbehaves it can be disabled from the ini instead of needing a new
	// build. Messages still go to the log regardless.
	void SetNotificationsEnabled(bool Enabled);

	// Log what resolved.
	void Report();
}
