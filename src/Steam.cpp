// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Steam.h"
#include "Log.h"

#include <windows.h>

#include <cstddef>
#include <deque>
#include <mutex>

namespace hmd::steam
{
	namespace
	{
		// -----------------------------------------------------------------
		// Minimal Steamworks ABI
		// -----------------------------------------------------------------
		//
		// Only the pieces this mod touches, transcribed from the public flat
		// API. These are part of Steam's stable ABI; the static_asserts below
		// are the guard rail - if a layout assumption is ever wrong the build
		// fails rather than the game crashing at runtime.

		using SteamAPICall_t = uint64_t;
		using CSteamID = uint64_t;

		constexpr SteamAPICall_t kInvalidAPICall = 0;

		// Callback identifiers. k_iSteamMatchmakingCallbacks = 500,
		// k_iSteamFriendsCallbacks = 300.
		constexpr int kLobbyCreated = 500 + 13;
		constexpr int kLobbyEnter = 500 + 4;
		constexpr int kGameLobbyJoinRequested = 300 + 33;

		// k_iSteamNetworkingMessagesCallbacks = 1250.
		// SteamNetworkingMessagesSessionRequest_t is the first of them,
		// SteamNetworkingMessagesSessionFailed_t the second.
		constexpr int kMessagesSessionRequest = 1250 + 1;
		constexpr int kMessagesSessionFailed = 1250 + 2;

		// ELobbyType::k_ELobbyTypeFriendsOnly
		constexpr int kLobbyTypeFriendsOnly = 1;

		// ESteamNetworkingIdentityType::k_ESteamNetworkingIdentityType_SteamID
		constexpr int kIdentityTypeSteamID = 16;

		// k_nSteamNetworkingSend_Reliable
		constexpr int kSendReliable = 8;

		// The channel this mod talks on. Arbitrary, but must match on both
		// sides and must not collide with anything the game itself uses.
		constexpr int kChannel = 0x484D;

		// Steam's callback structs are declared under #pragma pack(push, 8) in
		// the SDK headers (VALVE_CALLBACK_PACK_LARGE on x64), NOT pack(1).
		//
		// This was pack(1), and it mattered: LobbyCreated_t is
		// { EResult (4 bytes); uint64 lobby id; }, so under the real ABI the
		// lobby id sits at offset 8 and the struct is 16 bytes. Packed to 1 it
		// was 12 bytes with the id at offset 4 - so PollPendingLobbyCreate read
		// the lobby id out of four bytes of padding and four bytes of the real
		// id, and assigned that to g_Lobby. From that moment the host was
		// holding a lobby handle that does not exist: GetNumLobbyMembers on it
		// returns 0, so the host never adopted the peer, never reached
		// LobbyState::Joined, and therefore never sent a single byte.
		//
		// It failed silently and asymmetrically, because draining incoming
		// messages does not depend on our own state - so the host still received
		// the guest's beacons and could name them, while the guest heard nothing
		// and reported "opponent ?" forever.
		#pragma pack(push, 8)
		struct SteamNetworkingIdentity
		{
			int32_t m_eType;
			int32_t m_cbSize;
			union
			{
				uint64_t m_steamID64;
				uint8_t m_genericBytes[32];
				uint8_t m_reserved[128];
			};
		};

		struct LobbyCreated_t
		{
			int32_t m_eResult;
			uint64_t m_ulSteamIDLobby;
		};

		struct LobbyEnter_t
		{
			uint64_t m_ulSteamIDLobby;
			uint32_t m_rgfChatPermissions;
			bool m_bLocked;
			uint32_t m_EChatRoomEnterResponse;
		};

		struct GameLobbyJoinRequested_t
		{
			uint64_t m_steamIDLobby;
			uint64_t m_steamIDFriend;
		};

		// Raised on the RECEIVING side the first time a peer tries to send to
		// us. Until it is answered with AcceptSessionWithUser, the sender's
		// messages fail - which is the entire reason this struct exists here.
		struct MessagesSessionRequest_t
		{
			SteamNetworkingIdentity m_identityRemote;
		};
		#pragma pack(pop)

		static_assert(sizeof(SteamNetworkingIdentity) == 136,
			"SteamNetworkingIdentity layout does not match the Steamworks ABI");
		static_assert(offsetof(SteamNetworkingIdentity, m_steamID64) == 8,
			"SteamNetworkingIdentity::m_steamID64 moved");

