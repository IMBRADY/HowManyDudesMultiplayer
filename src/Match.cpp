// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Match.h"
#include "GameBridge.h"
#include "Json.h"
#include "Log.h"
#include "Net.h"

#include <atomic>
#include <chrono>

using namespace YYTK;
using namespace Aurie;

namespace hmd::match
{
	namespace
	{
		// Message kinds on the wire. Every frame carries one.
		constexpr const char* kKindArmy = "army";
		constexpr const char* kKindResult = "result";
		constexpr const char* kKindStatus = "status";

		// Scripts the mod intercepts. Names confirmed against data.win.
		constexpr const char* kEndOfActScript = "gml_Script_victory_ui_round_finish";
		constexpr const char* kWaveScript = "gml_Script_matchup_get_enemies_to_fit";

		// How long to wait for the peer's army before giving up and letting the
		// act resolve normally, so a dropped peer cannot soft-lock the run.
		//
		// Five minutes is deliberately generous. The two runs are not step-locked
		// - each player sends when their own act ends - so this window is what
		// lets someone who is a little behind catch up rather than losing the
		// exchange. The wait is not silent: progress is reported while it runs.
		constexpr auto kPeerTimeout = std::chrono::minutes(5);

		// How often each side tells the other what it is doing, and how often
		// that gets surfaced to the player. Presence traffic is tiny next to an
		// army payload, but there is no reason to send it every tick.
		constexpr auto kStatusSendInterval = std::chrono::seconds(2);
		constexpr auto kPresenceNagInterval = std::chrono::seconds(15);
		constexpr auto kWaitProgressInterval = std::chrono::seconds(30);

		// A peer that has said nothing for this long is treated as unknown
		// rather than as "not in a run", so a stalled link cannot masquerade as
		// an opponent sitting in the menu.
		constexpr auto kPresenceStaleAfter = std::chrono::seconds(10);

		std::atomic<Phase> g_Phase{ Phase::Offline };
		std::atomic<bool> g_ActEnded{ false };

		Lives g_Lives;
		roster::Snapshot g_LocalArmy;
		roster::Snapshot g_PeerArmy;

		std::chrono::steady_clock::time_point g_WaitStarted;
		int g_InjectedCount = 0;
		bool g_ResultSent = false;
		bool g_PeerResultReceived = false;
		bool g_PeerWonTheirFight = false;

		// --- Presence -----------------------------------------------------
		PeerPresence g_PeerPresence;
		std::chrono::steady_clock::time_point g_PeerPresenceAt;
		std::chrono::steady_clock::time_point g_LastStatusSent;
		std::chrono::steady_clock::time_point g_LastPresenceNag;
		std::chrono::steady_clock::time_point g_LastWaitProgress;

		// --- Hook state ---------------------------------------------------
		PFUNC_YYGMLScript g_EndOfActTrampoline = nullptr;
		bool g_EndOfActHooked = false;

		void SetPhase(Phase NewPhase, const char* Reason)
		{
			g_Phase.store(NewPhase);
			LogInfo("phase -> %s (%s)", PhaseName(), Reason ? Reason : "");
		}

		bool InGameplayRoom()
		{
			return bridge::CurrentRoomName() == "rm_gameplay";
		}

		// "In a run" means the arena is actually populated, not merely that the
		// gameplay room is loaded - a room can be up for a moment before any
		// dudes exist, and an army cannot be captured from an empty arena.
		bool InRun()
		{
			return InGameplayRoom() && !bridge::FindInstances("o_dude").empty();
		}

		int LocalActNumber()
		{
			RValue act;
			if (bridge::CallScript("gml_Script_get_act_number", {}, act))
				return act.ToInt32();
			return 0;
		}

		// True when the peer has reported recently enough to be believed.
		bool PeerPresenceFresh()
		{
			return g_PeerPresence.known &&
				(std::chrono::steady_clock::now() - g_PeerPresenceAt) < kPresenceStaleAfter;
		}

		void ForgetPeerPresence()
		{
			g_PeerPresence = PeerPresence{};
			g_PeerPresenceAt = {};
		}

		// -----------------------------------------------------------------
		// Hook
		// -----------------------------------------------------------------
		//
		// Resolve a script's YYC entry point. GetNamedRoutinePointer hands back
		// a CScript*, and the native function lives at m_Functions->
		// m_ScriptFunction. Those offsets come from YYTK's struct definitions,
		// which are only correct if this runner matches what YYTK expects - so
		// before trusting any of it, we read CScript::m_Name back and require it
		// to equal the name we asked for. If the layout were wrong, that string
		// compare would fail (or the pointer would be unreadable) and we bail
		// out rather than hooking a bogus address.
		// What a guarded probe of a CScript found. Plain data only - this is
		// filled inside an SEH block, which cannot coexist with locals that
		// need C++ unwinding.
		struct ScriptProbe
		{
			bool faulted;
			const char* name;
			PFUNC_YYGMLScript entry;
		};

