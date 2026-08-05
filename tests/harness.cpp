// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Offline test harness.
//
// Covers the parts of the mod that do not need a live GameMaker runtime: the
// JSON codec, the payload sanitiser, and the TCP transport (framing, handshake,
// and LAN discovery). Everything that touches YYTK is exercised only in-game,
// so this harness deliberately links just Json.cpp, Sanitize.cpp and Net.cpp.
//
// Logging in those units routes through hmd::g_Interface, which stays null
// here; the log helpers early-out on null, so this also proves the mod does not
// fault when the interface is unavailable.
//
// winsock2.h must precede anything that pulls in windows.h (Log.h does).
#include <winsock2.h>
#include <ws2tcpip.h>

#include "DuelSchedule.h"
#include "Json.h"
#include "Log.h"
#include "Net.h"
#include "ProbeJournal.h"
#include "Sanitize.h"

#pragma comment(lib, "ws2_32.lib")

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace hmd
{
	// The harness stands in for ModuleMain.cpp, which owns this in the real DLL.
	YYTK::YYTKInterface* g_Interface = nullptr;
}

namespace
{
	int g_Passed = 0;
	int g_Failed = 0;

	void Check(bool Condition, const char* Description)
	{
		if (Condition)
		{
			g_Passed++;
			printf("  PASS  %s\n", Description);
		}
		else
		{
			g_Failed++;
			printf("  FAIL  %s\n", Description);
		}
	}

	// --- Raw peer helpers -------------------------------------------------
	// These stand in for the second client, speaking the wire protocol by hand
	// so the transport is tested against bytes rather than against itself.

	SOCKET ConnectRawClient(unsigned short Port)
	{
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client == INVALID_SOCKET)
			return INVALID_SOCKET;

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(Port);
		inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

		if (connect(client, reinterpret_cast<sockaddr*>(&address),
			sizeof(address)) == SOCKET_ERROR)
		{
			closesocket(client);
			return INVALID_SOCKET;
		}

