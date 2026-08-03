// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Steam lobby + peer-to-peer transport.
//
// Why this exists: the TCP transport works, but it makes two friends deal with
// ports. On a LAN that is hidden by UDP discovery; over the internet it means a
// port forward or a mesh VPN. Steam removes the problem outright - its relay
// handles NAT traversal, so a session needs no ports, no addresses, and no
// router configuration. The join flow becomes Shift+Tab, Invite Friend, click.
//
// Why no Steamworks SDK: the game already loads steam_api64.dll, and that DLL
// exports the whole flat C API. Everything here is resolved out of the module
// the game already has open, so there is no SDK to vendor, nothing to link, and
// no second copy of Steam's runtime in the process. If the DLL or any function
// is missing, Available() stays false and the mod falls back to TCP rather than
// failing - the same graceful-degradation rule the rest of the mod follows.
//
#include <cstdint>
#include <string>

namespace hmd::steam
{
	enum class LobbyState
	{
		None,        // No lobby.
		Creating,    // CreateLobby issued, waiting on the result.
		Hosting,     // We own a lobby and are waiting for someone to join.
		Joining,     // Entering a lobby we were invited to.
		Joined       // In a lobby with a peer; the P2P channel is usable.
	};

	// Binds to steam_api64.dll and resolves the interfaces. Safe to call when
	// Steam is not present; returns false and leaves the subsystem inert.
	bool Initialize();
	void Shutdown();

	// True when every required export resolved and the interfaces are live.
	bool Available();

	// Pumped from the frame callback. Polls pending async calls and drains
	// incoming P2P messages. Cheap and safe to call when unavailable.
	void Tick();

	// --- Session ---------------------------------------------------------

	// Create a lobby and become the host. The invite overlay can be opened once
	// the lobby exists.
	bool HostSession();

	// Open Steam's own "invite a friend" overlay for the current lobby. This is
	// the same dialog Shift+Tab shows. Does nothing if we are not hosting.
	bool OpenInviteOverlay();

	// Leave the lobby and drop the P2P session.
	void LeaveSession();

	LobbyState State();
	const char* StateName();

	// The peer's Steam persona name once connected, else empty.
	std::string PeerName();

	// This player's own Steam persona name, or empty if Steam is unavailable.
	//
	// Sent to the peer in the status beacon rather than relying on them looking
	// it up: over a direct TCP session there is no lobby to read a name out of,
	// and the opponent badge should still say who you are.
	std::string LocalName();

	// --- Transport -------------------------------------------------------
	// Mirrors hmd::net's surface so the two are interchangeable.

	bool IsConnected();
	bool Send(const std::string& Payload);
	bool Poll(std::string& OutPayload);
}