		// The one that was wrong. Assert the offset, not just the size: a size
		// that happens to match with the field in the wrong place is exactly the
		// failure this is here to catch.
		static_assert(offsetof(LobbyCreated_t, m_ulSteamIDLobby) == 8,
			"LobbyCreated_t::m_ulSteamIDLobby moved - the lobby id would be read "
			"out of padding and the host would hold a lobby that does not exist");
		static_assert(sizeof(LobbyCreated_t) == 16, "LobbyCreated_t layout changed");

		static_assert(offsetof(LobbyEnter_t, m_ulSteamIDLobby) == 0,
			"LobbyEnter_t::m_ulSteamIDLobby moved");
		static_assert(sizeof(LobbyEnter_t) == 24, "LobbyEnter_t layout changed");

		static_assert(sizeof(GameLobbyJoinRequested_t) == 16,
			"GameLobbyJoinRequested_t layout changed");
		static_assert(sizeof(MessagesSessionRequest_t) == 136,
			"MessagesSessionRequest_t layout does not match the Steamworks ABI");

		// SteamNetworkingMessage_t, as received from ReceiveMessagesOnChannel.
		// Only the leading fields are read; m_pfnRelease is called to free it.
		//
		// Every field here has to be exactly right, not just the ones that get
		// read. m_conn was declared uint64_t in the first version of this file;
		// the real HSteamNetConnection is uint32, and getting it wrong pushed
		// everything after it eight bytes along - so m_pfnRelease was read out
		// of m_nChannel and m_nFlags and called as a function pointer. The game
		// died on the first message it ever successfully received.
		//
		// The offsets are asserted below rather than trusted, because this
		// struct is only exercised once a real peer sends something, which is
		// the worst possible place for a silent layout error to be waiting.
		struct SteamNetworkingMessage_t
		{
			void* m_pData;
			int32_t m_cbSize;
			uint32_t m_conn;         // HSteamNetConnection - uint32, NOT uint64
			SteamNetworkingIdentity m_identityPeer;
			int64_t m_nConnUserData;
			int64_t m_usecTimeReceived;
			int64_t m_nMessageNumber;
			void (*m_pfnFreeData)(SteamNetworkingMessage_t*);
			void (*m_pfnRelease)(SteamNetworkingMessage_t*);
			int32_t m_nChannel;
			int32_t m_nFlags;
			int64_t m_nUserData;
			uint16_t m_idxLane;
			uint16_t m__pad1;
		};

		static_assert(offsetof(SteamNetworkingMessage_t, m_pData) == 0,
			"SteamNetworkingMessage_t::m_pData moved");
		static_assert(offsetof(SteamNetworkingMessage_t, m_cbSize) == 8,
			"SteamNetworkingMessage_t::m_cbSize moved");
		static_assert(offsetof(SteamNetworkingMessage_t, m_identityPeer) == 16,
			"SteamNetworkingMessage_t::m_identityPeer moved - check m_conn's width");
		static_assert(offsetof(SteamNetworkingMessage_t, m_pfnRelease) == 184,
			"SteamNetworkingMessage_t::m_pfnRelease moved - calling this at the "
			"wrong offset jumps into whatever follows it");
		static_assert(sizeof(SteamNetworkingMessage_t) == 216,
			"SteamNetworkingMessage_t layout does not match the Steamworks ABI");

		// Steam's C++ callback base. SteamAPI_RegisterCallback expects an
		// object with this exact shape: three virtuals, then the two data
		// members. Reproduced rather than vendored, for the same reason as
		// everything else here.
		class CallbackBase
		{
		public:
			virtual void Run(void* Parameter) = 0;
			virtual void Run(void* Parameter, bool IoFailure, SteamAPICall_t Call) = 0;
			virtual int GetCallbackSizeBytes() = 0;

			uint8_t m_nCallbackFlags = 0;
			int m_iCallback = 0;
		};

		// -----------------------------------------------------------------
		// Dynamic binding
		// -----------------------------------------------------------------

		struct SteamApi
		{
			// Interface accessors
			void* (*SteamUser)() = nullptr;
			void* (*SteamFriends)() = nullptr;
			void* (*SteamMatchmaking)() = nullptr;
			void* (*SteamUtils)() = nullptr;
			void* (*SteamNetworkingMessages)() = nullptr;

			// User / friends
			CSteamID(*GetSteamID)(void*) = nullptr;
			const char* (*GetPersonaName)(void*) = nullptr;
			const char* (*GetFriendPersonaName)(void*, CSteamID) = nullptr;
			bool (*ActivateInviteOverlay)(void*, CSteamID) = nullptr;