		// Bound so a missing reply fails the test rather than hanging it.
		DWORD timeout = 5000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&timeout), sizeof(timeout));

		return client;
	}

	bool SendRawFrame(SOCKET Client, const std::string& Payload)
	{
		char header[8];
		memcpy(header, "HMD1", 4);
		uint32_t length = htonl(static_cast<uint32_t>(Payload.size()));
		memcpy(header + 4, &length, 4);

		if (send(Client, header, sizeof(header), 0) != sizeof(header))
			return false;

		return send(Client, Payload.data(), static_cast<int>(Payload.size()), 0) ==
			static_cast<int>(Payload.size());
	}

	bool ReceiveRawFrame(SOCKET Client, std::string& Out)
	{
		char header[8]{};
		if (recv(Client, header, sizeof(header), MSG_WAITALL) != sizeof(header))
			return false;

		if (memcmp(header, "HMD1", 4) != 0)
			return false;

		uint32_t length = 0;
		memcpy(&length, header + 4, 4);
		length = ntohl(length);

		Out.assign(length, '\0');
		if (length == 0)
			return true;

		return recv(Client, Out.data(), static_cast<int>(length), MSG_WAITALL) ==
			static_cast<int>(length);
	}

	std::string MakeHello(int Protocol, const std::string& Key)
	{
		hmd::json::Value hello = hmd::json::Value::Object();
		hello.Set("hmd", hmd::json::Value(Protocol));
		hello.Set("key", hmd::json::Value(Key));
		return hello.Serialize();
	}

	// Completes the handshake as a well-behaved peer would.
	bool ClientHandshake(SOCKET Client, const std::string& Key)
	{
		if (!SendRawFrame(Client, MakeHello(1, Key)))
			return false;

		std::string hello;
		return ReceiveRawFrame(Client, hello);
	}

	bool WaitForConnected(bool Expected, int AttemptLimit = 150)
	{
		for (int i = 0; i < AttemptLimit; i++)
		{
			if (hmd::net::IsConnected() == Expected)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}

	void TestJsonBasics()
	{
		printf("\n[json] basics\n");

		hmd::json::Value root = hmd::json::Value::Object();
		root.Set("proto", hmd::json::Value(1));
		root.Set("name", hmd::json::Value("dude \"one\"\n"));
		root.Set("hp", hmd::json::Value(37.5));
		root.Set("ko", hmd::json::Value(true));

		hmd::json::Value units = hmd::json::Value::Array();
		units.Push(hmd::json::Value(1));
		units.Push(hmd::json::Value(2));
		root.Set("units", units);

		const std::string text = root.Serialize();

		hmd::json::Value parsed;
		Check(hmd::json::Parse(text, parsed), "round-trips through the parser");
		Check(parsed["proto"].AsInt() == 1, "integer survives the round trip");
		Check(parsed["hp"].AsNumber() == 37.5, "double survives the round trip");
		Check(parsed["ko"].AsBool(), "bool survives the round trip");
		Check(parsed["name"].AsString() == "dude \"one\"\n",
			"escapes survive the round trip");
		Check(parsed["units"].Size() == 2, "array length survives the round trip");
		Check(parsed["units"].Items()[1].AsInt() == 2, "array element is intact");

		// Integers must not pick up a decimal tail.
		Check(text.find("\"proto\":1") != std::string::npos,
			"integers serialise without a decimal tail");

		Check(parsed.Members().size() == 5, "object members are enumerable");
		Check(hmd::json::Value(1).Members().empty(),
			"non-objects enumerate as having no members");
	}

	void TestJsonRobustness()
	{
		printf("\n[json] malformed input is rejected, not tolerated\n");

		const char* bad[] = {
			"{",
			"}",
			"{\"a\":}",
			"{\"a\" 1}",
			"[1,2",
			"{\"a\":1}trailing",
			"\"unterminated",
			"",
			"{\"a\":01x}",
		};

		bool all_rejected = true;
		for (const char* text : bad)
		{
			hmd::json::Value parsed;
			if (hmd::json::Parse(text, parsed))
			{
				printf("        (accepted bad input: %s)\n", text);
				all_rejected = false;
			}
		}
		Check(all_rejected, "every malformed document is rejected");

		// Deep nesting must hit the depth guard rather than the stack.
		std::string deep;
		for (int i = 0; i < 5000; i++) deep += "[";
		hmd::json::Value parsed;
		Check(!hmd::json::Parse(deep, parsed),
			"pathologically nested input is refused without a stack overflow");

		// Missing keys must yield defaults rather than faulting.
		hmd::json::Value empty = hmd::json::Value::Object();
		Check(empty["nope"].AsInt(42) == 42, "missing key falls back cleanly");
		Check(empty["nope"]["deeper"].AsString("d") == "d",
			"chained missing keys fall back cleanly");
	}

	void TestSanitizeNumbers()
	{
		printf("\n[sanitize] peer-supplied numbers are bounded\n");

		using hmd::sanitize::ClampNumber;

		Check(ClampNumber(5.0, 0.0, 10.0) == 5.0, "an in-range value is untouched");
		Check(ClampNumber(-1.0, 0.0, 10.0) == 0.0, "a low value clamps up");
		Check(ClampNumber(1e300, 0.0, 10.0) == 10.0, "an absurd value clamps down");

		// The three that would otherwise reach the runtime as-is.
		const double nan_value = std::nan("");
		const double infinity = HUGE_VAL;
		Check(ClampNumber(nan_value, 0.0, 10.0) == 0.0, "NaN collapses to the minimum");
		Check(ClampNumber(infinity, 0.0, 10.0) == 0.0, "+infinity collapses to the minimum");
		Check(ClampNumber(-infinity, 0.0, 10.0) == 0.0, "-infinity collapses to the minimum");
	}

	void TestSanitizeText()
	{
		printf("\n[sanitize] peer-supplied text is bounded\n");

		using hmd::sanitize::ClampText;

		Check(ClampText("goon").size() == 4, "ordinary text passes through");
		Check(ClampText(std::string(500, 'x')).size() ==
			hmd::sanitize::kMaxTextField, "over-long text is truncated");
		Check(ClampText("a\nb\tc").find('\n') == std::string::npos,
			"embedded newlines are stripped");
		Check(ClampText("a\nb").size() == 2, "control characters are dropped entirely");
	}

	// Everything ui::Notify says ends up inside the game's own markup parser,
	// which aborts the game rather than complaining when it dislikes its input.
	// One tester's game died on "variable_struct_set: illegal to use empty
	// names" coming out of cf_parse, so what reaches it is pinned down here.
	void TestSanitizeNotifications()
	{
		printf("\n[sanitize] notifications are safe to hand to cf_parse\n");

		using hmd::sanitize::ClampNotification;

		Check(ClampNotification("You beat Nur's army!") == "You beat Nur's army!",
			"an ordinary message passes through unchanged");
		Check(ClampNotification("Round 20 - duel round.") ==
			"Round 20 - duel round.", "punctuation the mod actually uses survives");

		// The injection vector: opponent names come from Steam and are whatever
		// that player typed into it.
		Check(ClampNotification("Connected to [rainbow]bob.").find('[') ==
			std::string::npos, "markup brackets are stripped");
		Check(ClampNotification("hi {x} there").find('{') == std::string::npos,
			"so are braces");
		Check(ClampNotification("a<b>c").find('<') == std::string::npos,
			"so are angle brackets");

		// Stripping must not weld words together, and must not leave the ragged
		// whitespace that stripping produces.
		Check(ClampNotification("one[]two") == "one two",
			"stripped markup leaves a separator, not a join");
		Check(ClampNotification("  lots   of   space  ") == "lots of space",
			"whitespace is collapsed and trimmed");

		// The case that actually killed a game: nothing renderable left. The
		// caller must be able to tell, so it can skip the game call entirely.
		Check(ClampNotification("").empty(), "empty input stays empty");
		Check(ClampNotification("[[[]]]").empty(),
			"a message made only of markup collapses to empty");
		Check(ClampNotification("\n\t\r").empty(),
			"a message made only of control characters collapses to empty");

		Check(ClampNotification(std::string(1000, 'x')).size() ==
			hmd::sanitize::kMaxNotification, "an over-long message is truncated");

		// A truncated multi-byte sequence is possible because messages are
		// assembled byte-wise from several sources.
		Check(ClampNotification("caf\xC3").find('\xC3') == std::string::npos,
			"a dangling multi-byte lead is dropped");
	}

	void TestSanitizeMatchupDetection()
	{
		printf("\n[sanitize] only real matchup payloads are trusted\n");

		using hmd::sanitize::IsMatchupPayload;

		const std::string real =
			R"({"dudes":[],"enemies":[],"relics":[],"cash":10,"boss":0})";
		Check(IsMatchupPayload(real), "a real export is recognised");

		const std::string wrapped =
			R"({"version":2,"data":{"dudes":[],"enemies":[],"relics":[]}})";
		Check(IsMatchupPayload(wrapped), "one level of wrapping is tolerated");

		// The clipboard-leak cases: whatever the player happened to have copied
		// must never be mistaken for an export.
		Check(!IsMatchupPayload(""), "empty input is rejected");
		Check(!IsMatchupPayload("hunter2"), "a stray password is rejected");
		Check(!IsMatchupPayload("https://example.com/private"),
			"a copied URL is rejected");
		Check(!IsMatchupPayload(R"({"note":"remember the milk"})"),
			"unrelated JSON is rejected");
		Check(!IsMatchupPayload(R"({"dudes":[]})"),
			"a single matching key is not enough");
		Check(!IsMatchupPayload("[1,2,3]"), "a JSON array is rejected");
		Check(!IsMatchupPayload(std::string(hmd::sanitize::kMaxMatchupBytes + 1, 'x')),
			"an oversized payload is rejected on length alone");

		// The duel payload changed shape when injection stopped translating
		// dude types into enemy names: the army now travels in `dudes` and
		// `enemies` is emptied. That gate is what every peer payload passes
		// through before the mod will read it, so emptying a key it counts is
		// exactly the kind of change that breaks the duel silently at the
		// receiver - it would look like "the peer sent nothing".
		//
		// This is the real BuildDuelPayload output, taken verbatim from a
		// one-dude export.
		const std::string duel =
			R"({"difficulty_score":0.0,"non_boss_enemies":{},"relic_order":[],)"
			R"("dudes":{"basic":1.0},"roster_order":["basic"],)"
			R"("arena_modifiers":[],"boss_fight_id":"","cash":100.0,)"
			R"("trinket_dude_types":{},"tier":1.0,"relics":{},)"
			R"("estimated_dude_count":0.0,"food":{},"estimated_relic_count":0.0,)"
			R"("estimated_round":0.0,"food_ids":[],"consumables":{},)"
			R"("dude_type_trinkets":{},"enemies":{}})";

		Check(IsMatchupPayload(duel),
			"the duel payload still validates with 'enemies' emptied");
	}

	void TestNetLoopback()
	{
		printf("\n[net] loopback session\n");

		hmd::net::Configure(hmd::net::Options{});
		Check(hmd::net::Initialize(), "subsystem initialises");

		constexpr unsigned short kPort = 47899;
		Check(hmd::net::Host(kPort), "host session queued");

		// Give the worker a moment to reach the listening state.
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);

		SOCKET client = ConnectRawClient(kPort);
		Check(client != INVALID_SOCKET, "client connects to the hosted session");

		Check(ClientHandshake(client, {}), "handshake completes in both directions");
		Check(WaitForConnected(true), "transport reports the link as connected");
		Check(hmd::net::PeerAddress() == "127.0.0.1",
			"the peer's address is reported");

		// Send a framed payload the way a peer would.
		const std::string payload = R"({"kind":"army","body":{"proto":1}})";
		Check(SendRawFrame(client, payload), "framed payload is sent");

		std::string received;
		bool got_it = false;
		for (int i = 0; i < 100 && !got_it; i++)
		{
			got_it = hmd::net::Poll(received);
			if (!got_it)
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		Check(got_it, "framed payload is received");
		Check(received == payload, "payload arrives byte-identical");

		// Now the other direction.
		const std::string outgoing = R"({"kind":"result","won":true})";
		Check(hmd::net::Send(outgoing), "outbound payload is accepted");

		std::string echoed;
		Check(ReceiveRawFrame(client, echoed), "outbound frame arrives well-formed");
		Check(echoed == outgoing, "outbound payload arrives byte-identical");

		closesocket(client);
		WSACleanup();

		hmd::net::Disconnect();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		hmd::net::Shutdown();
		Check(true, "subsystem shuts down without hanging");
	}

	void TestNetRejectsGarbage()
	{
		printf("\n[net] a wrong-protocol peer is rejected\n");

		hmd::net::Configure(hmd::net::Options{});
		Check(hmd::net::Initialize(), "subsystem re-initialises");

		constexpr unsigned short kPort = 47898;
		hmd::net::Host(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);

		SOCKET client = ConnectRawClient(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));

		// Eight bytes of noise: right length for a header, wrong magic.
		const char garbage[8] = { 'N', 'O', 'P', 'E', 0, 0, 0, 1 };
		send(client, garbage, sizeof(garbage), 0);

		Check(WaitForConnected(false),
			"link is dropped rather than deserialising garbage");

		std::string nothing;
		Check(!hmd::net::Poll(nothing), "no payload is surfaced from the bad frame");

		closesocket(client);
		WSACleanup();
		hmd::net::Shutdown();
	}

	void TestHandshakeGuardsTheSession()
	{
		printf("\n[net] the handshake gates who may exchange payloads\n");

		hmd::net::Options options;
		options.session_key = "correct horse";
		hmd::net::Configure(options);
		Check(hmd::net::Initialize(), "subsystem initialises with a session key");

		constexpr unsigned short kPort = 47897;
		hmd::net::Host(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);

		// 1. Wrong passphrase.
		SOCKET intruder = ConnectRawClient(kPort);
		Check(intruder != INVALID_SOCKET, "an uninvited peer can still open a socket");
		SendRawFrame(intruder, MakeHello(1, "battery staple"));

		Check(WaitForConnected(false), "a wrong session key never reaches connected");

		std::string nothing;
		Check(!hmd::net::Poll(nothing),
			"no payload is accepted from a peer that failed the handshake");
		closesocket(intruder);

		// 2. Right passphrase, wrong protocol version.
		hmd::net::Host(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		SOCKET wrong_version = ConnectRawClient(kPort);
		SendRawFrame(wrong_version, MakeHello(99, "correct horse"));
		Check(WaitForConnected(false), "a mismatched protocol version is refused");
		closesocket(wrong_version);

		// 3. The invited peer.
		hmd::net::Host(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));

		SOCKET friendly = ConnectRawClient(kPort);
		Check(ClientHandshake(friendly, "correct horse"),
			"the matching passphrase completes the handshake");
		Check(WaitForConnected(true), "the invited peer reaches connected");
		closesocket(friendly);

		WSACleanup();
		hmd::net::Disconnect();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		hmd::net::Shutdown();
	}

	void TestDiscovery()
	{
		printf("\n[net] a host answers local-network discovery probes\n");

		Check(hmd::net::DiscoveryPortFor(47801) == 47802,
			"discovery sits one port above the session port");
		Check(hmd::net::DiscoveryPortFor(65535) == 65534,
			"the top of the port range does not overflow");

		hmd::net::Configure(hmd::net::Options{});
		hmd::net::Initialize();

		constexpr unsigned short kPort = 47896;
		hmd::net::Host(kPort);
		std::this_thread::sleep_for(std::chrono::milliseconds(400));

		WSADATA wsa{};
		WSAStartup(MAKEWORD(2, 2), &wsa);

		SOCKET prober = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		DWORD timeout = 2000;
		setsockopt(prober, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&timeout), sizeof(timeout));

		sockaddr_in destination{};
		destination.sin_family = AF_INET;
		destination.sin_port = htons(hmd::net::DiscoveryPortFor(kPort));
		inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);

		// Malformed probes must be ignored without upsetting the responder.
		const char runt[3] = { 'H', 'M', 'D' };
		sendto(prober, runt, sizeof(runt), 0,
			reinterpret_cast<sockaddr*>(&destination), sizeof(destination));

		char probe[7]{};
		memcpy(probe, "HMDD", 4);
		probe[4] = '?';
		unsigned short encoded = htons(kPort);
		memcpy(probe + 5, &encoded, 2);

		int sent = sendto(prober, probe, sizeof(probe), 0,
			reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
		Check(sent == sizeof(probe), "a discovery probe is sent");

		char reply[64]{};
		sockaddr_in from{};
		int from_length = sizeof(from);
		int received = recvfrom(prober, reply, sizeof(reply), 0,
			reinterpret_cast<sockaddr*>(&from), &from_length);

		Check(received == 7, "the host replies with a well-formed datagram");
		Check(received == 7 && memcmp(reply, "HMDD", 4) == 0,
			"the reply carries the discovery magic");
		Check(received == 7 && reply[4] == '!', "the reply is tagged as a reply");

		unsigned short advertised = 0;
		if (received == 7)
			memcpy(&advertised, reply + 5, 2);
		Check(ntohs(advertised) == kPort,
			"the reply advertises the host's session port");

		closesocket(prober);
		WSACleanup();

		hmd::net::Disconnect();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		hmd::net::Shutdown();
	}

	// --- Duel schedule ----------------------------------------------------
	//
	// This is the arithmetic that decides which round's boss fight becomes a
	// duel. Its failure modes are not subtle - an off-by-one here means either
	// no duels at all or a duel that never ends - but they are invisible
	// without two machines, so they are pinned down here instead.
	void TestDuelSchedule()
	{
		using namespace hmd::duel;

		printf("\n[duel] which rounds are duel rounds\n");

		Check(IsDuelRound(20, 20), "round 20 is a duel round");
		Check(IsDuelRound(40, 20), "round 40 is a duel round");
		Check(IsDuelRound(60, 20), "round 60 is a duel round");
		Check(!IsDuelRound(19, 20), "round 19 is not");
		Check(!IsDuelRound(21, 20), "round 21 is not");
		Check(!IsDuelRound(1, 20), "the first round is not");

		// The round tracker reports 0 when it could not resolve a round. If
		// that counted as a duel round the gate would fire the instant a run
		// started, empty the arena, and wait forever for an opponent who is
		// not at a duel either.
		Check(!IsDuelRound(0, 20), "an unknown round is never a duel round");
		Check(!IsDuelRound(-5, 20), "a negative round is never a duel round");

		printf("\n[duel] when the next duel is\n");

		Check(NextDuelRound(0, 20) == 20, "before the run starts, it is round 20");
		Check(NextDuelRound(1, 20) == 20, "from round 1, it is round 20");
		Check(NextDuelRound(19, 20) == 20, "from round 19, it is round 20");
		Check(NextDuelRound(20, 20) == 20, "round 20 is its own next duel");
		Check(NextDuelRound(21, 20) == 40, "from round 21, it is round 40");
		Check(NextDuelRound(40, 20) == 40, "round 40 is its own next duel");
		Check(NextDuelRound(41, 20) == 60, "from round 41, it is round 60");

		printf("\n[duel] a custom interval\n");

		Check(IsDuelRound(5, 5), "an interval of 5 duels on round 5");
		Check(!IsDuelRound(6, 5), "and not on round 6");
		Check(NextDuelRound(6, 5) == 10, "the next one is round 10");
		Check(IsDuelRound(7, 1), "an interval of 1 duels every round");

		printf("\n[duel] rejected intervals\n");

		Check(!IsValidInterval(0), "an interval of 0 is refused");
		Check(!IsValidInterval(-1), "a negative interval is refused");
		Check(!IsValidInterval(100000), "an absurd interval is refused");
		Check(IsValidInterval(20), "the default interval is accepted");

		// A bad interval must not make every round a duel round by way of a
		// modulo against zero, which would also be a divide by zero.
		Check(!IsDuelRound(20, 0), "a zero interval never reports a duel round");
		Check(NextDuelRound(20, 0) == 0, "and never schedules one");

		printf("\n[duel] progress when the round number is unknown\n");

		// Both clients have to agree on how far along each other is, even when
		// one of them could only resolve the act.
		Check(ProgressFromAct(1, 20) == 20, "finishing act 1 is 20 rounds in");
		Check(ProgressFromAct(3, 20) == 60, "finishing act 3 is 60 rounds in");
		Check(ProgressFromAct(0, 20) == 0, "an unknown act reports no progress");
		Check(ProgressFromAct(2, 0) == 0, "a bad interval reports no progress");

		// The estimate has to line up with the real thing, or a player whose
		// round resolved would never match one whose did not.
		Check(IsDuelRound(ProgressFromAct(2, 20), 20),
			"an act-derived progress lands on a real duel round");

		printf("\n[duel] the act, derived rather than asked for\n");

		// The act used to come from gml_Script_get_act_number, which can abort
		// the game when a mod calls it. It is derived now, and the only thing
		// that makes that substitution honest is that it agrees with the value
		// it replaced at the boundaries anyone looks at.
		Check(ActFromRound(20, 20) == 1, "round 20 is the end of act 1");
		Check(ActFromRound(40, 20) == 2, "round 40 is the end of act 2");
		Check(ActFromRound(1, 20) == 0, "round 1 is still act 0");
		Check(ActFromRound(19, 20) == 0, "and so is round 19");
		Check(ActFromRound(21, 20) == 1, "round 21 is one act in");

		Check(ActFromRound(0, 20) == 0, "an unknown round reports no act");
		Check(ActFromRound(-5, 20) == 0, "and so does a negative one");
		Check(ActFromRound(20, 0) == 0, "a bad interval reports no act");

		// The round trip is the whole justification for using floor.
		Check(ProgressFromAct(ActFromRound(20, 20), 20) == 20,
			"act and progress round-trip on a duel round");
		Check(ProgressFromAct(ActFromRound(60, 20), 20) == 60,
			"and on a later one");
	}

	// --- The probe journal ------------------------------------------------
	//
	// This is here rather than in the game because the thing it protects
	// against is a writer and a reader disagreeing about a line, and that class
	// of bug has cost this project two rounds of play once already: a probe
	// reported "no spawn code has been observed yet" while sitting on a full
	// file, because the writer emitted three fields and the reader wanted four.
	//
	// The journal decides which probes a launch is allowed to run. If it reads
	// its own output wrongly, a bisect either repeats a probe that kills the
	// game or skips one that was never asked - and both look like results.
	void TestProbeJournalFormat()
	{
		using namespace hmd::journal;

		printf("\n[journal] a line survives a round trip\n");

		Check(FormatLine("payload/3-boss", Status::Attempted) ==
			"payload/3-boss attempted\n", "attempted renders as two tokens");

		Check(FormatLine("x", Status::Survived) == "x survived\n",
			"and so does survived");

		Check(FormatLine("x", Status::Skipped) == "x skipped\n",
			"and skipped");

		// An id with whitespace would write a line that parses as a different
		// id plus a junk status. It cannot happen from a literal, which is
		// exactly why it would go unnoticed if it ever did.
		Check(FormatLine("two words", Status::Attempted) ==
			"two_words attempted\n", "whitespace in an id is closed off");

		const std::vector<Entry> round_trip = Parse(
			FormatLine("a/1", Status::Attempted) +
			FormatLine("a/1", Status::Survived));

		Check(round_trip.size() == 2, "both lines parse back");
		Check(round_trip.size() == 2 && round_trip[0].id == "a/1" &&
			round_trip[0].status == Status::Attempted,
			"the first keeps its id and status");
		Check(round_trip.size() == 2 && round_trip[1].status == Status::Survived,
			"and so does the second");

		printf("\n[journal] a torn file yields everything before the tear\n");

		// The ordinary result of the process dying mid-write. A parser that
		// gives up here would report an empty journal and re-run the probe
		// that just killed the game.
		const std::vector<Entry> torn =
			Parse("a attempted\nb survived\nc att");

		Check(torn.size() == 2, "the complete lines survive a truncated last one");

		Check(Parse("").empty(), "an empty journal parses to nothing");
		Check(Parse("\n\n\n").empty(), "and so does a file of blank lines");
		Check(Parse("nonsense here\n").empty(),
			"an unknown status is dropped rather than guessed");
		Check(Parse("lonely\n").empty(), "and so is a line with no status");

		// A journal written by a future build with extra columns must not read
		// as garbage - the first two tokens are the contract.
		const std::vector<Entry> extra = Parse("a attempted 12:01:33 whatever\n");
		Check(extra.size() == 1 && extra[0].status == Status::Attempted,
			"trailing columns are ignored rather than fatal");
	}

	void TestProbeJournalDecides()
	{
		using namespace hmd::journal;

		printf("\n[journal] what a launch is allowed to run\n");

		const auto path = std::filesystem::temp_directory_path() /
			"hmd_probe_journal_test.txt";

		std::error_code ignored;
		std::filesystem::remove(path, ignored);

		{
			Journal fresh(path);

			Check(!fresh.WasEverAttempted("injection/generate-spawn"),
				"nothing is attempted in a journal that does not exist yet");
			Check(!fresh.IsProvenLethal("injection/generate-spawn"),
				"and nothing is lethal");
		}

		{
			// A probe that ran and came back.
			Journal survivor(path);
			survivor.Record("apply/one", Status::Attempted);
			survivor.Record("apply/one", Status::Survived);

			Check(survivor.WasEverAttempted("apply/one"),
				"an entry recorded this session counts immediately");
			Check(!survivor.IsProvenLethal("apply/one"),
				"one that came back is not lethal");
		}

		{
			// A probe that armed and never returned - the game died inside it.
			// Nothing writes the Survived line in that case, which is the
			// entire signal.
			Journal casualty(path);
			casualty.Record("apply/two", Status::Attempted);
		}

		{
			Journal next_launch(path);

			Check(next_launch.WasEverAttempted("apply/one"),
				"a survival is still on record after a relaunch");
			Check(!next_launch.IsProvenLethal("apply/one"),
				"and is still not lethal");

			Check(next_launch.WasEverAttempted("apply/two"),
				"so is an attempt that never returned");
			Check(next_launch.IsProvenLethal("apply/two"),
				"and that one is what killed the game");

			Check(next_launch.Lethal().size() == 1,
				"exactly one entry is named as lethal");
			Check(next_launch.Lethal().size() == 1 &&
				next_launch.Lethal().front() == "apply/two",
				"and it is the one that did not come back");

			// Skipping is not consuming. A probe the mod declined to run has
			// not had its launch, and the next press must ask again - "SKIPPED
			// BY THE MOD is not a pass" is a lesson this file now enforces.
			next_launch.Record("apply/three", Status::Skipped);

			Check(!next_launch.WasEverAttempted("apply/three"),
				"a skip does not consume the entry");
			Check(!next_launch.IsProvenLethal("apply/three"),
				"and does not make it look lethal");
		}

		{
			Journal after_skip(path);
			Check(!after_skip.WasEverAttempted("apply/three"),
				"a skip is still not a consumption after a relaunch");
		}

		{
			Journal to_clear(path);
			to_clear.Clear();

			Check(!to_clear.WasEverAttempted("apply/one"),
				"clearing re-opens every question");
			Check(to_clear.Lethal().empty(), "including the lethal ones");
		}

		Check(!std::filesystem::exists(path, ignored),
			"and it removes the file");

		std::filesystem::remove(path, ignored);
	}
}

int main()
{
	printf("HowManyDudesMultiplayer - offline test harness\n");

	TestDuelSchedule();
	TestProbeJournalFormat();
	TestProbeJournalDecides();

	TestJsonBasics();
	TestJsonRobustness();
	TestSanitizeNumbers();
	TestSanitizeText();
	TestSanitizeNotifications();
	TestSanitizeMatchupDetection();
	TestNetLoopback();
	TestNetRejectsGarbage();
	TestHandshakeGuardsTheSession();
	TestDiscovery();

	printf("\n%d passed, %d failed\n", g_Passed, g_Failed);
	return g_Failed == 0 ? 0 : 1;
}