		ScriptProbe ProbeScript(void* Raw)
		{
			ScriptProbe probe{ false, nullptr, nullptr };

			__try
			{
				CScript* script = static_cast<CScript*>(Raw);
				probe.name = script->m_Name;

				YYGMLFuncs* functions = script->m_Functions;
				if (functions)
					probe.entry = functions->m_ScriptFunction;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				probe.faulted = true;
				probe.name = nullptr;
				probe.entry = nullptr;
			}

			return probe;
		}

		PFUNC_YYGMLScript ResolveScriptEntryPoint(const char* ScriptName)
		{
			void* raw = bridge::ScriptPointer(ScriptName);
			if (!raw)
				return nullptr;

			ScriptProbe probe = ProbeScript(raw);

			if (probe.faulted)
			{
				LogWarn("faulted while reading CScript for '%s' - refusing to hook",
					ScriptName);
				return nullptr;
			}

			// Layout sanity check: if the struct offsets were wrong for this
			// runner, m_Name would not read back as the name we looked up.
			if (!probe.name || strcmp(probe.name, ScriptName) != 0)
			{
				LogWarn("CScript layout check failed for '%s' (got '%s') - "
					"refusing to hook", ScriptName,
					probe.name ? probe.name : "<null>");
				return nullptr;
			}

			if (!probe.entry)
			{
				LogWarn("'%s' has no YYC entry point - refusing to hook", ScriptName);
				return nullptr;
			}

			return probe.entry;
		}

		// Our replacement for victory_ui_round_finish. It does the minimum
		// possible: note that the act ended, then run the original. All the
		// real work is deferred to Tick() on the next frame.
		RValue& EndOfActDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue* Arguments[]
		)
		{
			SignalActEnded();

			if (!g_EndOfActTrampoline)
			{
				// Should be unreachable - the hook is only installed once the
				// trampoline exists - but never return an uninitialised RValue.
				Result = RValue();
				return Result;
			}

			return g_EndOfActTrampoline(Self, Other, Result, ArgumentCount, Arguments);
		}

		bool InstallEndOfActHook(AurieModule* Module)
		{
			PFUNC_YYGMLScript target = ResolveScriptEntryPoint(kEndOfActScript);
			if (!target)
			{
				LogWarn("end-of-act hook unavailable - falling back to frame "
					"polling for act detection");
				return false;
			}

			PVOID trampoline = nullptr;
			AurieStatus status = MmCreateHook(
				Module,
				"hmd_end_of_act",
				reinterpret_cast<PVOID>(target),
				reinterpret_cast<PVOID>(&EndOfActDetour),
				&trampoline
			);

			if (!AurieSuccess(status) || !trampoline)
			{
				LogWarn("MmCreateHook failed for '%s' (status %d) - falling back "
					"to frame polling", kEndOfActScript, static_cast<int>(status));
				return false;
			}

			g_EndOfActTrampoline = reinterpret_cast<PFUNC_YYGMLScript>(trampoline);
			g_EndOfActHooked = true;
			LogInfo("hooked %s", kEndOfActScript);
			return true;
		}

		// -----------------------------------------------------------------
		// Messaging
		// -----------------------------------------------------------------
		std::string BuildArmyMessage(const roster::Snapshot& Army)
		{
			json::Value root;
			json::Value payload;
			if (!json::Parse(Army.Serialize(), payload))
				return {};

			root.Set("kind", json::Value(kKindArmy));
			root.Set("body", payload);
			return root.Serialize();
		}

		// Lightweight presence beacon: what this client is doing right now, so
		// the other side can say "waiting for your opponent to start a run"
		// instead of sitting silent.
		std::string BuildStatusMessage()
		{
			json::Value root;
			root.Set("kind", json::Value(kKindStatus));
			root.Set("in_run", json::Value(InRun()));
			root.Set("act", json::Value(LocalActNumber()));
			root.Set("phase", json::Value(PhaseName()));
			return root.Serialize();
		}

		std::string BuildResultMessage(bool LocalWon)
		{
			json::Value root;
			root.Set("kind", json::Value(kKindResult));
			// "won" is from the sender's point of view: true means the sender
			// beat the army we sent them.
			root.Set("won", json::Value(LocalWon));
			root.Set("lives", json::Value(g_Lives.local));
			return root.Serialize();
		}