			// Matchmaking
			SteamAPICall_t(*CreateLobby)(void*, int, int) = nullptr;
			SteamAPICall_t(*JoinLobby)(void*, CSteamID) = nullptr;
			void (*LeaveLobby)(void*, CSteamID) = nullptr;
			bool (*SetLobbyData)(void*, CSteamID, const char*, const char*) = nullptr;
			const char* (*GetLobbyData)(void*, CSteamID, const char*) = nullptr;
			int (*GetNumLobbyMembers)(void*, CSteamID) = nullptr;
			CSteamID(*GetLobbyMemberByIndex)(void*, CSteamID, int) = nullptr;
			CSteamID(*GetLobbyOwner)(void*, CSteamID) = nullptr;

			// Utils - lets async calls be polled instead of needing CallResults
			bool (*IsAPICallCompleted)(void*, SteamAPICall_t, bool*) = nullptr;
			bool (*GetAPICallResult)(void*, SteamAPICall_t, void*, int, int, bool*) = nullptr;

			// Networking messages
			int (*SendMessageToUser)(void*, const SteamNetworkingIdentity*,
				const void*, uint32_t, int, int) = nullptr;
			int (*ReceiveMessagesOnChannel)(void*, int,
				SteamNetworkingMessage_t**, int) = nullptr;
			bool (*AcceptSessionWithUser)(void*, const SteamNetworkingIdentity*) = nullptr;

			// Callbacks
			void (*RegisterCallback)(CallbackBase*, int) = nullptr;
			void (*UnregisterCallback)(CallbackBase*) = nullptr;
		};

		SteamApi g_Api;
		bool g_Available = false;

		void* g_User = nullptr;
		void* g_Friends = nullptr;
		void* g_Matchmaking = nullptr;
		void* g_Utils = nullptr;
		void* g_Messages = nullptr;

		std::mutex g_Mutex;
		std::deque<std::string> g_Inbound;

		LobbyState g_State = LobbyState::None;
		CSteamID g_Lobby = 0;
		CSteamID g_Peer = 0;
		SteamAPICall_t g_PendingCreate = kInvalidAPICall;
		std::string g_PeerName;

		// Lobby data key carrying the protocol version, so an incompatible
		// build is rejected before any payload is exchanged.
		constexpr const char* kLobbyKeyProtocol = "hmd_proto";
		constexpr const char* kLobbyProtocolValue = "1";

		template <typename T>
		bool Bind(HMODULE Module, const char* Name, T& Target)
		{
			Target = reinterpret_cast<T>(
				reinterpret_cast<void*>(GetProcAddress(Module, Name)));

			if (!Target)
				LogWarn("steam: export '%s' is missing - Steam features disabled", Name);

			return Target != nullptr;
		}

		SteamNetworkingIdentity MakeIdentity(CSteamID User)
		{
			SteamNetworkingIdentity identity{};
			identity.m_eType = kIdentityTypeSteamID;
			identity.m_cbSize = sizeof(uint64_t);
			identity.m_steamID64 = User;
			return identity;
		}

		void SetState(LobbyState NewState, const char* Reason)
		{
			g_State = NewState;
			LogStage(kStageConnect, "steam: %s (%s)", StateName(), Reason ? Reason : "");
		}

		// Once both sides are in the lobby, each proactively accepts a session
		// with the other. Doing it from both ends means neither needs to handle
		// an incoming session-request callback.
		void AdoptPeerFromLobby()
		{
			if (!g_Lobby || !g_Api.GetNumLobbyMembers)
				return;

			const CSteamID self = g_Api.GetSteamID(g_User);
			const int members = g_Api.GetNumLobbyMembers(g_Matchmaking, g_Lobby);

			for (int i = 0; i < members; i++)
			{
				CSteamID member = g_Api.GetLobbyMemberByIndex(g_Matchmaking, g_Lobby, i);
				if (member == self || member == 0)
					continue;

				g_Peer = member;

				// g_PeerName was declared, cleared in four places, and never
				// once assigned - so steam::PeerName() always returned an empty
				// string and every "who am I playing?" fallback in the mod fell
				// through to "your opponent". The lobby knows who they are
				// before a single message has crossed, so read it here.
				if (g_Api.GetFriendPersonaName)
				{
					const char* name = g_Api.GetFriendPersonaName(g_Friends, g_Peer);
					if (name && *name)
						g_PeerName = name;
				}

				SteamNetworkingIdentity identity = MakeIdentity(g_Peer);
				if (g_Api.AcceptSessionWithUser)
					g_Api.AcceptSessionWithUser(g_Messages, &identity);

				SetState(LobbyState::Joined, "peer present in lobby");
				LogStage(kStageConnect, "steam: opponent is '%s'",
					g_PeerName.empty() ? "?" : g_PeerName.c_str());
				return;
			}
		}

