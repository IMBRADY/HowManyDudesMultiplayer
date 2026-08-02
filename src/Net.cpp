// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Net.h"

// winsock2.h must precede anything that drags in windows.h (Log.h ->
// YYTK_Shared.hpp does), otherwise the old winsock.h definitions win and the
// two headers collide.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Json.h"
#include "Log.h"
#include "Steam.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace hmd::net
{
	namespace
	{
		// Wire framing: magic, then a big-endian payload length, then payload.
		// The magic makes a wrong-port connection fail loudly instead of
		// deserialising garbage.
		constexpr char kMagic[4] = { 'H', 'M', 'D', '1' };
		constexpr uint32_t kMaxPayload = 4u * 1024u * 1024u;
		constexpr int kPollIntervalMs = 20;

		// Handshake. Accepting a TCP connection proves nothing about who is on
		// the other end - the listener binds every interface, so on a forwarded
		// port that is any host on the internet. Before a single byte of game
		// data is exchanged, both sides must present this protocol version and
		// the configured passphrase.
		constexpr int kHandshakeProtocol = 1;
		constexpr auto kHandshakeTimeout = std::chrono::seconds(10);

		// LAN discovery. A fixed-size datagram, so a malformed or oversized one
		// is rejected on length alone before anything is parsed.
		constexpr char kDiscoveryMagic[4] = { 'H', 'M', 'D', 'D' };
		constexpr char kDiscoveryProbe = '?';
		constexpr char kDiscoveryReply = '!';
		constexpr size_t kDiscoveryMessageSize = 7;
		constexpr int kDiscoveryTimeoutMs = 3000;

		std::atomic<bool> g_Initialized{ false };
		std::atomic<bool> g_WorkerShouldRun{ false };
		std::atomic<LinkState> g_State{ LinkState::Idle };

		std::thread g_Worker;
		std::mutex g_Mutex;

		std::deque<std::string> g_Outbound;
		std::deque<std::string> g_Inbound;
		std::string g_LastError;
		std::string g_PeerAddress;
		Options g_Options;

		// Session request handed from the game thread to the worker.
		struct SessionRequest
		{
			bool pending = false;
			bool host = false;
			bool discover = false;
			std::string address;
			unsigned short port = 0;
		};
		SessionRequest g_Request;
		std::atomic<bool> g_DisconnectRequested{ false };

		std::string SessionKeyCopy()
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			return g_Options.session_key;
		}

		bool DiscoveryEnabled()
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			return g_Options.enable_discovery;
		}

		void SetPeerAddress(const std::string& Address)
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			g_PeerAddress = Address;
		}

		std::string DescribeSocketAddress(const sockaddr_in& Address)
		{
			char text[INET_ADDRSTRLEN]{};
			if (!inet_ntop(AF_INET, &Address.sin_addr, text, sizeof(text)))
				return "unknown";
			return text;
		}

		void SetError(const std::string& Message)
		{
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				g_LastError = Message;
			}
			LogWarn("net: %s", Message.c_str());
		}

		std::string WsaErrorText(int Code)
		{
			char buffer[256]{};
			_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "winsock error %d", Code);
			return buffer;
		}

		void CloseSocketSafe(SOCKET& Handle)
		{
			if (Handle != INVALID_SOCKET)
			{
				closesocket(Handle);
				Handle = INVALID_SOCKET;
			}
		}

		// --- Blocking-with-timeout primitives -----------------------------
		// The worker owns the socket, so blocking here is fine; each call still
		// honours the shutdown flag so Shutdown() cannot hang the game.

		bool SendAll(SOCKET Socket, const char* Data, size_t Length)
		{
			size_t sent = 0;
			while (sent < Length)
			{
				int chunk = send(
					Socket,
					Data + sent,
					static_cast<int>(Length - sent),
					0
				);

				if (chunk == SOCKET_ERROR)
				{
					int error = WSAGetLastError();
					if (error == WSAEWOULDBLOCK)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
						continue;
					}
					SetError("send failed: " + WsaErrorText(error));
					return false;
				}

				if (chunk == 0)
					return false;

				sent += static_cast<size_t>(chunk);
			}
			return true;
		}

		bool SendFrame(SOCKET Socket, const std::string& Payload)
		{
			if (Payload.size() > kMaxPayload)
			{
				SetError("refusing to send an oversized payload");
				return false;
			}

			char header[8];
			memcpy(header, kMagic, 4);
			uint32_t length = htonl(static_cast<uint32_t>(Payload.size()));
			memcpy(header + 4, &length, 4);

			if (!SendAll(Socket, header, sizeof(header)))
				return false;

			return SendAll(Socket, Payload.data(), Payload.size());
		}

		// Reads exactly Length bytes into Out unless the link drops or shutdown
		// is requested. Returns false on any of those.
		bool ReceiveExact(SOCKET Socket, char* Out, size_t Length, bool& Closed)
		{
			size_t received = 0;
			while (received < Length)
			{
				if (!g_WorkerShouldRun.load() || g_DisconnectRequested.load())
					return false;

				int chunk = recv(
					Socket,
					Out + received,
					static_cast<int>(Length - received),
					0
				);

				if (chunk == 0)
				{
					Closed = true;
					return false;
				}

				if (chunk == SOCKET_ERROR)
				{
					int error = WSAGetLastError();
					if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT)
					{
						std::this_thread::sleep_for(
							std::chrono::milliseconds(kPollIntervalMs));
						continue;
					}
					SetError("recv failed: " + WsaErrorText(error));
					return false;
				}

				received += static_cast<size_t>(chunk);
			}
			return true;
		}

		// Attempts to read one complete frame. Returns:
		//   1  - a frame was read into OutPayload
		//   0  - nothing available right now
		//  -1  - the link is finished (closed or errored)
		int TryReceiveFrame(SOCKET Socket, std::string& OutPayload)
		{
			// Peek for a full header first so a quiet link costs nothing.
			char header[8];
			int peeked = recv(Socket, header, sizeof(header), MSG_PEEK);

			if (peeked == 0)
				return -1;

			if (peeked == SOCKET_ERROR)
			{
				int error = WSAGetLastError();
				if (error == WSAEWOULDBLOCK)
					return 0;
				SetError("recv failed: " + WsaErrorText(error));
				return -1;
			}

			if (peeked < static_cast<int>(sizeof(header)))
				return 0;

			if (memcmp(header, kMagic, 4) != 0)
			{
				SetError("peer sent an unrecognised frame - wrong port or version");
				return -1;
			}

			uint32_t length = 0;
			memcpy(&length, header + 4, 4);
			length = ntohl(length);

			if (length > kMaxPayload)
			{
				SetError("peer announced an oversized payload - dropping link");
				return -1;
			}

			// Now consume the header for real, then the body.
			bool closed = false;
			if (!ReceiveExact(Socket, header, sizeof(header), closed))
				return -1;

			std::string payload;
			payload.resize(length);
			if (length > 0 && !ReceiveExact(Socket, payload.data(), length, closed))
				return -1;

			OutPayload = std::move(payload);
			return 1;
		}

		// --- Handshake ----------------------------------------------------
		//
		// Symmetric: both sides send their hello immediately, then wait for the
		// peer's. The messages are small enough to sit in the socket buffer, so
		// neither side can block the other. A mismatch closes the link before
		// any game data moves in either direction.

		std::string BuildHello()
		{
			json::Value hello = json::Value::Object();
			hello.Set("hmd", json::Value(kHandshakeProtocol));
			hello.Set("key", json::Value(SessionKeyCopy()));
			return hello.Serialize();
		}

		bool PerformHandshake(SOCKET Peer)
		{
			g_State.store(LinkState::Handshaking);

			if (!SendFrame(Peer, BuildHello()))
			{
				SetError("could not send the handshake");
				return false;
			}

			const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;

			std::string incoming;
			while (true)
			{
				if (!g_WorkerShouldRun.load() || g_DisconnectRequested.load())
					return false;

				if (std::chrono::steady_clock::now() > deadline)
				{
					SetError("peer did not complete the handshake in time");
					return false;
				}

				int result = TryReceiveFrame(Peer, incoming);
				if (result < 0)
					return false;

				if (result > 0)
					break;

				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
			}

			json::Value hello;
			if (!json::Parse(incoming, hello) || !hello.IsObject())
			{
				SetError("peer's handshake was not a valid message - dropping link");
				return false;
			}

			const int protocol = hello["hmd"].AsInt(0);
			if (protocol != kHandshakeProtocol)
			{
				SetError("peer speaks handshake protocol " +
					std::to_string(protocol) + ", this build speaks " +
					std::to_string(kHandshakeProtocol) + " - dropping link");
				return false;
			}

			// Compared in full; the passphrase never appears in any log line.
			if (hello["key"].AsString() != SessionKeyCopy())
			{
				SetError("peer presented the wrong session key - dropping link");
				return false;
			}

			LogStage(kStageConnect, "handshake accepted");
			return true;
		}

		// --- LAN discovery ------------------------------------------------
		//
		// Exists so that neither player has to know the other's IP address on a
		// local network. The host answers probes while it is listening; the
		// joiner broadcasts one and dials whoever replies.

		void EncodeDiscoveryMessage(char Kind, unsigned short Port, char* Out)
		{
			memcpy(Out, kDiscoveryMagic, 4);
			Out[4] = Kind;
			unsigned short port = htons(Port);
			memcpy(Out + 5, &port, 2);
		}

		bool DecodeDiscoveryMessage(
			const char* Data,
			int Length,
			char& OutKind,
			unsigned short& OutPort
		)
		{
			if (Length != static_cast<int>(kDiscoveryMessageSize))
				return false;

			if (memcmp(Data, kDiscoveryMagic, 4) != 0)
				return false;

			OutKind = Data[4];

			unsigned short port = 0;
			memcpy(&port, Data + 5, 2);
			OutPort = ntohs(port);
			return true;
		}

		SOCKET OpenDiscoveryResponder(unsigned short DiscoveryPort)
		{
			SOCKET responder = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (responder == INVALID_SOCKET)
				return INVALID_SOCKET;

			int reuse = 1;
			setsockopt(responder, SOL_SOCKET, SO_REUSEADDR,
				reinterpret_cast<const char*>(&reuse), sizeof(reuse));

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = INADDR_ANY;
			address.sin_port = htons(DiscoveryPort);

			if (bind(responder, reinterpret_cast<sockaddr*>(&address),
				sizeof(address)) == SOCKET_ERROR)
			{
				// Not fatal: discovery is a convenience, and the session port
				// itself is unaffected. Players can still join by address.
				LogWarn("net: could not bind discovery port %u - joining by "
					"address will still work", DiscoveryPort);
				closesocket(responder);
				return INVALID_SOCKET;
			}

			u_long non_blocking = 1;
			ioctlsocket(responder, FIONBIO, &non_blocking);
			return responder;
		}

		// Answers every probe waiting on the socket. Returns immediately when
		// there are none, so this is safe to call from the accept poll loop.
		void ServiceDiscoveryResponder(SOCKET Responder, unsigned short SessionPort)
		{
			if (Responder == INVALID_SOCKET)
				return;

			for (;;)
			{
				char buffer[64]{};
				sockaddr_in from{};
				int from_length = sizeof(from);

				int received = recvfrom(
					Responder,
					buffer,
					sizeof(buffer),
					0,
					reinterpret_cast<sockaddr*>(&from),
					&from_length
				);

				if (received <= 0)
					return;

				char kind = 0;
				unsigned short port = 0;
				if (!DecodeDiscoveryMessage(buffer, received, kind, port))
					continue;

				if (kind != kDiscoveryProbe)
					continue;

				char reply[kDiscoveryMessageSize]{};
				EncodeDiscoveryMessage(kDiscoveryReply, SessionPort, reply);

				sendto(
					Responder,
					reply,
					static_cast<int>(sizeof(reply)),
					0,
					reinterpret_cast<sockaddr*>(&from),
					from_length
				);

				LogInfo("net: answered a discovery probe from %s",
					DescribeSocketAddress(from).c_str());
			}
		}

		// Broadcasts a probe and waits for a host to answer. Returns the
		// responder's address, which the caller then dials normally.
		bool DiscoverHost(unsigned short SessionPort, std::string& OutAddress)
		{
			const unsigned short discovery_port = DiscoveryPortFor(SessionPort);

			SOCKET prober = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (prober == INVALID_SOCKET)
			{
				SetError("could not create a discovery socket");
				return false;
			}

			int broadcast = 1;
			setsockopt(prober, SOL_SOCKET, SO_BROADCAST,
				reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

			u_long non_blocking = 1;
			ioctlsocket(prober, FIONBIO, &non_blocking);

			char probe[kDiscoveryMessageSize]{};
			EncodeDiscoveryMessage(kDiscoveryProbe, SessionPort, probe);

			// Broadcast for the LAN case, plus loopback so two clients on one
			// machine find each other (broadcast does not reliably loop back).
			const char* targets[] = { "255.255.255.255", "127.0.0.1" };
			for (const char* target : targets)
			{
				sockaddr_in destination{};
				destination.sin_family = AF_INET;
				destination.sin_port = htons(discovery_port);
				inet_pton(AF_INET, target, &destination.sin_addr);

				sendto(
					prober,
					probe,
					static_cast<int>(sizeof(probe)),
					0,
					reinterpret_cast<sockaddr*>(&destination),
					sizeof(destination)
				);
			}

			LogStage(kStageConnect,
				"looking for a host on the local network (port %u)", discovery_port);

			const auto deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(kDiscoveryTimeoutMs);

			while (std::chrono::steady_clock::now() < deadline)
			{
				if (!g_WorkerShouldRun.load() || g_DisconnectRequested.load())
					break;

				char buffer[64]{};
				sockaddr_in from{};
				int from_length = sizeof(from);

				int received = recvfrom(
					prober,
					buffer,
					sizeof(buffer),
					0,
					reinterpret_cast<sockaddr*>(&from),
					&from_length
				);

				if (received > 0)
				{
					char kind = 0;
					unsigned short port = 0;
					if (DecodeDiscoveryMessage(buffer, received, kind, port) &&
						kind == kDiscoveryReply)
					{
						OutAddress = DescribeSocketAddress(from);
						closesocket(prober);
						LogStage(kStageConnect, "found a host at %s",
							OutAddress.c_str());
						return true;
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
			}

			closesocket(prober);
			SetError("no host answered on the local network - start the host "
				"first, or set peer_address to the host's IP");
			return false;
		}

		void ConfigureSocket(SOCKET Socket)
		{
			// Non-blocking, so a quiet peer never pins the worker inside recv.
			u_long non_blocking = 1;
			ioctlsocket(Socket, FIONBIO, &non_blocking);

			// Payloads are small and latency-sensitive at the act boundary.
			int no_delay = 1;
			setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY,
				reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
		}

		SOCKET AcceptPeer(unsigned short Port)
		{
			SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET)
			{
				SetError("could not create a listening socket: " +
					WsaErrorText(WSAGetLastError()));
				return INVALID_SOCKET;
			}

			int reuse = 1;
			setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
				reinterpret_cast<const char*>(&reuse), sizeof(reuse));

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = INADDR_ANY;
			address.sin_port = htons(Port);

			if (bind(listener, reinterpret_cast<sockaddr*>(&address),
				sizeof(address)) == SOCKET_ERROR)
			{
				SetError("could not bind port " + std::to_string(Port) + ": " +
					WsaErrorText(WSAGetLastError()));
				CloseSocketSafe(listener);
				return INVALID_SOCKET;
			}

			if (listen(listener, 1) == SOCKET_ERROR)
			{
				SetError("listen failed: " + WsaErrorText(WSAGetLastError()));
				CloseSocketSafe(listener);
				return INVALID_SOCKET;
			}

			ConfigureSocket(listener);
			g_State.store(LinkState::Listening);
			LogStage(kStageConnect, "hosting on port %u - waiting for a peer", Port);

			// The listener binds every interface, so on a forwarded port this is
			// reachable from anywhere. Say so once, plainly, when there is no
			// passphrase to distinguish an invited peer from any other.
			if (SessionKeyCopy().empty())
			{
				LogWarn("no session_key is set - any peer that can reach port %u "
					"may join. Fine on a trusted LAN; set session_key in the ini "
					"before forwarding this port to the internet.", Port);
			}

			SOCKET responder = DiscoveryEnabled()
				? OpenDiscoveryResponder(DiscoveryPortFor(Port))
				: INVALID_SOCKET;

			SOCKET peer = INVALID_SOCKET;
			sockaddr_in peer_address{};
			int peer_address_length = sizeof(peer_address);

			while (g_WorkerShouldRun.load() && !g_DisconnectRequested.load())
			{
				peer = accept(
					listener,
					reinterpret_cast<sockaddr*>(&peer_address),
					&peer_address_length
				);

				if (peer != INVALID_SOCKET)
					break;

				int error = WSAGetLastError();
				if (error != WSAEWOULDBLOCK)
				{
					SetError("accept failed: " + WsaErrorText(error));
					break;
				}

				ServiceDiscoveryResponder(responder, Port);
				std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
			}

			CloseSocketSafe(listener);
			CloseSocketSafe(responder);

			if (peer != INVALID_SOCKET)
			{
				ConfigureSocket(peer);

				const std::string address = DescribeSocketAddress(peer_address);
				SetPeerAddress(address);
				LogStage(kStageConnect, "peer connected from %s", address.c_str());
			}

			return peer;
		}

		SOCKET DialPeer(const std::string& Address, unsigned short Port)
		{
			g_State.store(LinkState::Connecting);
			LogStage(kStageConnect, "connecting to %s:%u", Address.c_str(), Port);

			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;

			addrinfo* resolved = nullptr;
			std::string port_text = std::to_string(Port);

			int status = getaddrinfo(Address.c_str(), port_text.c_str(), &hints, &resolved);
			if (status != 0 || !resolved)
			{
				SetError("could not resolve '" + Address + "'");
				return INVALID_SOCKET;
			}

			SOCKET peer = socket(
				resolved->ai_family,
				resolved->ai_socktype,
				resolved->ai_protocol
			);

			if (peer == INVALID_SOCKET)
			{
				SetError("could not create a socket: " + WsaErrorText(WSAGetLastError()));
				freeaddrinfo(resolved);
				return INVALID_SOCKET;
			}

			// Connect while still blocking so the outcome is unambiguous, then
			// switch to non-blocking for the session itself.
			bool connected = connect(
				peer,
				resolved->ai_addr,
				static_cast<int>(resolved->ai_addrlen)
			) != SOCKET_ERROR;

			int connect_error = connected ? 0 : WSAGetLastError();
			freeaddrinfo(resolved);

			if (!connected)
			{
				SetError("connect to " + Address + ":" + port_text + " failed: " +
					WsaErrorText(connect_error));
				CloseSocketSafe(peer);
				return INVALID_SOCKET;
			}

			ConfigureSocket(peer);
			SetPeerAddress(Address);
			LogStage(kStageConnect, "connected to %s:%u", Address.c_str(), Port);
			return peer;
		}

		// Drives one session from connect through to disconnect.
		void RunSession(SOCKET Peer)
		{
			g_State.store(LinkState::Connected);

			// Clear any payloads left over from a previous session so the two
			// sides cannot desynchronise on stale data.
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				g_Outbound.clear();
				g_Inbound.clear();
			}

			while (g_WorkerShouldRun.load() && !g_DisconnectRequested.load())
			{
				// Drain the outbound queue.
				std::string outgoing;
				bool has_outgoing = false;
				{
					std::lock_guard<std::mutex> lock(g_Mutex);
					if (!g_Outbound.empty())
					{
						outgoing = std::move(g_Outbound.front());
						g_Outbound.pop_front();
						has_outgoing = true;
					}
				}

				if (has_outgoing && !SendFrame(Peer, outgoing))
					break;

				// Drain whatever the peer has sent.
				std::string incoming;
				int result = TryReceiveFrame(Peer, incoming);

				if (result < 0)
					break;

				if (result > 0)
				{
					std::lock_guard<std::mutex> lock(g_Mutex);
					g_Inbound.push_back(std::move(incoming));
					continue; // Try for another frame straight away.
				}

				if (!has_outgoing)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(kPollIntervalMs));
			}

			CloseSocketSafe(Peer);
			LogStage(kStageConnect, "session ended");
		}

		void WorkerMain()
		{
			while (g_WorkerShouldRun.load())
			{
				SessionRequest request;
				{
					std::lock_guard<std::mutex> lock(g_Mutex);
					if (g_Request.pending)
					{
						request = g_Request;
						g_Request.pending = false;
					}
				}

				if (!request.pending)
				{
					std::this_thread::sleep_for(
						std::chrono::milliseconds(kPollIntervalMs));
					continue;
				}

				g_DisconnectRequested.store(false);
				SetPeerAddress({});

				// A joiner with no configured address finds the host first.
				std::string address = request.address;
				if (!request.host && request.discover)
				{
					g_State.store(LinkState::Discovering);
					if (!DiscoverHost(request.port, address))
					{
						g_State.store(LinkState::Failed);
						continue;
					}
				}

				SOCKET peer = request.host
					? AcceptPeer(request.port)
					: DialPeer(address, request.port);

				if (peer == INVALID_SOCKET)
				{
					g_State.store(LinkState::Failed);
					continue;
				}

				// Nothing but the handshake may cross this socket until the peer
				// has proved it is this mod, speaking this protocol, holding the
				// configured passphrase.
				if (!PerformHandshake(peer))
				{
					CloseSocketSafe(peer);
					SetPeerAddress({});
					g_State.store(
						g_DisconnectRequested.load() ? LinkState::Idle : LinkState::Failed
					);
					continue;
				}

				RunSession(peer);

				// A clean teardown returns to Idle; anything else leaves the
				// failure visible to the player.
				g_State.store(
					g_DisconnectRequested.load() ? LinkState::Idle : LinkState::Failed
				);
			}
		}

		bool QueueSession(
			bool AsHost,
			const std::string& Address,
			unsigned short Port,
			bool Discover = false
		)
		{
			if (!g_Initialized.load())
			{
				SetError("networking is not initialised");
				return false;
			}

			// Drop whatever is running before starting the new session.
			g_DisconnectRequested.store(true);

			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Request.pending = true;
			g_Request.host = AsHost;
			g_Request.discover = Discover;
			g_Request.address = Address;
			g_Request.port = Port;
			g_LastError.clear();
			g_PeerAddress.clear();
			return true;
		}
	}

	unsigned short DiscoveryPortFor(unsigned short SessionPort)
	{
		// One above the session port, wrapping rather than overflowing so the
		// top of the range stays usable.
		return SessionPort == 65535
			? static_cast<unsigned short>(65534)
			: static_cast<unsigned short>(SessionPort + 1);
	}

	void Configure(const Options& NewOptions)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Options = NewOptions;
	}

	bool Initialize()
	{
		if (g_Initialized.load())
			return true;

		WSADATA winsock_data{};
		int status = WSAStartup(MAKEWORD(2, 2), &winsock_data);
		if (status != 0)
		{
			LogError("WSAStartup failed (%d) - multiplayer disabled", status);
			return false;
		}

		g_Initialized.store(true);
		g_WorkerShouldRun.store(true);
		g_State.store(LinkState::Idle);

		g_Worker = std::thread(WorkerMain);
		LogInfo("net: worker started");
		return true;
	}

	void Shutdown()
	{
		if (!g_Initialized.load())
			return;

		g_DisconnectRequested.store(true);
		g_WorkerShouldRun.store(false);

		if (g_Worker.joinable())
			g_Worker.join();

		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Outbound.clear();
			g_Inbound.clear();
			g_Request.pending = false;
			g_PeerAddress.clear();
		}

		WSACleanup();
		g_Initialized.store(false);
		g_State.store(LinkState::Idle);
		LogInfo("net: worker stopped");
	}

	bool Host(unsigned short Port)
	{
		return QueueSession(true, {}, Port);
	}

	bool Join(const std::string& Address, unsigned short Port)
	{
		return QueueSession(false, Address, Port);
	}

	bool JoinDiscovered(unsigned short Port)
	{
		return QueueSession(false, {}, Port, true);
	}

	std::string PeerAddress()
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_PeerAddress;
	}

	void Disconnect()
	{
		g_DisconnectRequested.store(true);
	}

	LinkState State()
	{
		return g_State.load();
	}

	// A Steam session and a direct TCP session are mutually exclusive in
	// practice - opening one leaves the other idle - so the accessors below
	// simply prefer Steam whenever it holds a live session. Match.cpp talks to
	// this surface and never needs to know which transport carried a payload.
	const char* StateName()
	{
		if (steam::IsConnected())
			return "steam";

		switch (g_State.load())
		{
		case LinkState::Idle:        return "idle";
		case LinkState::Listening:   return "listening";
		case LinkState::Discovering: return "discovering";
		case LinkState::Connecting:  return "connecting";
		case LinkState::Handshaking: return "handshaking";
		case LinkState::Connected:   return "connected";
		case LinkState::Failed:      return "failed";
		}
		return "unknown";
	}

	std::string LastError()
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_LastError;
	}

	bool IsConnected()
	{
		return steam::IsConnected() || g_State.load() == LinkState::Connected;
	}

	bool Send(const std::string& Payload)
	{
		if (steam::IsConnected())
			return steam::Send(Payload);

		if (g_State.load() != LinkState::Connected)
			return false;

		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Outbound.push_back(Payload);
		return true;
	}

	bool Poll(std::string& OutPayload)
	{
		// Steam first, so a Steam session drains even if a stale TCP queue
		// still holds something from an earlier direct session.
		if (steam::Poll(OutPayload))
			return true;

		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_Inbound.empty())
			return false;

		OutPayload = std::move(g_Inbound.front());
		g_Inbound.pop_front();
		return true;
	}
}