		void HandleMessage(const std::string& Text)
		{
			json::Value root;
			if (!json::Parse(Text, root) || !root.IsObject())
			{
				LogWarn("discarding malformed peer message (%zu bytes)", Text.size());
				return;
			}

			const std::string kind = root["kind"].AsString();

			if (kind == kKindArmy)
			{
				roster::Snapshot peer;
				if (!roster::Snapshot::Deserialize(root["body"].Serialize(), peer))
					return;

				g_PeerArmy = std::move(peer);
				LogStage(kStageSerialize,
					"received peer army: %zu unit(s), act %d, peer lives %d",
					g_PeerArmy.units.size(), g_PeerArmy.act, g_PeerArmy.lives);

				if (g_Phase.load() == Phase::AwaitingPeer)
					SetPhase(Phase::Injecting, "peer army received");
				return;
			}

			if (kind == kKindStatus)
			{
				const bool was_in_run = g_PeerPresence.known && g_PeerPresence.in_run;

				g_PeerPresence.known = true;
				g_PeerPresence.in_run = root["in_run"].AsBool();
				g_PeerPresence.act = root["act"].AsInt(0);
				g_PeerPresenceAt = std::chrono::steady_clock::now();

				// Only announce the transition, not every beacon.
				if (!was_in_run && g_PeerPresence.in_run)
					LogInfo("your opponent has started a run (act %d)",
						g_PeerPresence.act);
				else if (was_in_run && !g_PeerPresence.in_run)
					LogInfo("your opponent has left their run");

				return;
			}

			if (kind == kKindResult)
			{
				g_PeerWonTheirFight = root["won"].AsBool();
				g_Lives.remote = root["lives"].AsInt(g_Lives.remote);
				g_PeerResultReceived = true;

				LogStage(kStageResolve,
					"peer reports %s against our army; peer lives %d",
					g_PeerWonTheirFight ? "a win" : "a loss", g_Lives.remote);
				return;
			}

			LogWarn("ignoring peer message of unknown kind '%s'", kind.c_str());
		}

		// -----------------------------------------------------------------
		// Battle outcome
		// -----------------------------------------------------------------
		//
		// Resolved by observation rather than by hooking the loss path: count
		// what is still standing. This is immune to the exact shape of the
		// game's own victory/loss bookkeeping.
		enum class Outcome { Undecided, LocalWon, LocalLost };

		Outcome EvaluateBattle()
		{
			std::vector<RValue> enemies = bridge::FindInstances("o_enemy");
			std::vector<RValue> dudes = bridge::FindInstances("o_dude");

			if (enemies.empty() && dudes.empty())
				return Outcome::Undecided; // Arena not populated yet.

			if (enemies.empty())
				return Outcome::LocalWon;

			// The local side is beaten once every dude is knocked out.
			bool any_standing = false;
			for (const RValue& dude : dudes)
			{
				RValue knocked;
				if (bridge::CallScript("gml_Script_dude_is_knocked_out", { dude }, knocked))
				{
					if (!knocked.ToBoolean())
					{
						any_standing = true;
						break;
					}
				}
				else
				{
					// If the script is unavailable, treat a live instance as
					// standing rather than declaring a loss we cannot verify.
					any_standing = true;
					break;
				}
			}

			return any_standing ? Outcome::Undecided : Outcome::LocalLost;
		}