		void DrainIncoming()
		{
			if (!g_Api.ReceiveMessagesOnChannel || !g_Messages)
				return;

			SteamNetworkingMessage_t* messages[16]{};
			int received = g_Api.ReceiveMessagesOnChannel(
				g_Messages, kChannel, messages, 16);

			for (int i = 0; i < received; i++)
			{
				SteamNetworkingMessage_t* message = messages[i];
				if (!message)
					continue;

				if (message->m_pData && message->m_cbSize > 0)
				{
					std::lock_guard<std::mutex> lock(g_Mutex);
					g_Inbound.emplace_back(
						static_cast<const char*>(message->m_pData),
						static_cast<size_t>(message->m_cbSize)
					);
				}

				// Steam hands ownership to us; releasing is mandatory.
				if (message->m_pfnRelease)
					message->m_pfnRelease(message);
			}
		}

		void PollPendingLobbyCreate()
		{
			if (g_PendingCreate == kInvalidAPICall || !g_Api.IsAPICallCompleted)
				return;

			bool failed = false;
			if (!g_Api.IsAPICallCompleted(g_Utils, g_PendingCreate, &failed))
				return;

			LobbyCreated_t result{};
			bool io_failure = false;
			const bool ok = g_Api.GetAPICallResult(
				g_Utils, g_PendingCreate, &result,
				sizeof(result), kLobbyCreated, &io_failure);

			g_PendingCreate = kInvalidAPICall;

			// EResult::k_EResultOK == 1
			if (!ok || io_failure || result.m_eResult != 1)
			{
				// If LobbyEnter already handed us a working lobby, believe it
				// over this. Reporting a failure and tearing the state down
				// would throw away a session that is demonstrably up.
				if (g_Lobby && g_State == LobbyState::Hosting)
				{
					LogWarn("steam: could not read the lobby-creation result, but "
						"the lobby is already open - continuing");
					return;
				}

				LogError("steam: lobby creation failed (result %d)", result.m_eResult);
				SetState(LobbyState::None, "lobby creation failed");
				return;
			}

			// The LobbyEnter callback normally gets here first and has already
			// stamped the protocol tag and moved us to Hosting.
			//
			// Never overwrite a lobby id that callback established. It is the
			// authoritative one - it is the id Steam handed us on entering the
			// lobby we are actually sitting in - and this used to clobber it
			// with a value read at the wrong offset, after which the host owned
			// a handle to nothing. A disagreement between the two is worth
			// saying out loud rather than silently picking one.
			if (g_Lobby && g_Lobby != result.m_ulSteamIDLobby)
			{
				LogWarn("steam: lobby id from the creation result (%llu) does not "
					"match the one we entered (%llu) - keeping the latter",
					static_cast<unsigned long long>(result.m_ulSteamIDLobby),
					static_cast<unsigned long long>(g_Lobby));
			}
			else if (!g_Lobby)
			{
				g_Lobby = result.m_ulSteamIDLobby;
			}

			if (g_State != LobbyState::Hosting)
			{
				g_Api.SetLobbyData(g_Matchmaking, g_Lobby,
					kLobbyKeyProtocol, kLobbyProtocolValue);
				SetState(LobbyState::Hosting, "lobby ready");
				LogStage(kStageConnect,
					"steam: lobby open - press F7 again to invite a friend");
			}
		}

