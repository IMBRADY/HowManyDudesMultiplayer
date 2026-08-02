// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Phase 3: peer-to-peer transport.
//
// Design note - why sockets rather than ENet:
//
// The brief allows "ENet or socket equivalent". The exchange this mod performs
// is one small JSON payload per act, strictly request/response, with no
// real-time state to reconcile - the brief explicitly rules out physics sync.
// That is a workload TCP already solves: ordered, reliable, and with no
// reliability layer for us to get wrong. Vendoring ENet would add a build
// dependency and a UDP reliability surface to debug in exchange for latency
// characteristics this mod cannot use. So: a direct TCP link between the two
// peers, one side hosting, no server in the middle.
//
// All socket work happens on a dedicated worker thread. The game thread only
// ever touches the mutex-guarded queues via Send/Poll, so nothing here can
// block the frame loop.
//
#include <string>

namespace hmd::net
{
	enum class LinkState
	{
		Idle,          // No session.
		Listening,     // Hosting, waiting for a peer to connect.
		Discovering,   // Joining, looking for a host on the local network.
		Connecting,    // Joining, dialling the host.
		Handshaking,   // Socket up, verifying the peer speaks this protocol.
		Connected,     // Handshake complete, payloads may flow.
		Failed         // Session ended in an error; see LastError().
	};

	// LAN discovery listens one port above the session port, so a single
	// configured value covers both.
	unsigned short DiscoveryPortFor(unsigned short SessionPort);

	// Session-wide settings. Applied to every session opened afterwards.
	struct Options
	{
		// Shared passphrase. Both peers must present the same value or the
		// handshake fails. Empty means "no passphrase" - the handshake still
		// runs and still rejects anything that is not this mod speaking this
		// protocol, but it will not distinguish one HMD peer from another.
		std::string session_key;

		// Answer LAN discovery probes while hosting, and send them while
		// joining. Turning this off makes the mod address-only.
		bool enable_discovery = true;
	};

	void Configure(const Options& NewOptions);

	// Start/stop the worker thread. Safe to call Shutdown when never started.
	bool Initialize();
	void Shutdown();

	// Begin hosting on a port, or join a peer. Either call replaces any session
	// already in progress.
	bool Host(unsigned short Port);
	bool Join(const std::string& Address, unsigned short Port);

	// Join without knowing the host's address: broadcast a discovery probe on
	// the local network and dial whoever answers. Falls back to no session (and
	// a logged error) if nothing responds.
	bool JoinDiscovered(unsigned short Port);

	// The address of the connected peer, once a session is up. Empty otherwise.
	std::string PeerAddress();

	// Tear down the current session without shutting the subsystem down.
	void Disconnect();

	LinkState State();
	const char* StateName();
	std::string LastError();

	// True once the handshake has completed and payloads may be exchanged.
	bool IsConnected();

	// Queue a payload for delivery. Returns false if there is no live session.
	bool Send(const std::string& Payload);

	// Pop the next received payload, if any. Returns false when the queue is
	// empty. Never blocks.
	bool Poll(std::string& OutPayload);
}