		void ApplyOutcome(bool LocalWon)
		{
			if (!LocalWon)
			{
				g_Lives.local--;
				LogStage(kStageResolve, "local player lost a life (%d remaining)",
					g_Lives.local);
			}
			else
			{
				LogStage(kStageResolve, "local player won the exchange");
			}

			if (!g_ResultSent)
			{
				net::Send(BuildResultMessage(LocalWon));
				g_ResultSent = true;
			}
		}
	}

	// ---------------------------------------------------------------------
	// Public surface
	// ---------------------------------------------------------------------
	bool Initialize(AurieModule* Module)
	{
		g_Lives = Lives{};
		ResetMatchState();

		// The hook is an optimisation over polling, not a requirement. If it
		// cannot be installed safely the mod still works.
		InstallEndOfActHook(Module);

		if (!bridge::ScriptExists(kWaveScript))
		{
			LogWarn("'%s' did not resolve - default wave suppression will rely "
				"on clearing spawned enemies instead", kWaveScript);
		}

		SetPhase(Phase::Offline, "initialised");
		return true;
	}

	void Shutdown(AurieModule* Module)
	{
		if (g_EndOfActHooked && Module)
		{
			MmRemoveHook(Module, "hmd_end_of_act");
			g_EndOfActHooked = false;
			g_EndOfActTrampoline = nullptr;
		}

		roster::SetDefaultWaveSuppressed(false);
		SetPhase(Phase::Offline, "shutdown");
	}

	// Wipes the match completely, lives included. Matches never survive a
	// disconnect, so there is no state worth carrying between them - and
	// carrying lives across was previously a real bug: a second match would
	// start on whatever the first one ended with.
	void ResetMatchState()
	{
		g_ActEnded.store(false);
		g_Lives = Lives{};
		g_LocalArmy = roster::Snapshot{};
		g_PeerArmy = roster::Snapshot{};
		g_InjectedCount = 0;
		g_ResultSent = false;
		g_PeerResultReceived = false;
		g_PeerWonTheirFight = false;
		ForgetPeerPresence();
		g_LastStatusSent = {};
		g_LastPresenceNag = {};
		g_LastWaitProgress = {};
		roster::SetDefaultWaveSuppressed(false);
	}

	void SignalActEnded()
	{
		g_ActEnded.store(true);
	}

	bool ShouldSuppressDefaultWave()
	{
		return roster::IsDefaultWaveSuppressed();
	}

	Phase CurrentPhase()
	{
		return g_Phase.load();
	}

	const char* PhaseName()
	{
		switch (g_Phase.load())
		{
		case Phase::Offline:      return "offline";
		case Phase::Idle:         return "idle";
		case Phase::Capturing:    return "capturing";
		case Phase::AwaitingPeer: return "awaiting-peer";
		case Phase::Injecting:    return "injecting";
		case Phase::Fighting:     return "fighting";
		case Phase::Resolving:    return "resolving";
		case Phase::GameOver:     return "game-over";
		}
		return "unknown";
	}

	Lives CurrentLives()
	{
		return g_Lives;
	}

	PeerPresence CurrentPeerPresence()
	{
		// Stale beacons are reported as "unknown" rather than as a stale truth.
		if (!PeerPresenceFresh())
			return PeerPresence{};

		return g_PeerPresence;
	}

	bool LocalInRun()
	{
		return InRun();
	}

	void Tick()
	{
		// Always drain the network queue, whatever phase we are in, so peer
		// messages are never left to go stale.
		std::string message;
		while (net::Poll(message))
			HandleMessage(message);

		const Phase phase = g_Phase.load();

		// A dropped link ends the match. There is no resuming: matches are not
		// recoverable across a disconnect, so the state is torn down and the
		// next connection starts a fresh one with full lives. This also covers
		// GameOver, which must return to Offline or a later session would find
		// the phase stuck and never start.
		if (!net::IsConnected())
		{
			if (phase != Phase::Offline)
			{
				if (phase != Phase::GameOver)
				{
					LogStage(kStageResolve,
						"peer disconnected - the match is over. Lives were "
						"local %d, remote %d. Reconnecting starts a new match.",
						g_Lives.local, g_Lives.remote);
				}

				roster::SetDefaultWaveSuppressed(false);
				ResetMatchState();
				SetPhase(Phase::Offline, "peer link lost - match ended");
			}
			return;
		}

		if (phase == Phase::Offline)
		{
			// Every connection is a new match, by design.
			ResetMatchState();
			SetPhase(Phase::Idle, "peer link established - new match, 3 lives each");
			return;
		}

		if (phase == Phase::GameOver)
			return;

		// Tell the peer what we are doing, so their client can report it.
		const auto now = std::chrono::steady_clock::now();
		if (now - g_LastStatusSent >= kStatusSendInterval)
		{
			g_LastStatusSent = now;
			net::Send(BuildStatusMessage());
		}

		switch (phase)
		{
		case Phase::Idle:
		{
			// Neither side can exchange anything until both are actually in a
			// run. Say so periodically rather than leaving the player guessing.
			if (now - g_LastPresenceNag >= kPresenceNagInterval)
			{
				const bool local_in_run = InRun();
				const bool peer_in_run = PeerPresenceFresh() && g_PeerPresence.in_run;

				if (local_in_run && !peer_in_run)
				{
					g_LastPresenceNag = now;
					LogInfo("waiting for your opponent to start a run - armies "
						"are exchanged when you both finish an act");
				}
				else if (!local_in_run && peer_in_run)
				{
					g_LastPresenceNag = now;
					LogInfo("your opponent is in a run (act %d) - start yours to "
						"play against them", g_PeerPresence.act);
				}
			}

			if (!g_ActEnded.load())
				return;

			g_ActEnded.store(false);

			if (!InGameplayRoom())
			{
				LogWarn("act-end signalled outside rm_gameplay - ignoring");
				return;
			}

			SetPhase(Phase::Capturing, "act ended");
			return;
		}

		case Phase::Capturing:
		{
			// Halt normal act progression while the exchange happens.
			roster::SetDefaultWaveSuppressed(true);

			if (!roster::CaptureLocalArmy(g_LocalArmy))
			{
				// Nothing to send. Let the act proceed vanilla rather than
				// stalling the run.
				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::Idle, "army capture failed");
				return;
			}

			g_LocalArmy.lives = g_Lives.local;

			RValue act_number;
			if (bridge::CallScript("gml_Script_get_act_number", {}, act_number))
				g_LocalArmy.act = act_number.ToInt32();

			const std::string payload = BuildArmyMessage(g_LocalArmy);
			if (payload.empty() || !net::Send(payload))
			{
				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::Idle, "could not transmit army");
				return;
			}

			LogStage(kStageSerialize, "transmitted %zu byte army payload",
				payload.size());

			g_WaitStarted = std::chrono::steady_clock::now();

			// The peer's army may already have arrived while we were capturing.
			SetPhase(
				g_PeerArmy.units.empty() && g_PeerArmy.matchup.empty()
					? Phase::AwaitingPeer
					: Phase::Injecting,
				"local army sent"
			);
			return;
		}

		case Phase::AwaitingPeer:
		{
			const auto waited = now - g_WaitStarted;

			if (waited > kPeerTimeout)
			{
				LogWarn("your opponent did not finish an act within %lld minutes "
					"- playing this act normally",
					static_cast<long long>(
						std::chrono::duration_cast<std::chrono::minutes>(
							kPeerTimeout).count()));
				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::Idle, "peer timeout");
				return;
			}

			// A five minute wait must not look like a hang. Report progress,
			// and say specifically what the opponent is doing when we know.
			if (now - g_LastWaitProgress >= kWaitProgressInterval)
			{
				g_LastWaitProgress = now;

				const auto elapsed = std::chrono::duration_cast<
					std::chrono::seconds>(waited).count();
				const auto remaining = std::chrono::duration_cast<
					std::chrono::seconds>(kPeerTimeout - waited).count();

				if (!PeerPresenceFresh())
				{
					LogInfo("waiting for your opponent's army - no word from "
						"them for a while (%llds elapsed, %llds left)",
						static_cast<long long>(elapsed),
						static_cast<long long>(remaining));
				}
				else if (!g_PeerPresence.in_run)
				{
					LogInfo("waiting for your opponent to start a run "
						"(%llds elapsed, %llds left)",
						static_cast<long long>(elapsed),
						static_cast<long long>(remaining));
				}
				else
				{
					LogInfo("waiting for your opponent to finish act %d "
						"(%llds elapsed, %llds left)",
						g_PeerPresence.act,
						static_cast<long long>(elapsed),
						static_cast<long long>(remaining));
				}
			}
			return;
		}

		case Phase::Injecting:
		{
			roster::ClearDefaultEnemyWave();
			g_InjectedCount = roster::InjectOpponentArmy(g_PeerArmy);

			if (g_InjectedCount <= 0)
			{
				LogWarn("no opponent units were injected - resuming normal act "
					"progression");
				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::Idle, "injection produced no units");
				return;
			}

			g_ResultSent = false;
			SetPhase(Phase::Fighting, "opponent wave live");
			return;
		}

		case Phase::Fighting:
		{
			Outcome outcome = EvaluateBattle();
			if (outcome == Outcome::Undecided)
				return;

			ApplyOutcome(outcome == Outcome::LocalWon);
			SetPhase(Phase::Resolving, "battle decided");
			return;
		}

		case Phase::Resolving:
		{
			// Wait for the peer's own result so both clients agree on the score
			// before deciding whether the run continues.
			if (!g_PeerResultReceived)
				return;

			LogStage(kStageResolve, "lives - local %d, remote %d",
				g_Lives.local, g_Lives.remote);

			if (g_Lives.local <= 0 || g_Lives.remote <= 0)
			{
				LogStage(kStageResolve, "game over - %s",
					g_Lives.local <= 0 ? "local player is out of lives"
									   : "peer is out of lives");
				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::GameOver, "a player reached zero lives");
				return;
			}

			// Lives remain on both sides: advance to the next act.
			g_PeerArmy = roster::Snapshot{};
			g_LocalArmy = roster::Snapshot{};
			g_PeerResultReceived = false;
			g_ResultSent = false;
			g_InjectedCount = 0;
			roster::SetDefaultWaveSuppressed(false);

			SetPhase(Phase::Idle, "advancing to the next act");
			return;
		}

		default:
			return;
		}
	}
}