		// Accepting an invite while the game is CLOSED does not raise
		// GameLobbyJoinRequested. Steam launches the game with
		// "+connect_lobby <id>" on the command line instead and expects the
		// game to act on it - so a mod that only listens for the callback sees
		// nothing at all, and the guest never enters the lobby. This closes
		// that hole.
		//
		// Runs once. The command line does not change, and re-joining a lobby
		// we have already left would fight the player's own F11.
		void JoinLobbyFromCommandLine()
		{
			static bool considered = false;
			if (considered)
				return;
			considered = true;

			const char* command_line = GetCommandLineA();
			if (!command_line)
				return;

			const char* found = strstr(command_line, "+connect_lobby");
			if (!found)
				return;

			found += strlen("+connect_lobby");
			while (*found == ' ' || *found == '\t')
				found++;

			const CSteamID lobby = _strtoui64(found, nullptr, 10);
			if (lobby == 0)
			{
				LogWarn("steam: launched with +connect_lobby but the id could "
					"not be read - press F7 to connect instead");
				return;
			}

			LogStage(kStageConnect,
				"steam: launched from an invite - joining the lobby");

			g_Lobby = lobby;
			g_Api.JoinLobby(g_Matchmaking, g_Lobby);
			SetState(LobbyState::Joining, "invite accepted at launch");
		}

		// --- Callback shims ----------------------------------------------

		class JoinRequestedCallback : public CallbackBase
		{
		public:
			void Run(void* Parameter) override
			{
				auto* request = static_cast<GameLobbyJoinRequested_t*>(Parameter);
				if (!request)
					return;

				LogStage(kStageConnect, "steam: invite accepted - joining lobby");

				// Leave our own lobby first if we have one. Both players
				// pressing F7 leaves each of them hosting, and accepting an
				// invite while still in your own lobby leaves the client in two
				// lobbies at once - after which the state machine flips between
				// "joined" and "own lobby ready" and neither player can tell
				// which session is real. Seen in the wild; this is the fix.
				if (g_Lobby && g_Api.LeaveLobby)
				{
					LogInfo("steam: leaving your own lobby to join theirs");
					g_Api.LeaveLobby(g_Matchmaking, g_Lobby);
					g_Peer = 0;
					g_PeerName.clear();
				}

				g_Lobby = request->m_steamIDLobby;
				g_Api.JoinLobby(g_Matchmaking, g_Lobby);
				SetState(LobbyState::Joining, "invite accepted");
			}

			void Run(void* Parameter, bool, SteamAPICall_t) override { Run(Parameter); }
			int GetCallbackSizeBytes() override { return sizeof(GameLobbyJoinRequested_t); }
		};

		class LobbyEnterCallback : public CallbackBase
		{
		public:
			void Run(void* Parameter) override
			{
				auto* entered = static_cast<LobbyEnter_t*>(Parameter);
				if (!entered)
					return;

				g_Lobby = entered->m_ulSteamIDLobby;

				// Steam fires this for the creator too, the moment their own
				// lobby exists. The host must not version-check itself: the
				// protocol tag is written here, so at this point it is not set
				// yet and the check would always fail.
				const CSteamID self = g_Api.GetSteamID(g_User);
				const CSteamID owner = g_Api.GetLobbyOwner(g_Matchmaking, g_Lobby);

				if (owner == self)
				{
					g_Api.SetLobbyData(g_Matchmaking, g_Lobby,
						kLobbyKeyProtocol, kLobbyProtocolValue);
					SetState(LobbyState::Hosting, "own lobby ready");
					LogStage(kStageConnect,
						"steam: lobby open - press F7 again to invite a friend");
					return;
				}

				// Guest: the host's tag must match before anything is exchanged.
				const char* protocol = g_Api.GetLobbyData(
					g_Matchmaking, g_Lobby, kLobbyKeyProtocol);

				// An ABSENT tag is not a mismatch. Lobby metadata replicates
				// separately from lobby membership, so a guest can legitimately
				// enter before the host's tag has reached them. Treating that as
				// a version mismatch - which this used to do - drops the guest
				// straight back out on a race, and the host just sees a lobby
				// that never gains a second member.
				if (protocol && strcmp(protocol, kLobbyProtocolValue) != 0)
				{
					LogError("steam: the host is running a different version of "
						"this mod (theirs '%s', yours '%s') - leaving",
						protocol, kLobbyProtocolValue);
					LeaveSession();
					return;
				}

				if (!protocol)
				{
					LogWarn("steam: the host's version tag has not replicated "
						"yet - continuing anyway");
				}

				LogStage(kStageConnect, "steam: entered the host's lobby");
				AdoptPeerFromLobby();
			}

			void Run(void* Parameter, bool, SteamAPICall_t) override { Run(Parameter); }
			int GetCallbackSizeBytes() override { return sizeof(LobbyEnter_t); }
		};

