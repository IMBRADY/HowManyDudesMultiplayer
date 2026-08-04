// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RunState.h"
#include "DuelSchedule.h"
#include "GameBridge.h"
#include "GameHooks.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

using namespace YYTK;
using namespace Aurie;

namespace hmd::runstate
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		// Scripts observed for run position. Names confirmed against data.win.
		constexpr const char* kRoundNumberScript = "gml_Script_round_number_frame";
		constexpr const char* kRunMapCellScript = "gml_Script_run_map_cell";
		constexpr const char* kVisualBoxScript = "gml_Script_cakeframe_get_visual_box";
		// Deliberately not used. cf_get_box aborts the game when handed
		// anything that is not a fully laid-out cakeframe, and it was the
		// fallback that did exactly that. Named here so nobody reinstates it
		// without reading why it went.
		// constexpr const char* kBoxScript = "gml_Script_cf_get_box";

		// Deliberately not called either, for the same class of reason. This one
		// resolves and looks harmless - it is a getter, by every reading of its
		// name - but it calls gold_star_round internally, and that aborts the
		// game with "I32 argument is undefined" when it is entered from a mod
		// tick rather than from the game's own progression. A tester's game died
		// on it twice inside twenty seconds. The act is now derived from the
		// round instead; see duel::ActFromRound. The name is kept only so the
		// probe can report whether the build still has it.
		constexpr const char* kUnsafeActScript = "gml_Script_get_act_number";

		// A round number read from a UI hook is only believed for as long as
		// that UI is plausibly still on screen. Past that we fall back down the
		// ladder rather than reporting a number from two rooms ago.
		constexpr auto kRoundFreshFor = std::chrono::seconds(3);

		// Run-map cells are re-recorded every frame the map is up. Anything not
		// refreshed within this window is dropped, so a stale cell cannot leave
		// a marker floating over a map that is no longer drawn.
		constexpr auto kCellFreshFor = std::chrono::milliseconds(400);

		// Sanity bounds for a round number picked out of an unknown signature.
		constexpr int kMinRound = 1;
		constexpr int kMaxRound = 500;

		std::atomic<int> g_DuelInterval{ duel::kDefaultInterval };

		// The one round whose cell is measured. See SetTrackedRound.
		std::atomic<int> g_TrackedRound{ 0 };

		// CurrentRound is asked several times a tick and, when the display hook
		// has not fired, each answer costs a walk of the candidate globals.
		// Memoised for well under a frame so repeated asks are free without the
		// value ever being visibly stale.
		constexpr auto kRoundCacheFor = std::chrono::milliseconds(100);
		std::mutex g_CacheMutex;
		int g_CachedRound = 0;
		clock::time_point g_CachedRoundAt;

		std::mutex g_Mutex;

		int g_HookRound = 0;
		clock::time_point g_HookRoundAt;

		struct Cell
		{
			Box box;
			clock::time_point at;
		};

		std::map<int, Cell> g_Cells;

		Box g_DrawSpace;
		clock::time_point g_DrawSpaceAt;

		// Which rung of the ladder answered last, for the report.
		const char* g_RoundSource = "unresolved";

		// Latched once a real round number has been seen. The duel gate uses
		// this to choose between round-accurate triggering and the act-based
		// fallback, and latching means one unlucky read between rooms does not
		// flip the whole mod into fallback mode.
		std::atomic<bool> g_RoundEverResolved{ false };

		// The counted round. Maintained from the run-start and round-finish
		// hooks, which is the only source on this build that actually works.
		std::atomic<int> g_CountedRound{ 0 };

		// Whether a room that only exists during a run has been seen since the
		// current run started. See HasBeenInRunRoom.
		std::atomic<bool> g_SeenRunRoom{ false };

		// --- Hook state ---------------------------------------------------
		PFUNC_YYGMLScript g_RoundNumberTrampoline = nullptr;
		PFUNC_YYGMLScript g_RunMapCellTrampoline = nullptr;

		// -----------------------------------------------------------------
		// Box reading
		// -----------------------------------------------------------------
		//
		// cakeframe_get_visual_box's return shape is not knowable statically -
		// YYC strips the code that builds it. It is either a struct with named
		// edges or a four-element array, and both are handled rather than one
		// being assumed. A shape we do not recognise reports "no box" and the
		// overlay simply does not draw, which is the correct failure.
		bool ReadBoxFromArray(const RValue& Value, Box& Out)
		{
			double edges[4]{};

			for (int i = 0; i < 4; i++)
			{
				auto element = bridge::CallBuiltin(
					"array_get",
					{ Value, RValue(static_cast<double>(i)) }
				);

				if (!element)
					return false;

				if (element->m_Kind != VALUE_REAL &&
					element->m_Kind != VALUE_INT32 &&
					element->m_Kind != VALUE_INT64)
					return false;

				edges[i] = element->ToDouble();
			}

			Out.left = edges[0];
			Out.top = edges[1];
			Out.right = edges[2];
			Out.bottom = edges[3];
			Out.valid = true;
			return true;
		}

		bool ReadBoxFromStruct(const RValue& Value, Box& Out)
		{
			// Two naming conventions, tried in order.
			static const char* kEdgeNames[2][4] = {
				{ "left", "top", "right", "bottom" },
				{ "x1", "y1", "x2", "y2" },
			};

			for (const auto& names : kEdgeNames)
			{
				auto left = bridge::GetMember(Value, names[0]);
				auto top = bridge::GetMember(Value, names[1]);
				auto right = bridge::GetMember(Value, names[2]);
				auto bottom = bridge::GetMember(Value, names[3]);

				if (!left || !top || !right || !bottom)
					continue;

				Out.left = left->ToDouble();
				Out.top = top->ToDouble();
				Out.right = right->ToDouble();
				Out.bottom = bottom->ToDouble();
				Out.valid = true;
				return true;
			}

			return false;
		}

		// A box is only useful if it describes a real rectangle. Degenerate or
		// non-finite values mean the frame has not been laid out yet.
		bool BoxIsPlausible(const Box& Value)
		{
			if (!std::isfinite(Value.left) || !std::isfinite(Value.top) ||
				!std::isfinite(Value.right) || !std::isfinite(Value.bottom))
				return false;

			return Value.Width() > 1.0 && Value.Height() > 1.0;
		}

		// There was a ReadIntScript helper here - call a no-argument game script,
		// believe it if it hands back a finite number. Its only caller was the
		// act lookup, and that lookup is what crashed a tester's game. A generic
		// "call any script and read the result" helper is how an unsafe call gets
		// added without anyone weighing whether that particular script is safe to
		// call, so it went with its caller rather than being left to invite the
		// next one.

		// Globals the round might live in. YYC strips VARI, so this is a probe
		// list in the same spirit as Roster.cpp's field candidates.
		int ReadRoundFromGlobals()
		{
			static const char* kCandidates[] = {
				"round", "current_round", "round_number", "round_num",
				"wave", "current_wave", "run_round"
			};

			for (const char* name : kCandidates)
			{
				auto value = bridge::GetGlobal(name);
				if (!value)
					continue;

				if (value->m_Kind != VALUE_REAL && value->m_Kind != VALUE_INT32 &&
					value->m_Kind != VALUE_INT64)
					continue;

				const double raw = value->ToDouble();
				if (!std::isfinite(raw))
					continue;

				const int round = static_cast<int>(raw);
				if (round >= kMinRound && round <= kMaxRound)
					return round;
			}

			return 0;
		}

		// The round resolution ladder, behind CurrentRound's cache.
		//
		// There is deliberately no rung that derives a round from the act. Such
		// a value is an exact multiple of the duel interval every single time,
		// and the duel gate reads exactly that to decide when to fire - so a
		// derived value would mean a permanent duel. Callers that need a number
		// regardless use CurrentProgress, which is explicit about estimating.
		int ResolveRound()
		{
			// 1. The count kept from the run-start and round-finish hooks.
			//    First because it is the only source that has ever worked on
			//    this build, and it is exact rather than inferred.
			const int counted = g_CountedRound.load();
			if (counted > 0)
			{
				g_RoundSource = "counted from round-finish hook";
				g_RoundEverResolved.store(true);
				return counted;
			}

			// 2. What the round display was told to draw. Kept because a build
			//    where round_number_frame does take its round as an argument
			//    would be strictly better served by it - on this one it takes
			//    none, so this never fires.
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				if (g_HookRound > 0 &&
					(clock::now() - g_HookRoundAt) < kRoundFreshFor)
				{
					g_RoundSource = "round_number_frame hook";
					g_RoundEverResolved.store(true);
					return g_HookRound;
				}
			}

			// 3. A global holding it under one of the plausible names.
			const int from_global = ReadRoundFromGlobals();
			if (from_global > 0)
			{
				g_RoundSource = "global variable";
				g_RoundEverResolved.store(true);
				return from_global;
			}

			g_RoundSource = "unresolved";
			return 0;
		}

		// -----------------------------------------------------------------
		// Detours
		// -----------------------------------------------------------------
		RValue& RoundNumberDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue* Arguments[]
		)
		{
			int round = 0;
			if (hooks::FindIntegerArgument(ArgumentCount, Arguments,
					kMinRound, kMaxRound, round))
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				g_HookRound = round;
				g_HookRoundAt = clock::now();
			}

			if (!g_RoundNumberTrampoline)
			{
				Result = RValue();
				return Result;
			}

			RValue& returned = g_RoundNumberTrampoline(
				Self, Other, Result, ArgumentCount, Arguments);

			HMD_LOG_SIGNATURE_ONCE(kRoundNumberScript, ArgumentCount,
				Arguments, &returned);

			return returned;
		}

		RValue& RunMapCellDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue* Arguments[]
		)
		{
			if (!g_RunMapCellTrampoline)
			{
				Result = RValue();
				return Result;
			}

			// The original runs first: we want the cakeframe it produces, and
			// observing must never change what the game builds.
			RValue& returned = g_RunMapCellTrampoline(
				Self, Other, Result, ArgumentCount, Arguments);

			HMD_LOG_SIGNATURE_ONCE(kRunMapCellScript, ArgumentCount,
				Arguments, &returned);

			// Only the opponent's cell is measured. This detour runs for every
			// cell on the map every frame, and measuring one means calling back
			// into the game - so measuring all of them to place a single sprite
			// would cost hundreds of script calls a second for nothing.
			// Which link in this chain fails is the open question behind the
			// opponent marker never appearing. Every early return below is a
			// different answer, and from outside they all look identical - the
			// cell simply is not there. So each one is named, once every ten
			// seconds and immediately whenever the answer changes.
			//
			// This runs for every cell on the map every frame. Everything here
			// is cheap and nothing is logged on the happy path.
			auto note = [](const char* What)
			{
				static const char* last = nullptr;
				static clock::time_point at{};

				const auto now = clock::now();
				if (What == last && (now - at) < std::chrono::seconds(10))
					return;

				last = What;
				at = now;
				LogInfo("run map: %s", What);
			};

			const int tracked = g_TrackedRound.load(std::memory_order_relaxed);
			if (tracked <= 0)
			{
				note("no opponent round is being tracked, so no cell is measured");
				return returned;
			}

			int round = 0;
			if (!hooks::FindIntegerArgument(ArgumentCount, Arguments,
					kMinRound, kMaxRound, round))
			{
				note("could not find a round number in run_map_cell's arguments");
				return returned;
			}

			if (round != tracked)
			{
				// Expected for most cells - only one matches. Logged anyway
				// because "the map never draws the opponent's cell at all" and
				// "it draws it and we reject it" are different problems, and
				// the numbers tell them apart.
				static int last_seen = 0;
				if (round != last_seen)
				{
					last_seen = round;
					LogInfo("run map: drew cell for round %d, waiting for %d",
						round, tracked);
				}
				return returned;
			}

			Box box;
			if (!ReadCakeframeBox(returned, box))
			{
				note("the opponent's cell was drawn but its box could not be read");
				return returned;
			}

			if (!BoxIsPlausible(box))
			{
				LogInfo("run map: the opponent's cell box was rejected as "
					"implausible: %.0f,%.0f..%.0f,%.0f",
					box.left, box.top, box.right, box.bottom);
				return returned;
			}

			note("measuring the opponent's cell");

			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Cells[round] = Cell{ box, clock::now() };
			return returned;
		}
	}

	// ---------------------------------------------------------------------
	// Public surface
	// ---------------------------------------------------------------------
	bool Initialize(AurieModule* Module)
	{
		// Both hooks are observational and both are optional. Losing either
		// costs a feature, not the mod: without round_number_frame the round
		// falls back down the ladder, and without run_map_cell the opponent
		// marker simply is not drawn.
		g_RoundNumberTrampoline = hooks::Install(
			Module, "hmd_round_number", kRoundNumberScript, &RoundNumberDetour);

		if (!g_RoundNumberTrampoline)
		{
			LogWarn("round display hook unavailable - the current round will be "
				"inferred from the act number instead");
		}

		g_RunMapCellTrampoline = hooks::Install(
			Module, "hmd_run_map_cell", kRunMapCellScript, &RunMapCellDetour);

		if (!g_RunMapCellTrampoline)
		{
			LogWarn("run map hook unavailable - your opponent's position will "
				"not be marked on the round track");
		}

		return true;
	}

	void Shutdown(AurieModule* Module)
	{
		if (g_RoundNumberTrampoline)
		{
			hooks::Remove(Module, "hmd_round_number");
			g_RoundNumberTrampoline = nullptr;
		}

		if (g_RunMapCellTrampoline)
		{
			hooks::Remove(Module, "hmd_run_map_cell");
			g_RunMapCellTrampoline = nullptr;
		}

		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Cells.clear();
	}

	void Tick()
	{
		const auto now = clock::now();

		std::lock_guard<std::mutex> lock(g_Mutex);

		for (auto it = g_Cells.begin(); it != g_Cells.end(); )
		{
			if (now - it->second.at > kCellFreshFor)
				it = g_Cells.erase(it);
			else
				++it;
		}

		if (now - g_DrawSpaceAt > kCellFreshFor)
			g_DrawSpace.valid = false;
	}

	bool ReadCakeframeBox(const RValue& Frame, Box& Out)
	{
		// Accepts the kinds a cakeframe actually arrives as, which includes
		// VALUE_REF.
		//
		// The history matters, because this check was VALUE_OBJECT-only on
		// purpose. The draw hooks used to hand this whatever their first
		// argument happened to be; a sprite reference is a VALUE_REF, and
		// feeding one to cf_get_box aborted the game the moment a peer
		// connected. Narrowing the kind was the fix.
		//
		// It was the wrong fix for the right problem. What made that crash
		// possible was the CALLER passing an arbitrary argument, and that caller
		// is gone - cf_get_box is no longer used at all, and the only route in
		// here now is RunMapCellDetour passing run_map_cell's own return value.
		// The kind check was doing a caller's job, and it was also rejecting the
		// real thing: this runner returns cakeframes as VALUE_REF, exactly as it
		// returns instances from instance_find. Logged as:
		//
		//     run map: the opponent's cell was drawn but its box could not be read
		//
		// which is the whole reason the opponent marker has never appeared.
		const bool usable_kind =
			Frame.m_Kind == VALUE_OBJECT ||
			Frame.m_Kind == VALUE_REF ||
			Frame.m_Kind == VALUE_PTR;

		if (!usable_kind || !Frame.m_Pointer)
		{
			// Named once, so a kind nobody predicted does not become another
			// silent rejection.
			static int last_rejected = -1;
			if (static_cast<int>(Frame.m_Kind) != last_rejected)
			{
				last_rejected = static_cast<int>(Frame.m_Kind);
				LogInfo("run map: cakeframe arrived as kind %d, which is not "
					"something a box can be read from", last_rejected);
			}
			return false;
		}

		// visual_box only. cf_get_box was the fallback and it is the one that
		// crashed; a second accessor that can abort the game is not worth the
		// marginal chance of a box the first one could not produce.
		//
		// The kind check above now passes and the read still fails, so the
		// failure is somewhere in here - and this had exactly one outcome for
		// four different causes. Each step is named once, the same way the
		// detour above is, because "could not be read" has already cost two
		// sessions of guessing.
		auto note = [](const char* What)
		{
			static const char* last = nullptr;
			if (What == last)
				return;

			last = What;
			LogInfo("run map: %s", What);
		};

		for (const char* script : { kVisualBoxScript })
		{
			if (!bridge::ScriptExists(script))
			{
				note("cakeframe_get_visual_box does not exist on this build");
				continue;
			}

			RValue result;
			if (!bridge::CallScriptAnnounced(script, { Frame }, result))
			{
				note("cakeframe_get_visual_box could not be called");
				continue;
			}

			if (result.m_Kind == VALUE_ARRAY)
			{
				if (ReadBoxFromArray(result, Out))
					return true;

				note("visual_box returned an array no box could be read from");
			}
			else if (result.m_Kind == VALUE_OBJECT || result.m_Kind == VALUE_REF)
			{
				if (ReadBoxFromStruct(result, Out))
					return true;

				note("visual_box returned a struct with no box fields in it - "
					"press F6 in the ranch to dump what it does have");

				// The names are the whole problem, so dump them once. This is
				// the same trick that is about to name o_dude's members, and it
				// is the only way to learn field names on a YYC build.
				static bool dumped = false;
				if (!dumped)
				{
					dumped = true;
					bridge::LogInstanceMembers(result, "visual_box result");
				}
			}
			else
			{
				static int last_kind = -1;
				if (static_cast<int>(result.m_Kind) != last_kind)
				{
					last_kind = static_cast<int>(result.m_Kind);
					LogInfo("run map: visual_box returned kind %d, which is "
						"neither an array nor a struct", last_kind);
				}
			}
		}

		return false;
	}

	int CurrentRound()
	{
		{
			std::lock_guard<std::mutex> lock(g_CacheMutex);
			if (g_CachedRoundAt.time_since_epoch().count() != 0 &&
				(clock::now() - g_CachedRoundAt) < kRoundCacheFor)
				return g_CachedRound;
		}

		const int resolved = ResolveRound();

		std::lock_guard<std::mutex> lock(g_CacheMutex);
		g_CachedRound = resolved;
		g_CachedRoundAt = clock::now();
		return resolved;
	}

	void SetTrackedRound(int Round)
	{
		g_TrackedRound.store(Round > 0 ? Round : 0);
	}

	bool RoundTrackingAvailable()
	{
		return g_RoundEverResolved.load();
	}

	void NoteInRunRoom()
	{
		g_SeenRunRoom.store(true);
	}

	bool HasBeenInRunRoom()
	{
		return g_SeenRunRoom.load();
	}

	void NoteRunStarted()
	{
		// Cleared here, not in NoteRunEnded: the point of the flag is to say
		// whether a run room has been seen since THIS run started, and at this
		// moment the player is still in the menu the run was started from.
		g_SeenRunRoom.store(false);

		g_CountedRound.store(1);
		g_RoundEverResolved.store(true);

		// The cached value is from the previous run, or from before there was
		// one. Drop it so the new round is visible immediately rather than up
		// to a tenth of a second later.
		std::lock_guard<std::mutex> lock(g_CacheMutex);
		g_CachedRoundAt = {};

		LogInfo("run started - round 1");
	}

	void NoteRoundFinished()
	{
		// A round finishing before a run has been seen to start means the mod
		// loaded mid-run, or the run-start hook did not fire. Counting from 1
		// would be wrong, but refusing to count at all would disable the duel
		// entirely - so start counting from here and say so.
		int round = g_CountedRound.load();
		if (round <= 0)
		{
			LogWarn("a round finished before any run start was seen - counting "
				"rounds from here, so the first duel may be off");
			round = 1;
		}

		g_CountedRound.store(round + 1);
		g_RoundEverResolved.store(true);

		std::lock_guard<std::mutex> lock(g_CacheMutex);
		g_CachedRoundAt = {};
	}

	void NoteRunEnded()
	{
		g_CountedRound.store(0);

		std::lock_guard<std::mutex> lock(g_CacheMutex);
		g_CachedRoundAt = {};
	}

	int CurrentProgress()
	{
		// Nothing but the round, now. There used to be an act-derived fallback
		// for builds where the round never resolved, and it was worth having
		// right up until the script it rested on turned out to be able to abort
		// the game - at which point a fallback that crashes is worse than no
		// fallback at all. On this build the round resolves from the round-
		// finish hook anyway, which is the rung that has always done the work.
		//
		// Zero means "no idea", and every caller already treats it that way.
		const int round = CurrentRound();
		return round > 0 ? round : 0;
	}

	int CurrentAct()
	{
		// Derived, never asked. See kUnsafeActScript.
		return duel::ActFromRound(CurrentRound(), DuelInterval());
	}

	int DuelInterval()
	{
		return g_DuelInterval.load();
	}

	void SetDuelInterval(int Rounds)
	{
		if (!duel::IsValidInterval(Rounds))
		{
			LogWarn("ignoring out-of-range duel interval %d - keeping %d",
				Rounds, g_DuelInterval.load());
			return;
		}

		g_DuelInterval.store(Rounds);
	}

	bool IsDuelRound(int Round)
	{
		return duel::IsDuelRound(Round, DuelInterval());
	}

	int NextDuelRound(int Round)
	{
		return duel::NextDuelRound(Round, DuelInterval());
	}

	bool CellBox(int Round, Box& Out)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);

		auto found = g_Cells.find(Round);
		if (found == g_Cells.end())
			return false;

		if (clock::now() - found->second.at > kCellFreshFor)
			return false;

		Out = found->second.box;
		return true;
	}

	bool DrawSpace(Box& Out)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);

		if (!g_DrawSpace.valid)
			return false;

		Out = g_DrawSpace;
		return true;
	}

	void NoteDrawnFrame(const Box& Frame)
	{
		if (!BoxIsPlausible(Frame))
			return;

		std::lock_guard<std::mutex> lock(g_Mutex);

		const auto now = clock::now();

		// Reset rather than accumulate once the previous observation has aged
		// out, so a resolution change or a room change is picked up instead of
		// leaving the overlay anchored to a screen that no longer exists.
		if (!g_DrawSpace.valid || (now - g_DrawSpaceAt) > kCellFreshFor)
		{
			g_DrawSpace = Frame;
			g_DrawSpaceAt = now;
			return;
		}

		g_DrawSpace.left = (std::min)(g_DrawSpace.left, Frame.left);
		g_DrawSpace.top = (std::min)(g_DrawSpace.top, Frame.top);
		g_DrawSpace.right = (std::max)(g_DrawSpace.right, Frame.right);
		g_DrawSpace.bottom = (std::max)(g_DrawSpace.bottom, Frame.bottom);
		g_DrawSpace.valid = true;
		g_DrawSpaceAt = now;
	}

	void Report()
	{
		const int round = CurrentRound();

		LogInfo("probe: round=%d (source: %s) tracking=%s progress=%d act=%d "
			"duel every %d rounds",
			round, g_RoundSource,
			RoundTrackingAvailable() ? "available" : "UNAVAILABLE (no duel gate)",
			CurrentProgress(), CurrentAct(), DuelInterval());

		// Report only whether the candidate accessors exist. Emphatically do
		// NOT call them to see what they return.
		//
		// An earlier version of this did exactly that, and it crashed the game
		// on startup: gml_Script_wave_real is a constructor, not a getter, and
		// invoking it directly raises "constructors should only be called using
		// new". A name is not evidence of what a routine is, and this build has
		// no bytecode to check against - so a diagnostic must never be the
		// thing that runs unknown game code. Anything actually called from here
		// is either a builtin or a routine the mod already depends on.
		for (const char* script : {
				"gml_Script_wave_real",
				"gml_Script_get_round_to_jump_to",
				"gml_Script_round_has_boss",
				"gml_Script_jump_to_round",
				kUnsafeActScript })
		{
			LogInfo("probe:   %s - %s", script,
				bridge::ScriptExists(script) ? "present" : "not present");
		}

		std::lock_guard<std::mutex> lock(g_Mutex);

		LogInfo("probe: %zu run-map cell(s) tracked", g_Cells.size());
		for (const auto& entry : g_Cells)
		{
			LogInfo("probe:   round %-3d box %.0f,%.0f .. %.0f,%.0f",
				entry.first,
				entry.second.box.left, entry.second.box.top,
				entry.second.box.right, entry.second.box.bottom);
		}

		if (g_DrawSpace.valid)
		{
			LogInfo("probe: draw space %.0f,%.0f .. %.0f,%.0f",
				g_DrawSpace.left, g_DrawSpace.top,
				g_DrawSpace.right, g_DrawSpace.bottom);
		}
		else
		{
			LogInfo("probe: draw space not observed - the cf_draw hook has not "
				"delivered a frame");
		}
	}
}
