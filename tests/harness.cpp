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

#include "Json.h"
#include "Log.h"
#include "Net.h"
#include "Sanitize.h"

#pragma comment(lib, "ws2_32.lib")

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

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
}

int main()
{
	printf("HowManyDudesMultiplayer - offline test harness\n");

	TestJsonBasics();
	TestJsonRobustness();
	TestSanitizeNumbers();
	TestSanitizeText();
	TestSanitizeMatchupDetection();
	TestNetLoopback();
	TestNetRejectsGarbage();
	TestHandshakeGuardsTheSession();
	TestDiscovery();

	printf("\n%d passed, %d failed\n", g_Passed, g_Failed);
	return g_Failed == 0 ? 0 : 1;
}