		// Answering this is what makes peer-to-peer messaging work at all.
		//
		// AdoptPeerFromLobby calls AcceptSessionWithUser proactively on both
		// sides, on the theory that if each end accepts the other up front,
		// neither needs to handle an incoming request. That theory is wrong:
		// Steam only honours AcceptSessionWithUser in response to a session
		// request that actually exists. Called before the peer has sent
		// anything, it does nothing.
		//
		// The result was that neither side ever accepted, so every single send
		// returned k_EResultConnectFailed (35) forever, on both machines, while
		// both clients cheerfully reported themselves connected because the
		// lobby was fine. The lobby was never the problem.
		class SessionRequestCallback : public CallbackBase
		{
		public:
			void Run(void* Parameter) override
			{
				auto* request = static_cast<MessagesSessionRequest_t*>(Parameter);
				if (!request || !g_Api.AcceptSessionWithUser || !g_Messages)
					return;

				const bool accepted = g_Api.AcceptSessionWithUser(
					g_Messages, &request->m_identityRemote);

				LogStage(kStageConnect,
					"steam: peer opened a message session - %s",
					accepted ? "accepted" : "REFUSED");
			}

			void Run(void* Parameter, bool, SteamAPICall_t) override { Run(Parameter); }
			int GetCallbackSizeBytes() override { return sizeof(MessagesSessionRequest_t); }
		};

		// The other half of the session story. A relay session that cannot be
		// established fails here, and without this the only symptom is silence -
		// which is indistinguishable from an opponent who simply has not sent
		// anything yet, and which cost a whole playtest to tell apart.
		//
		// The payload is SteamNetConnectionInfo_t, which is large and has grown
		// between SDK versions, so it is deliberately never dereferenced: the
		// fact that a session failed is the useful part.
		class SessionFailedCallback : public CallbackBase
		{
		public:
			void Run(void*) override
			{
				LogError("steam: the peer-to-peer session with your opponent "
					"failed - press F11 and reconnect");
			}

			void Run(void* Parameter, bool, SteamAPICall_t) override { Run(Parameter); }

			// Never used to size a read, only compared by Steam's dispatcher.
			int GetCallbackSizeBytes() override { return 0; }
		};

		JoinRequestedCallback g_JoinRequested;
		LobbyEnterCallback g_LobbyEnter;
		SessionRequestCallback g_SessionRequest;
		SessionFailedCallback g_SessionFailed;
		bool g_CallbacksRegistered = false;
	}

	// ---------------------------------------------------------------------
	// Public surface
	// ---------------------------------------------------------------------

