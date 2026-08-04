// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Match.h"
#include "GameBridge.h"
#include "GameHooks.h"
#include "Json.h"
#include "Log.h"
#include "Net.h"
#include "RunState.h"
#include "Sanitize.h"
#include "Steam.h"
#include "Ui.h"

#include <atomic>
#include <chrono>
#include <mutex>

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
		constexpr const char* kKindRunStart = "run_start";

		// Scripts the mod intercepts. Names confirmed against data.win.
		constexpr const char* kEndOfActScript = "gml_Script_victory_ui_round_finish";
		constexpr const char* kWaveScript = "gml_Script_matchup_get_enemies_to_fit";
		constexpr const char* kRunStartScript = "gml_Script_new_run_start";

		// Present in this build, resolves cleanly, and must not be called. It is
		// a progression routine that grants relics and updates collectible and
		// star state on its way through, and it aborts the game when a mod calls
		// it. AbandonDuel used to; the comment there has the stack trace. Kept
		// named so the probe can still report on it and so nobody reintroduces
		// the call without reading why it went.
		// constexpr const char* kJumpToRoundScript = "gml_Script_jump_to_round";

		// How long to wait for the peer's army before giving up and letting the
		// round resolve normally, so a dropped peer cannot soft-lock the run.
		//
		// Five minutes is deliberately generous. The two runs are not step-locked
		// - each player reaches the duel round in their own time - so this window
		// is what lets someone who is a little behind catch up rather than losing
		// the exchange. The wait is not silent: progress is reported while it runs.
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

		// While the arena is being held empty for a duel, the game may keep
		// trying to populate it. Sweeping on an interval rather than every tick
		// keeps that cheap.
		constexpr auto kWaveClearInterval = std::chrono::milliseconds(500);

		// The opponent's army has to actually appear before its absence can
		// mean anything. Until it does, an empty arena means "not spawned yet",
		// not "you won".
		constexpr auto kInjectionSettleTime = std::chrono::seconds(2);

		std::atomic<Phase> g_Phase{ Phase::Offline };
		std::atomic<bool> g_ActEnded{ false };
		std::atomic<bool> g_SyncRunStart{ true };

		Lives g_Lives;
		roster::Snapshot g_LocalArmy;
		roster::Snapshot g_PeerArmy;

		std::chrono::steady_clock::time_point g_WaitStarted;
		int g_InjectedCount = 0;
		bool g_ResultSent = false;
		bool g_PeerResultReceived = false;
		bool g_PeerWonTheirFight = false;

		// --- Duel state ---------------------------------------------------
		// The round this duel is being fought at, and the last one that was
		// seen through to a result. The second is what stops a finished duel
		// from immediately re-triggering: the player is still standing on round
		// 20 when it resolves.
		int g_DuelRound = 0;
		int g_CompletedDuelRound = 0;

		// The progress value this duel is gated on. Compared against the peer's
		// reported progress rather than against their round, because progress
		// is defined on both clients whatever either could resolve.
		int g_DuelProgress = 0;

		std::chrono::steady_clock::time_point g_LastWaveClear;

		// Set once the injected wave has actually been observed alive. Without
		// it, the "no enemies left" test would fire on the empty arena that
		// exists for a moment before the opponent's army spawns.
		bool g_SawInjectedEnemies = false;
		std::chrono::steady_clock::time_point g_InjectedAt;

		// How long the opponent's in-run state must hold before it is announced
		// to the player. Longer than any room transition, shorter than anyone
		// would notice waiting.
		constexpr auto kPresenceSettleTime = std::chrono::seconds(4);

		// --- Presence -----------------------------------------------------
		PeerPresence g_PeerPresence;

		// Debounce state for the in-run announcement: what we last told the
		// player, what the beacons are currently claiming, and since when.
		bool g_PeerRunAnnounced = false;
		bool g_PeerRunPending = false;
		std::chrono::steady_clock::time_point g_PeerRunPendingSince;
		std::chrono::steady_clock::time_point g_PeerPresenceAt;
		std::chrono::steady_clock::time_point g_LastStatusSent;
		std::chrono::steady_clock::time_point g_LastPresenceNag;
		std::chrono::steady_clock::time_point g_LastWaitProgress;

		// --- Run start sync -----------------------------------------------
		// Filled by the peer, drained on the game thread. new_run_start must be
		// called from the game's own thread, and messages arrive on the network
		// worker, so the two are decoupled through here.
		std::mutex g_RunStartMutex;
		bool g_RunStartPending = false;
		std::vector<RValue> g_RunStartArguments;

		// True while we are replaying the peer's run start, so our own hook does
		// not bounce it straight back at them.
		std::atomic<bool> g_ReplayingRunStart{ false };

		// --- Hook state ---------------------------------------------------
		PFUNC_YYGMLScript g_EndOfActTrampoline = nullptr;
		PFUNC_YYGMLScript g_RunStartTrampoline = nullptr;

		void SetPhase(Phase NewPhase, const char* Reason)
		{
			g_Phase.store(NewPhase);
			LogInfo("phase -> %s (%s)", PhaseName(), Reason ? Reason : "");
		}

		bool InGameplayRoom()
		{
			return bridge::CurrentRoomName() == "rm_gameplay";
		}

		// "In a run" spans the whole run, not just the arena.
		//
		// This used to require rm_gameplay AND live o_dude instances, which is
		// the right test for "can I capture an army right now" and the wrong
		// one for "is this player in a run". Between rounds the player is in
		// rm_ranch, so the strict test flickered false every single round - and
		// each flicker announced "your opponent started a run" / "left their
		// run" to the other player. That is the spam the tester saw.
		//
		// The ranch is part of a run. So is the arena before it has populated.
		bool InRun()
		{
			const std::string room = bridge::CurrentRoomName();
			const bool in_run = (room == "rm_gameplay" || room == "rm_ranch");

			// Latched here because this is the one place that already knows what
			// a run room is, and it is asked every tick. What it feeds is the
			// "have we actually left the menu yet" test in the idle case.
			if (in_run)
				runstate::NoteInRunRoom();

			return in_run;
		}

		// The stricter test: an army can actually be read out of the arena.
		bool ArenaIsPopulated()
		{
			return InGameplayRoom() && !bridge::FindInstances("o_dude").empty();
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
			g_PeerRunAnnounced = false;
			g_PeerRunPending = false;
			g_PeerRunPendingSince = {};
		}

		// How the opponent should be referred to on screen. Their own reported
		// name first, then whatever Steam told us, then something neutral.
		std::string PeerLabel()
		{
			if (!g_PeerPresence.name.empty())
				return g_PeerPresence.name;

			const std::string steam_name = steam::PeerName();
			if (!steam_name.empty())
				return steam_name;

			return "your opponent";
		}

		// -----------------------------------------------------------------
		// Hooks
		// -----------------------------------------------------------------

		// Our replacement for victory_ui_round_finish. It does the minimum
		// possible: note that a round ended, then run the original.
		RValue& EndOfActDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue* Arguments[]
		)
		{
			// This is a ROUND finish, not an act finish - the script's own name
			// says so, and the logs confirm it fires after round 1. Treating it
			// as an act boundary is what made the very first duel trigger on
			// round 2 instead of round 20. It is now what counts rounds.
			runstate::NoteRoundFinished();
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

		// Convert a hook argument into something that survives the wire. Only
		// scalars do; a struct argument is a run configuration we have no way to
		// rebuild on the far side, and saying so is better than sending a
		// half-formed one.
		bool SerializeRunStartArguments(
			int ArgumentCount,
			RValue* Arguments[],
			json::Value& Out
		)
		{
			Out = json::Value::Array();

			for (int i = 0; i < ArgumentCount; i++)
			{
				const RValue* argument = hooks::Argument(ArgumentCount, Arguments, i);
				if (!argument)
					return false;

				switch (argument->m_Kind)
				{
				case VALUE_REAL:
				case VALUE_INT32:
				case VALUE_INT64:
				case VALUE_BOOL:
					Out.Push(json::Value(argument->ToDouble()));
					break;

				case VALUE_STRING:
				{
					const char* text = argument->ToCString();
					Out.Push(json::Value(text ? std::string(text) : std::string{}));
					break;
				}

				case VALUE_UNDEFINED:
					// Replayable: GML treats a missing argument as undefined
					// anyway, so dropping it reproduces the same call.
					Out.Push(json::Value());
					break;

				default:
					return false;
				}
			}

			return true;
		}

		// Feature 1: pressing "start run" pulls the opponent in too.
		RValue& RunStartDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue* Arguments[]
		)
		{
			HMD_LOG_SIGNATURE_ONCE(kRunStartScript, ArgumentCount, Arguments, nullptr);

			// Round 1 starts here, however the run was started.
			runstate::NoteRunStarted();

			// A run we started because the peer told us to must not be echoed
			// back, or the two clients would bounce starts off each other.
			const bool replaying = g_ReplayingRunStart.load();

			if (!replaying && g_SyncRunStart.load() && net::IsConnected())
			{
				json::Value arguments;
				const bool replayable =
					SerializeRunStartArguments(ArgumentCount, Arguments, arguments);

				json::Value root;
				root.Set("kind", json::Value(kKindRunStart));
				root.Set("replayable", json::Value(replayable));
				if (replayable)
					root.Set("args", arguments);

				if (!replayable)
				{
					LogWarn("new_run_start was called with an argument this mod "
						"cannot forward - your opponent will be started with the "
						"game's defaults instead");
				}

				net::Send(root.Serialize());
				ui::Notify("Starting a run - pulling %s in too.",
					PeerLabel().c_str());
			}

			if (!g_RunStartTrampoline)
			{
				Result = RValue();
				return Result;
			}

			return g_RunStartTrampoline(Self, Other, Result, ArgumentCount, Arguments);
		}

		// Perform a run start the peer asked for. Called from Tick, on the
		// game's own thread - new_run_start changes rooms and rebuilds the run,
		// which is not something to do from a network worker.
		void DrainPendingRunStart()
		{
			std::vector<RValue> arguments;

			{
				std::lock_guard<std::mutex> lock(g_RunStartMutex);
				if (!g_RunStartPending)
					return;

				g_RunStartPending = false;
				arguments = std::move(g_RunStartArguments);
				g_RunStartArguments.clear();
			}

			// Never yank someone out of a run they are already playing.
			if (InRun())
			{
				LogWarn("your opponent started a run, but you are already in one "
					"- staying where you are");
				return;
			}

			if (!bridge::ScriptExists(kRunStartScript))
			{
				LogWarn("cannot start a run to match your opponent - '%s' did not "
					"resolve", kRunStartScript);
				return;
			}

			ui::Notify("%s started a run - starting yours.", PeerLabel().c_str());

			g_ReplayingRunStart.store(true);
			const bool started =
				bridge::CallScriptAnnounced(kRunStartScript, arguments);
			g_ReplayingRunStart.store(false);

			if (!started)
			{
				LogWarn("the synced run start did not take - start your run "
					"manually and the duel will still work");
			}
		}

		bool InstallHooks(AurieModule* Module)
		{
			g_EndOfActTrampoline = hooks::Install(
				Module, "hmd_end_of_act", kEndOfActScript, &EndOfActDetour);

			if (!g_EndOfActTrampoline)
			{
				LogWarn("end-of-round hook unavailable - duels will still trigger "
					"from the round number, this only costs a little accuracy in "
					"detecting when a round finished");
			}

			g_RunStartTrampoline = hooks::Install(
				Module, "hmd_run_start", kRunStartScript, &RunStartDetour);

			if (!g_RunStartTrampoline)
			{
				LogWarn("run-start hook unavailable - starting a run will not "
					"pull your opponent in; you will both need to press start");
			}

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
		// the other side can report it, place the marker on their round track,
		// and know when both players have reached the duel round.
		std::string BuildStatusMessage()
		{
			json::Value root;
			root.Set("kind", json::Value(kKindStatus));
			root.Set("in_run", json::Value(InRun()));
			root.Set("act", json::Value(runstate::CurrentAct()));
			root.Set("round", json::Value(runstate::CurrentRound()));
			root.Set("progress", json::Value(runstate::CurrentProgress()));
			root.Set("phase", json::Value(PhaseName()));
			root.Set("name", json::Value(steam::LocalName()));
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

		void HandleRunStartMessage(const json::Value& Root)
		{
			if (!g_SyncRunStart.load())
				return;

			std::vector<RValue> arguments;

			if (Root["replayable"].AsBool())
			{
				const json::Value& array = Root["args"];
				if (array.IsArray())
				{
					// A run start takes a handful of arguments at most. A peer
					// claiming otherwise is not one this mod built.
					constexpr size_t kMaxArguments = 16;

					for (const json::Value& item : array.Items())
					{
						if (arguments.size() >= kMaxArguments)
						{
							LogWarn("peer sent %zu run-start arguments - ignoring "
								"the rest", array.Size());
							break;
						}

						switch (item.GetType())
						{
						case json::Type::Number:
							arguments.push_back(RValue(item.AsNumber()));
							break;
						case json::Type::Bool:
							arguments.push_back(RValue(item.AsBool()));
							break;
						case json::Type::String:
							arguments.push_back(
								RValue(sanitize::ClampText(item.AsString())));
							break;
						default:
							arguments.push_back(RValue());
							break;
						}
					}
				}
			}

			std::lock_guard<std::mutex> lock(g_RunStartMutex);
			g_RunStartPending = true;
			g_RunStartArguments = std::move(arguments);
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
				g_PeerPresence.round = static_cast<int>(
					sanitize::ClampNumber(root["round"].AsInt(0), 0.0, 1000.0));
				g_PeerPresence.progress = static_cast<int>(
					sanitize::ClampNumber(root["progress"].AsInt(0), 0.0, 1000.0));
				g_PeerPresence.name = sanitize::ClampText(root["name"].AsString());
				g_PeerPresenceAt = std::chrono::steady_clock::now();

				UNREFERENCED_PARAMETER(was_in_run);

				// Announce only a change that has held for a while.
				//
				// Reporting every transition meant announcing "started a run"
				// and "left their run" over and over, because the old in-run
				// test went false every time the opponent passed through the
				// ranch between rounds. That test is fixed, but the debounce
				// stays: a room transition should never be able to spam the
				// other player, however brief.
				const auto now = std::chrono::steady_clock::now();

				if (g_PeerPresence.in_run != g_PeerRunPending)
				{
					g_PeerRunPending = g_PeerPresence.in_run;
					g_PeerRunPendingSince = now;
				}
				else if (g_PeerRunPending != g_PeerRunAnnounced &&
					(now - g_PeerRunPendingSince) >= kPresenceSettleTime)
				{
					g_PeerRunAnnounced = g_PeerRunPending;

					ui::Notify(g_PeerRunAnnounced
						? "%s started a run."
						: "%s left their run.", PeerLabel().c_str());
				}

				return;
			}

			if (kind == kKindRunStart)
			{
				HandleRunStartMessage(root);
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

			// The opponent's army has to have been seen alive before its
			// absence can mean a win. Injection is not instantaneous, and the
			// arena is legitimately empty for a moment either side of it - which
			// is exactly the window that used to be misread as an instant
			// victory, and which the duel gate would otherwise fall straight
			// through.
			if (!enemies.empty())
				g_SawInjectedEnemies = true;

			if (!g_SawInjectedEnemies)
			{
				const auto waited = std::chrono::steady_clock::now() - g_InjectedAt;
				if (waited > kInjectionSettleTime)
				{
					// Nothing ever spawned to fight. Awarding the round rather
					// than hanging is the only option that keeps the run
					// playable, and it is logged as the anomaly it is.
					LogWarn("the opponent's army never appeared in the arena - "
						"awarding the duel so the run can continue");
					return Outcome::LocalWon;
				}

				return Outcome::Undecided;
			}

			if (enemies.empty() && dudes.empty())
				return Outcome::Undecided; // Arena torn down, not a result.

			if (enemies.empty())
				return Outcome::LocalWon;

			// The local side is beaten once no dude instances are left standing.
			//
			// This used to ask dude_is_knocked_out per dude, every tick, for the
			// whole duel. That script aborts the game on an argument it does not
			// like, and it did exactly that on a tester's machine - see the note
			// in Roster::CaptureLocalArmy. Live instances are counted instead.
			//
			// The difference that matters: if the game keeps knocked-out dudes
			// as live instances, a loss is not detected until they are actually
			// destroyed. So this can be slow to call a loss - it cannot call one
			// that has not happened, and a slow local resolve falls through to
			// the peer's own reported result rather than to a wrong answer.
			const bool any_standing = !dudes.empty();

			return any_standing ? Outcome::Undecided : Outcome::LocalLost;
		}

		void ApplyOutcome(bool LocalWon)
		{
			if (!LocalWon)
			{
				g_Lives.local--;
				ui::Notify("You lost the duel. %d %s left.",
					g_Lives.local, g_Lives.local == 1 ? "life" : "lives");
			}
			else
			{
				ui::Notify("You beat %s's army!", PeerLabel().c_str());
			}

			if (!g_ResultSent)
			{
				net::Send(BuildResultMessage(LocalWon));
				g_ResultSent = true;
			}
		}

		// -----------------------------------------------------------------
		// Duel gate
		// -----------------------------------------------------------------

		// Hold the arena empty while waiting for the opponent. The game will
		// keep trying to populate a boss round, so this is swept rather than
		// done once.
		void HoldArenaEmpty(std::chrono::steady_clock::time_point Now)
		{
			if (Now - g_LastWaveClear < kWaveClearInterval)
				return;

			g_LastWaveClear = Now;
			roster::ClearDefaultEnemyWave();
		}

		// Give the round back to the game after a gate fails.
		//
		// This used to call gml_Script_jump_to_round to rebuild the wave it had
		// cleared, on the reasoning that re-entering the round regenerates it.
		// It does - and it also re-runs everything else round entry does. A
		// tester's game died inside it:
		//
		//     I32 argument is undefined
		//     - gml_Script_gold_star_round:15
		//     - gml_Script_collectibles_can_earn_credit:251
		//     - gml_Script_collectible_set_discovered:48
		//     - gml_Script_relic_acquire:966
		//     - gml_Script_jump_to_round:10
		//
		// jump_to_round is a progression routine, not a wave builder. It grants
		// relics and touches collectible and star bookkeeping, and that chain is
		// not survivable when it is entered from a mod tick instead of from the
		// game's own flow. Crashing the player is strictly worse than any state
		// this function was trying to rescue them from, so it no longer calls it.
		//
		// What is left is honest: stop suppressing, then say what actually
		// happened. If the sweep never destroyed anything the round is untouched
		// and there is nothing to rescue. If it did, the wave is gone and no
		// amount of mod-side cleverness brings it back - so the player is told
		// plainly rather than being crashed on their behalf.
		void AbandonDuel(const char* Reason)
		{
			roster::SetDefaultWaveSuppressed(false);

			const int cleared = roster::EnemiesCleared();
			roster::ResetEnemiesCleared();

			if (cleared <= 0)
			{
				// The common case, and the quiet one: the gate held an arena that
				// was already empty. Nothing was taken from the player.
				if (g_DuelRound > 0)
				{
					ui::Notify("Duel called off - %s. Playing round %d normally.",
						Reason, g_DuelRound);
				}
				else
				{
					ui::Notify("Duel called off - %s. Playing on normally.", Reason);
				}
			}
			else
			{
				// The mod deleted this round's wave and the duel that was meant
				// to replace it never happened. Nothing can put it back.
				ui::Notify("Duel called off - %s. This round's enemies were "
					"already cleared for the duel - leave the run and start a "
					"new one.", Reason);

				LogWarn("%d enemy instance(s) were cleared for a duel that did "
					"not happen - round %d cannot be rebuilt (jump_to_round is "
					"not safe to call; see AbandonDuel)", cleared, g_DuelRound);
			}

			// Do not immediately re-enter the gate on the same round.
			g_CompletedDuelRound = g_DuelRound;
			g_DuelRound = 0;
		}

		// Publish what the overlay should be drawing.
		void PushOverlay(const std::string& Banner)
		{
			ui::Overlay overlay;
			overlay.connected = net::IsConnected();

			int tracked = 0;

			if (overlay.connected)
			{
				const PeerPresence presence = CurrentPeerPresence();
				overlay.peer_name = !presence.name.empty()
					? presence.name
					: steam::PeerName();
				overlay.peer_round = presence.round;
				overlay.peer_in_run = presence.in_run;
				overlay.banner = Banner;

				if (presence.in_run)
					tracked = presence.round;
			}

			// Tell the run map which cell is worth measuring. Exactly one is:
			// the one the opponent is standing on.
			runstate::SetTrackedRound(tracked);

			ui::SetOverlay(overlay);
		}
	}

	// ---------------------------------------------------------------------
	// Public surface
	// ---------------------------------------------------------------------
	bool Initialize(AurieModule* Module)
	{
		g_Lives = Lives{};
		ResetMatchState();

		InstallHooks(Module);

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
		if (Module)
		{
			if (g_EndOfActTrampoline)
			{
				hooks::Remove(Module, "hmd_end_of_act");
				g_EndOfActTrampoline = nullptr;
			}

			if (g_RunStartTrampoline)
			{
				hooks::Remove(Module, "hmd_run_start");
				g_RunStartTrampoline = nullptr;
			}
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
		g_DuelRound = 0;
		g_CompletedDuelRound = 0;
		g_DuelProgress = 0;
		g_SawInjectedEnemies = false;
		ForgetPeerPresence();
		g_LastStatusSent = {};
		g_LastPresenceNag = {};
		g_LastWaitProgress = {};
		g_LastWaveClear = {};

		{
			std::lock_guard<std::mutex> lock(g_RunStartMutex);
			g_RunStartPending = false;
			g_RunStartArguments.clear();
		}

		roster::SetDefaultWaveSuppressed(false);
	}

	void SignalActEnded()
	{
		g_ActEnded.store(true);
	}

	void SetSyncRunStart(bool Enabled)
	{
		g_SyncRunStart.store(Enabled);
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
		case Phase::Offline:           return "offline";
		case Phase::Idle:              return "idle";
		case Phase::AwaitingPeerRound: return "awaiting-peer-round";
		case Phase::Capturing:         return "capturing";
		case Phase::AwaitingPeer:      return "awaiting-peer";
		case Phase::Injecting:         return "injecting";
		case Phase::Fighting:          return "fighting";
		case Phase::Resolving:         return "resolving";
		case Phase::GameOver:          return "game-over";
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

	void Report()
	{
		LogInfo("probe: match phase=%s duel_round=%d completed=%d "
			"sync_run_start=%d hooks: end_of_round=%s run_start=%s",
			PhaseName(),
			g_DuelRound,
			g_CompletedDuelRound,
			g_SyncRunStart.load() ? 1 : 0,
			g_EndOfActTrampoline ? "yes" : "no",
			g_RunStartTrampoline ? "yes" : "no");
	}

	void Tick()
	{
		// Always drain the network queue, whatever phase we are in, so peer
		// messages are never left to go stale.
		std::string message;
		while (net::Poll(message))
			HandleMessage(message);

		// A run the peer asked for is started here, on the game's thread.
		DrainPendingRunStart();

		const Phase phase = g_Phase.load();

		// A dropped link ends the match. There is no resuming: matches are not
		// recoverable across a disconnect, so the state is torn down and the
		// next connection starts a fresh one with full lives. This also covers
		// GameOver, which must return to Offline or a later session would find
		// the phase stuck and never start.
		if (!net::IsConnected())
		{
			// Receiving from someone we are not connected to is not a
			// contradiction - draining the inbound queue deliberately does not
			// depend on our own link state - but it does mean the link is
			// one-way, and a one-way link is invisible from this end: everything
			// we can see about the opponent works, and nothing we send arrives.
			//
			// This is the shape of a real bug that took two playtests to find
			// (see the LobbyCreated_t note in Steam.cpp). Say it out loud.
			if (PeerPresenceFresh())
			{
				const auto now_check = std::chrono::steady_clock::now();
				if (now_check - g_LastPresenceNag >= kPresenceNagInterval)
				{
					g_LastPresenceNag = now_check;
					LogError("one-way link: %s's messages are arriving, but this "
						"client is not connected (link=%s) so nothing we send "
						"reaches them. They will see 'opponent ?'. Press F11 and "
						"reconnect.", PeerLabel().c_str(), net::StateName());
				}
			}

			if (phase != Phase::Offline)
			{
				if (phase != Phase::GameOver)
				{
					ui::Notify("Your opponent disconnected - the match is over "
						"(lives were %d to %d).", g_Lives.local, g_Lives.remote);
				}

				roster::SetDefaultWaveSuppressed(false);
				ResetMatchState();
				SetPhase(Phase::Offline, "peer link lost - match ended");
			}

			PushOverlay({});
			return;
		}

		if (phase == Phase::Offline)
		{
			// Every connection is a new match, by design.
			ResetMatchState();
			SetPhase(Phase::Idle, "peer link established - new match, 3 lives each");
			ui::Notify("Connected to %s. 3 lives each - duels every %d rounds.",
				PeerLabel().c_str(), runstate::DuelInterval());
			PushOverlay({});
			return;
		}

		if (phase == Phase::GameOver)
		{
			PushOverlay({});
			return;
		}

		// Tell the peer what we are doing, so their client can report it and
		// place our marker on their round track.
		const auto now = std::chrono::steady_clock::now();
		if (now - g_LastStatusSent >= kStatusSendInterval)
		{
			g_LastStatusSent = now;
			net::Send(BuildStatusMessage());
		}

		std::string banner;

		switch (phase)
		{
		case Phase::Idle:
		{
			const int round = runstate::CurrentRound();
			const bool tracked = runstate::RoundTrackingAvailable();

			// Two ways in, depending on what this build could resolve.
			//
			// With a real round number, the duel triggers on the duel round
			// itself, which is what replaces the boss fight.
			//
			// Without one, the act-end hook is the fallback: an act is exactly
			// one duel interval of rounds and ends in the boss, so firing on
			// the act boundary lands on the same fight. It is a round late
			// rather than at the start of the round, which is the cost of not
			// knowing the number.
			const bool round_trigger =
				tracked && round > 0 && round != g_CompletedDuelRound &&
				runstate::IsDuelRound(round);

			const bool act_trigger = !tracked && g_ActEnded.load();

			// The arena has to be populated, not merely the run in progress:
			// the duel captures an army immediately, and there is none to
			// capture from the ranch between rounds.
			if ((round_trigger || act_trigger) && ArenaIsPopulated())
			{
				g_ActEnded.store(false);
				g_DuelRound = round > 0 ? round : runstate::CurrentProgress();
				g_DuelProgress = runstate::CurrentProgress();
				g_SawInjectedEnemies = false;
				g_WaitStarted = now;
				g_LastWaveClear = {};

				// Counted per duel, so that calling one off can tell whether it
				// is this duel that emptied the arena.
				roster::ResetEnemiesCleared();

				HoldArenaEmpty(now);

				if (g_DuelRound > 0)
				{
					ui::Notify("Round %d - duel round. No boss: you fight %s.",
						g_DuelRound, PeerLabel().c_str());
				}
				else
				{
					ui::Notify("Duel round. No boss: you fight %s.",
						PeerLabel().c_str());
				}

				SetPhase(Phase::AwaitingPeerRound, "duel round reached");
				break;
			}

			// A round that is not a duel round clears the guard, so the next
			// one can trigger.
			if (tracked && round > 0 && !runstate::IsDuelRound(round))
				g_CompletedDuelRound = 0;

			// Back at the menu with a round still counted means the run ended -
			// abandoned, lost, or finished. Stop reporting a round, so the next
			// run starts counting from 1 rather than continuing.
			//
			// The extra condition is not decoration. new_run_start fires while
			// the player is still standing in rm_mainmenu, so for the frames
			// between "run started - round 1" and the room actually changing,
			// this read round > 0 in rm_mainmenu and immediately threw the count
			// away again. The log of it:
			//
			//     02:06:52  run started - round 1
			//     02:07:01  probe: round=0 (source: unresolved)
			//     02:07:20  a round finished before any run start was seen
			//
			// The count only came back when the first round ended, so for the
			// whole of round 1 the mod believed it was not in a run - and the
			// duel gate needs round > 0, which is why round 2 never triggered
			// one. Requiring that a run room has actually been seen since the
			// run started makes "back at the menu" mean what it says.
			if (round > 0 && !InRun() && runstate::HasBeenInRunRoom() &&
				bridge::CurrentRoomName() == "rm_mainmenu")
			{
				runstate::NoteRunEnded();
			}

			// Neither side can exchange anything until both are actually in a
			// run. Say so periodically rather than leaving the player guessing.
			if (now - g_LastPresenceNag >= kPresenceNagInterval)
			{
				const bool local_in_run = InRun();
				const bool peer_in_run = PeerPresenceFresh() && g_PeerPresence.in_run;

				if (local_in_run && !peer_in_run)
				{
					g_LastPresenceNag = now;
					LogInfo("waiting for your opponent to start a run - the duel "
						"happens when you both reach round %d",
						runstate::NextDuelRound(round));
				}
				else if (!local_in_run && peer_in_run)
				{
					g_LastPresenceNag = now;
					LogInfo("your opponent is in a run (round %d) - start yours "
						"to play against them", g_PeerPresence.round);
				}
			}

			// In round-tracked mode the act-end signal is not what triggers a
			// duel, so it is cleared rather than left to accumulate. In
			// fallback mode it IS the trigger, and it is deliberately left
			// pending until the arena has populated enough to act on - dropping
			// it here would lose the only signal that build gets.
			if (tracked)
				g_ActEnded.store(false);
			break;
		}

		case Phase::AwaitingPeerRound:
		{
			// The arena stays empty until the opponent arrives. Without this
			// the boss the duel replaces would fight the player while they
			// wait.
			HoldArenaEmpty(now);

			// Compared on progress rather than on round, because progress is
			// the one measure both clients can always produce - the opponent
			// may be a build, or a machine, where the round number never
			// resolved.
			const bool peer_ready =
				PeerPresenceFresh() &&
				g_PeerPresence.in_run &&
				g_PeerPresence.progress >= g_DuelProgress;

			if (peer_ready)
			{
				ui::Notify("%s is here - duel!", PeerLabel().c_str());
				SetPhase(Phase::Capturing, "both players at the duel round");
				break;
			}

			const auto waited = now - g_WaitStarted;

			if (waited > kPeerTimeout)
			{
				AbandonDuel("your opponent never reached this round");
				SetPhase(Phase::Idle, "duel gate timed out");
				break;
			}

			// The banner is the whole point of the gate being visible: an empty
			// arena with no explanation reads as a bug.
			char text[192]{};
			if (PeerPresenceFresh() && g_PeerPresence.in_run)
			{
				_snprintf_s(text, sizeof(text) - 1, _TRUNCATE,
					"WAITING FOR %s - ROUND %d OF %d",
					PeerLabel().c_str(),
					g_PeerPresence.progress, g_DuelProgress);
			}
			else
			{
				_snprintf_s(text, sizeof(text) - 1, _TRUNCATE,
					"WAITING FOR %s TO START A RUN", PeerLabel().c_str());
			}
			banner = text;

			if (now - g_LastWaitProgress >= kWaitProgressInterval)
			{
				g_LastWaitProgress = now;
				const auto remaining = std::chrono::duration_cast<
					std::chrono::seconds>(kPeerTimeout - waited).count();
				LogInfo("duel gate: %s (%llds before giving up)",
					banner.c_str(), static_cast<long long>(remaining));
			}
			break;
		}

		case Phase::Capturing:
		{
			// Halt normal progression while the exchange happens.
			roster::SetDefaultWaveSuppressed(true);
			HoldArenaEmpty(now);

			// The gate that led here can have been open for up to five minutes,
			// and this is the first thing after it that assumes the run is still
			// where it was. It need not be: the arena is being held empty, so
			// the game can decide the round is won and take the player back to
			// the ranch, and their army goes with it.
			//
			// That is not a hypothetical. It is what happened to the tester whose
			// log has "no live o_dude instances found - cannot capture army"
			// seventy-three seconds into a gate. Checking the room first means
			// that case is reported as what it is, rather than arriving as a
			// capture failure that reads like the roster walk is broken.
			if (!InGameplayRoom())
			{
				AbandonDuel("the round ended while waiting for your opponent");
				SetPhase(Phase::Idle, "left the arena during the gate");
				break;
			}

			if (!ArenaIsPopulated())
			{
				AbandonDuel("your army was gone by the time the duel started");
				SetPhase(Phase::Idle, "arena emptied during the gate");
				break;
			}

			if (!roster::CaptureLocalArmy(g_LocalArmy))
			{
				// Nothing to send. Let the round proceed vanilla rather than
				// stalling the run.
				AbandonDuel("your army could not be captured");
				SetPhase(Phase::Idle, "army capture failed");
				break;
			}

			g_LocalArmy.lives = g_Lives.local;
			g_LocalArmy.act = runstate::CurrentAct();

			const std::string payload = BuildArmyMessage(g_LocalArmy);
			if (payload.empty() || !net::Send(payload))
			{
				AbandonDuel("your army could not be sent");
				SetPhase(Phase::Idle, "could not transmit army");
				break;
			}

			LogStage(kStageSerialize, "transmitted %zu byte army payload",
				payload.size());

			g_WaitStarted = now;

			// The peer's army may already have arrived while we were capturing.
			SetPhase(
				g_PeerArmy.units.empty() && g_PeerArmy.matchup.empty()
					? Phase::AwaitingPeer
					: Phase::Injecting,
				"local army sent"
			);
			break;
		}

		case Phase::AwaitingPeer:
		{
			HoldArenaEmpty(now);

			const auto waited = now - g_WaitStarted;

			if (waited > kPeerTimeout)
			{
				AbandonDuel("your opponent's army never arrived");
				SetPhase(Phase::Idle, "peer timeout");
				break;
			}

			banner = "EXCHANGING ARMIES...";

			if (now - g_LastWaitProgress >= kWaitProgressInterval)
			{
				g_LastWaitProgress = now;
				const auto elapsed = std::chrono::duration_cast<
					std::chrono::seconds>(waited).count();
				LogInfo("waiting for your opponent's army (%llds elapsed)",
					static_cast<long long>(elapsed));
			}
			break;
		}

		case Phase::Injecting:
		{
			roster::ClearDefaultEnemyWave();
			g_InjectedCount = roster::InjectOpponentArmy(g_PeerArmy);

			if (g_InjectedCount <= 0)
			{
				AbandonDuel("your opponent's army could not be spawned");
				SetPhase(Phase::Idle, "injection produced no units");
				break;
			}

			g_ResultSent = false;
			g_SawInjectedEnemies = false;
			g_InjectedAt = now;

			ui::Notify("%s's army of %d is in the arena. Fight!",
				PeerLabel().c_str(), g_InjectedCount);

			SetPhase(Phase::Fighting, "opponent wave live");
			break;
		}

		case Phase::Fighting:
		{
			Outcome outcome = EvaluateBattle();
			if (outcome == Outcome::Undecided)
				break;

			ApplyOutcome(outcome == Outcome::LocalWon);
			SetPhase(Phase::Resolving, "battle decided");
			break;
		}

		case Phase::Resolving:
		{
			// Wait for the peer's own result so both clients agree on the score
			// before deciding whether the run continues.
			if (!g_PeerResultReceived)
			{
				banner = "WAITING FOR THEIR RESULT...";
				break;
			}

			LogStage(kStageResolve, "lives - local %d, remote %d",
				g_Lives.local, g_Lives.remote);

			if (g_Lives.local <= 0 || g_Lives.remote <= 0)
			{
				ui::Notify("%s", g_Lives.local <= 0
					? "You are out of lives. Match over."
					: "Your opponent is out of lives. You win the match!");

				roster::SetDefaultWaveSuppressed(false);
				SetPhase(Phase::GameOver, "a player reached zero lives");
				break;
			}

			// Lives remain on both sides: carry on with the run.
			g_CompletedDuelRound = g_DuelRound;
			g_DuelRound = 0;
			g_PeerArmy = roster::Snapshot{};
			g_LocalArmy = roster::Snapshot{};
			g_PeerResultReceived = false;
			g_ResultSent = false;
			g_InjectedCount = 0;
			g_SawInjectedEnemies = false;
			roster::SetDefaultWaveSuppressed(false);

			ui::Notify("Lives: you %d, %s %d. Next duel at round %d.",
				g_Lives.local, PeerLabel().c_str(), g_Lives.remote,
				runstate::NextDuelRound(g_CompletedDuelRound + 1));

			SetPhase(Phase::Idle, "duel resolved - run continues");
			break;
		}

		default:
			break;
		}

		PushOverlay(banner);
	}
}
