// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Steam.h"
#include "Log.h"

#include <windows.h>

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

		// ELobbyType::k_ELobbyTypeFriendsOnly
		constexpr int kLobbyTypeFriendsOnly = 1;

		// ESteamNetworkingIdentityType::k_ESteamNetworkingIdentityType_SteamID
		constexpr int kIdentityTypeSteamID = 16;

		// k_nSteamNetworkingSend_Reliable
		constexpr int kSendReliable = 8;

		// The channel this mod talks on. Arbitrary, but must match on both
		// sides and must not collide with anything the game itself uses.
		constexpr int kChannel = 0x484D;

		#pragma pack(push, 1)
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
		#pragma pack(pop)

		static_assert(sizeof(SteamNetworkingIdentity) == 136,
			"SteamNetworkingIdentity layout does not match the Steamworks ABI");
		static_assert(sizeof(LobbyCreated_t) == 12, "LobbyCreated_t layout changed");
		static_assert(sizeof(GameLobbyJoinRequested_t) == 16,
			"GameLobbyJoinRequested_t layout changed");

		// SteamNetworkingMessage_t, as received from ReceiveMessagesOnChannel.
		// Only the leading fields are read; m_pfnRelease is called to free it.
		struct SteamNetworkingMessage_t
		{
			void* m_pData;
			int32_t m_cbSize;
			uint64_t m_conn;
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

				SteamNetworkingIdentity identity = MakeIdentity(g_Peer);
				if (g_Api.AcceptSessionWithUser)
					g_Api.AcceptSessionWithUser(g_Messages, &identity);

				SetState(LobbyState::Joined, "peer present in lobby");
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
				LogError("steam: lobby creation failed (result %d)", result.m_eResult);
				SetState(LobbyState::None, "lobby creation failed");
				return;
			}

			// The LobbyEnter callback normally gets here first and has already
			// stamped the protocol tag and moved us to Hosting. This path
			// exists to catch the failure case above, so only finish the job if
			// the callback has not already done it.
			g_Lobby = result.m_ulSteamIDLobby;

			if (g_State != LobbyState::Hosting)
			{
				g_Api.SetLobbyData(g_Matchmaking, g_Lobby,
					kLobbyKeyProtocol, kLobbyProtocolValue);
				SetState(LobbyState::Hosting, "lobby ready");
				LogStage(kStageConnect,
					"steam: lobby open - press F7 again to invite a friend");
			}
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

				if (!protocol || strcmp(protocol, kLobbyProtocolValue) != 0)
				{
					LogError("steam: host is running a different version of this "
						"mod - leaving");
					LeaveSession();
					return;
				}

				AdoptPeerFromLobby();
			}

			void Run(void* Parameter, bool, SteamAPICall_t) override { Run(Parameter); }
			int GetCallbackSizeBytes() override { return sizeof(LobbyEnter_t); }
		};

		JoinRequestedCallback g_JoinRequested;
		LobbyEnterCallback g_LobbyEnter;
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
		g_Api.RegisterCallback(&g_JoinRequested, kGameLobbyJoinRequested);
		g_Api.RegisterCallback(&g_LobbyEnter, kLobbyEnter);
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

		PollPendingLobbyCreate();

		// A host sits in Hosting until someone actually arrives.
		if (g_State == LobbyState::Hosting && g_Lobby &&
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