	bool Initialize()
	{
		// The game already loaded this; we never load a second copy.
		HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
		if (!steam)
		{
			LogInfo("steam: steam_api64.dll is not loaded - Steam invites "
				"unavailable, TCP sessions still work");
			return false;
		}

		bool ok = true;
		ok &= Bind(steam, "SteamAPI_SteamUser_v023", g_Api.SteamUser);
		ok &= Bind(steam, "SteamAPI_SteamFriends_v018", g_Api.SteamFriends);
		ok &= Bind(steam, "SteamAPI_SteamMatchmaking_v009", g_Api.SteamMatchmaking);
		ok &= Bind(steam, "SteamAPI_SteamUtils_v010", g_Api.SteamUtils);
		ok &= Bind(steam, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002",
			g_Api.SteamNetworkingMessages);
		ok &= Bind(steam, "SteamAPI_ISteamUser_GetSteamID", g_Api.GetSteamID);
		ok &= Bind(steam, "SteamAPI_ISteamFriends_GetPersonaName", g_Api.GetPersonaName);

		// Not gated on `ok`: knowing the opponent's name is a nicety, and losing
		// the whole Steam path over it would be a bad trade.
		Bind(steam, "SteamAPI_ISteamFriends_GetFriendPersonaName",
			g_Api.GetFriendPersonaName);

		ok &= Bind(steam, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog",
			g_Api.ActivateInviteOverlay);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_CreateLobby", g_Api.CreateLobby);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_JoinLobby", g_Api.JoinLobby);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_LeaveLobby", g_Api.LeaveLobby);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_SetLobbyData", g_Api.SetLobbyData);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_GetLobbyData", g_Api.GetLobbyData);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
			g_Api.GetNumLobbyMembers);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex",
			g_Api.GetLobbyMemberByIndex);
		ok &= Bind(steam, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", g_Api.GetLobbyOwner);
		ok &= Bind(steam, "SteamAPI_ISteamUtils_IsAPICallCompleted", g_Api.IsAPICallCompleted);
		ok &= Bind(steam, "SteamAPI_ISteamUtils_GetAPICallResult", g_Api.GetAPICallResult);
		ok &= Bind(steam, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser",
			g_Api.SendMessageToUser);
		ok &= Bind(steam, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel",
			g_Api.ReceiveMessagesOnChannel);
		ok &= Bind(steam, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser",
			g_Api.AcceptSessionWithUser);
		ok &= Bind(steam, "SteamAPI_RegisterCallback", g_Api.RegisterCallback);
		ok &= Bind(steam, "SteamAPI_UnregisterCallback", g_Api.UnregisterCallback);

		if (!ok)
		{
			LogWarn("steam: not every required export resolved - Steam invites "
				"disabled, TCP sessions still work");
			return false;
		}

		// Resolve the interface pointers. Any of these can legitimately be null
		// if the game's Steam session is not up yet.
		g_User = g_Api.SteamUser();
		g_Friends = g_Api.SteamFriends();
		g_Matchmaking = g_Api.SteamMatchmaking();
		g_Utils = g_Api.SteamUtils();
		g_Messages = g_Api.SteamNetworkingMessages();

		if (!g_User || !g_Friends || !g_Matchmaking || !g_Utils || !g_Messages)
		{
			LogWarn("steam: interfaces unavailable (is Steam running?) - "
				"Steam invites disabled");
			return false;
		}

		// The game runs SteamAPI_RunCallbacks itself, so registering here is
		// enough to start receiving these.
		g_JoinRequested.m_iCallback = kGameLobbyJoinRequested;
		g_LobbyEnter.m_iCallback = kLobbyEnter;
		g_SessionRequest.m_iCallback = kMessagesSessionRequest;
		g_SessionFailed.m_iCallback = kMessagesSessionFailed;
		g_Api.RegisterCallback(&g_JoinRequested, kGameLobbyJoinRequested);
		g_Api.RegisterCallback(&g_LobbyEnter, kLobbyEnter);
		g_Api.RegisterCallback(&g_SessionRequest, kMessagesSessionRequest);
		g_Api.RegisterCallback(&g_SessionFailed, kMessagesSessionFailed);
		g_CallbacksRegistered = true;

		g_Available = true;

		const char* name = g_Api.GetPersonaName(g_Friends);
		LogInfo("steam: ready as '%s' - F7 invites a friend, no ports needed",
			name ? name : "?");
		return true;
	}

	void Shutdown()
	{
		if (!g_Available)
			return;

		LeaveSession();

		if (g_CallbacksRegistered && g_Api.UnregisterCallback)
		{
			g_Api.UnregisterCallback(&g_JoinRequested);
			g_Api.UnregisterCallback(&g_LobbyEnter);
			g_Api.UnregisterCallback(&g_SessionRequest);
			g_Api.UnregisterCallback(&g_SessionFailed);
			g_CallbacksRegistered = false;
		}

		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Inbound.clear();
		g_Available = false;
	}

	bool Available()
	{
		return g_Available;
	}

	void Tick()
	{
		if (!g_Available)
			return;

		JoinLobbyFromCommandLine();
		PollPendingLobbyCreate();

		// Report the lobby's population whenever it changes. A session that
		// never connects looks identical to one that was never attempted
		// without this: the host just sits in "hosting" saying nothing, which
		// is exactly what happened the first time this was tested.
		if (g_Lobby && g_Api.GetNumLobbyMembers)
		{
			static int last_reported = -1;
			const int members = g_Api.GetNumLobbyMembers(g_Matchmaking, g_Lobby);

			if (members != last_reported)
			{
				last_reported = members;
				LogInfo("steam: lobby now has %d member(s)%s", members,
					members < 2 ? " - waiting for your friend to join" : "");

				// You are always a member of a lobby you are in, so zero is not
				// "nobody joined yet" - it means this handle does not name a
				// real lobby. That is exactly what a mis-read LobbyCreated_t
				// produced, and it is worth naming outright, because every
				// downstream symptom of it (no peer, nothing ever sent,
				// "opponent ?" on the other machine) looks like something else.
				if (members == 0 && g_State != LobbyState::Joining)
				{
					LogError("steam: lobby %llu reports no members at all - that "
						"handle is not a real lobby. Press F11, then F7 to open "
						"a new one.",
						static_cast<unsigned long long>(g_Lobby));
				}
			}
		}

		// A host sits in Hosting until someone actually arrives.
		if (g_State == LobbyState::Hosting && g_Lobby &&
			g_Api.GetNumLobbyMembers(g_Matchmaking, g_Lobby) > 1)
		{
			AdoptPeerFromLobby();
		}

		// A guest that entered the lobby but has not yet adopted the host -
		// because the lobby's member list had not populated when LobbyEnter
		// fired - keeps trying rather than sitting in Joining forever.
		if (g_State == LobbyState::Joining && g_Lobby &&
			g_Api.GetNumLobbyMembers(g_Matchmaking, g_Lobby) > 1)
		{
			AdoptPeerFromLobby();
		}

		// The peer leaving must surface as a dropped link, otherwise the match
		// layer never learns the opponent is gone and would sit waiting. There
		// is no reconnecting into a match, so this ends it.
		if (g_State == LobbyState::Joined && g_Lobby &&
			g_Api.GetNumLobbyMembers(g_Matchmaking, g_Lobby) < 2)
		{
			const CSteamID self = g_Api.GetSteamID(g_User);
			const bool we_own_it =
				g_Api.GetLobbyOwner(g_Matchmaking, g_Lobby) == self;

			g_Peer = 0;
			g_PeerName.clear();

			if (we_own_it)
			{
				// Our lobby is still ours; someone else may still be invited.
				SetState(LobbyState::Hosting, "peer left the lobby");
				LogStage(kStageConnect,
					"steam: your opponent left - press F7 to invite someone else");
			}
			else
			{
				// The host is gone, so the lobby is not usable any more.
				LogStage(kStageConnect, "steam: the host left the lobby");
				LeaveSession();
			}
		}

		DrainIncoming();
	}

	bool HostSession()
	{
		if (!g_Available)
		{
			LogWarn("steam: unavailable - use F9 to host a direct session instead");
			return false;
		}

		LeaveSession();

		g_PendingCreate = g_Api.CreateLobby(g_Matchmaking, kLobbyTypeFriendsOnly, 2);
		if (g_PendingCreate == kInvalidAPICall)
		{
			LogError("steam: could not start lobby creation");
			return false;
		}

		SetState(LobbyState::Creating, "creating lobby");
		return true;
	}

	bool OpenInviteOverlay()
	{
		if (!g_Available || !g_Lobby)
		{
			LogWarn("steam: no lobby yet - press F7 again in a moment, or F9 to "
				"host a direct session");
			return false;
		}

		g_Api.ActivateInviteOverlay(g_Friends, g_Lobby);
		LogStage(kStageConnect, "steam: invite overlay opened");
		return true;
	}

	void LeaveSession()
	{
		if (!g_Available)
			return;

		if (g_Lobby && g_Api.LeaveLobby)
			g_Api.LeaveLobby(g_Matchmaking, g_Lobby);

		g_Lobby = 0;
		g_Peer = 0;
		g_PeerName.clear();
		g_PendingCreate = kInvalidAPICall;
		g_State = LobbyState::None;

		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Inbound.clear();
	}

	LobbyState State()
	{
		return g_State;
	}

	const char* StateName()
	{
		switch (g_State)
		{
		case LobbyState::None:     return "none";
		case LobbyState::Creating: return "creating";
		case LobbyState::Hosting:  return "hosting";
		case LobbyState::Joining:  return "joining";
		case LobbyState::Joined:   return "joined";
		}
		return "unknown";
	}

	std::string PeerName()
	{
		return g_PeerName;
	}

	std::string LocalName()
	{
		if (!g_Available || !g_Friends || !g_Api.GetPersonaName)
			return {};

		const char* name = g_Api.GetPersonaName(g_Friends);
		return name ? std::string(name) : std::string{};
	}

	bool IsConnected()
	{
		return g_Available && g_State == LobbyState::Joined && g_Peer != 0;
	}

	bool Send(const std::string& Payload)
	{
		if (!IsConnected() || !g_Api.SendMessageToUser)
			return false;

		SteamNetworkingIdentity identity = MakeIdentity(g_Peer);

		// EResult::k_EResultOK == 1
		const int result = g_Api.SendMessageToUser(
			g_Messages,
			&identity,
			Payload.data(),
			static_cast<uint32_t>(Payload.size()),
			kSendReliable,
			kChannel
		);

		if (result != 1)
		{
			LogWarn("steam: send failed (result %d)", result);
			return false;
		}

		return true;
	}

	bool Poll(std::string& OutPayload)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_Inbound.empty())
			return false;

		OutPayload = std::move(g_Inbound.front());
		g_Inbound.pop_front();
		return true;
	}
}
