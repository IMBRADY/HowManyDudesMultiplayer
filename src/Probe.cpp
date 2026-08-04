// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Probe.h"
#include "GameBridge.h"
#include "Log.h"
#include "Match.h"
#include "Roster.h"
#include "RunState.h"
#include "Ui.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>

using namespace YYTK;

namespace hmd::probe
{
	namespace
	{
		// How long the census runs before it stops accepting new names. Long
		// enough to cover a room transition and a few hundred frames, short
		// enough that the per-call set lookup is not paid for the whole
		// session.
		constexpr auto kCensusWindow = std::chrono::seconds(20);

		// A hard ceiling on distinct names, so a runner that hands us a fresh
		// name per call cannot grow this without bound.
		constexpr size_t kMaxCensusEntries = 512;

		std::atomic<bool> g_CensusOpen{ true };
		std::atomic<bool> g_Reported{ false };

		// Safe-window statistics. Separate from the census because these never
		// stop counting - see NoteSafeWindow.
		std::atomic<uint64_t> g_WindowEntries{ 0 };
		std::atomic<uint64_t> g_WindowSafe{ 0 };
		std::atomic<int> g_DeepestNesting{ 0 };
		std::atomic<uint64_t> g_DepthResets{ 0 };

		std::mutex g_Mutex;
		std::set<const void*> g_CodeEntries;
		uint64_t g_CodeCallbacks = 0;
		uint64_t g_NullCodeEntries = 0;

		std::chrono::steady_clock::time_point g_Started;

		// Naming a code entry needs YYTKPrivateInterface::CCode_GetName, which
		// is not reachable from the public interface this mod holds. So the
		// census counts rather than names: how busy the callback is, and how
		// many distinct entries are behind it.
		//
		// That is enough to answer the question that mattered. Because entries
		// cannot be named, an object's Draw event cannot be identified and
		// therefore cannot be used as a draw hook - which is why the overlay
		// hangs off cf_draw, a top-level script that IS resolvable by name.
		void ReportCodeCensus()
		{
			std::lock_guard<std::mutex> lock(g_Mutex);

			LogInfo("probe: code callback fired %llu time(s) across %zu distinct "
				"entries (%llu with no code object)",
				static_cast<unsigned long long>(g_CodeCallbacks),
				g_CodeEntries.size(),
				static_cast<unsigned long long>(g_NullCodeEntries));

			if (g_CodeCallbacks == 0)
			{
				LogWarn("probe: the code callback never fired - the mod's tick "
					"is not running and nothing else here will work");
			}
		}

		// The health of the safe-window gate, which is the thing standing
		// between the mod and the crash that motivated it.
		void ReportSafeWindow()
		{
			const uint64_t total = g_WindowEntries.load(std::memory_order_relaxed);
			const uint64_t safe = g_WindowSafe.load(std::memory_order_relaxed);
			const uint64_t resets = g_DepthResets.load(std::memory_order_relaxed);

			// "Depth" throughout is the number of code entries already open when
			// this one arrived, so depth 0 is a top-level entry and is the only
			// one the mod will talk to the game from.
			LogInfo("probe: safe window - %llu of %llu code entries at depth 0 "
				"(%.1f%%), deepest %d enclosing entries, depth resets %llu",
				static_cast<unsigned long long>(safe),
				static_cast<unsigned long long>(total),
				total ? (100.0 * static_cast<double>(safe) /
					static_cast<double>(total)) : 0.0,
				g_DeepestNesting.load(std::memory_order_relaxed),
				static_cast<unsigned long long>(resets));

			if (total > 0 && safe == 0)
			{
				LogWarn("probe: NO safe window has ever opened on this build - "
					"the mod cannot call into the game at all, so duels and the "
					"overlay are dead. Steam and the hotkeys still work.");
			}

			if (resets > 0)
			{
				LogWarn("probe: the nesting counter has been force-reset %llu "
					"time(s) - the runtime is leaving the mod's frame without "
					"returning through it, which is what a GML abort does",
					static_cast<unsigned long long>(resets));
			}
		}

		void ReportGlobals()
		{
			CInstance* global = bridge::GlobalInstance();
			if (!global)
			{
				LogWarn("probe: global scope unavailable - cannot list globals");
				return;
			}

			// Search, do not dump. The global scope holds thousands of members
			// and an alphabetical listing never reaches the letter R, which is
			// where every name worth having happens to live.
			bridge::LogMatchingMembers(
				RValue(global),
				"global",
				{ "round", "wave", "act", "run", "stage", "level", "depth" }
			);
		}

		// The gameplay controller is the other place the round could live.
		void ReportGameplayMembers()
		{
			std::vector<RValue> gameplay = bridge::FindInstances("o_gameplay");
			if (gameplay.empty())
				return;

			bridge::LogMatchingMembers(
				gameplay.front(),
				"o_gameplay",
				{ "round", "wave", "act", "stage" }
			);
		}

		void ReportGameplayObject()
		{
			std::vector<RValue> gameplay = bridge::FindInstances("o_gameplay");
			if (gameplay.empty())
			{
				LogInfo("probe: o_gameplay not instantiated (not in a run) - "
					"re-run the probe from inside a round to list its members");
				return;
			}

			bridge::LogInstanceMembers(gameplay.front(), "o_gameplay");
		}

		// The member names of the combatant objects.
		//
		// This is the single most valuable thing the probe can produce and until
		// now it was unreachable: the only dump of o_dude lived inside
		// CaptureLocalArmy, which only runs during a duel, which needs a second
		// player and a duel round. The names it would print are what every
		// "field 'x' did not resolve to any known member name" warning is asking
		// for, and they cannot be recovered from data.win because this is a YYC
		// build with no VARI chunk.
		//
		// So it is on F6 instead, where one person alone standing in a fight can
		// produce it.
		void ReportCombatantMembers()
		{
			for (const char* object : { "o_dude", "o_enemy" })
			{
				std::vector<RValue> instances = bridge::FindInstances(object);

				if (instances.empty())
				{
					LogInfo("probe: no live %s to inspect - press F6 during a "
						"fight, not in the ranch or the menu", object);
					continue;
				}

				LogInfo("probe: %zu live %s instance(s); listing the first",
					instances.size(), object);

				bridge::LogInstanceMembers(instances.front(), object);
			}

			// Only meaningful mid-round, and it is the one remaining route to
			// an army now that instance members have proved unreadable.
			if (bridge::CurrentRoomName() == "rm_gameplay")
				roster::ProbeNativeExport();
		}
	}

	void NoteCodeEntry(CCode* Code)
	{
		if (!g_CensusOpen.load(std::memory_order_relaxed))
			return;

		std::lock_guard<std::mutex> lock(g_Mutex);

		g_CodeCallbacks++;

		if (!Code)
		{
			g_NullCodeEntries++;
			return;
		}

		// Pointers only - never dereferenced. The set is bounded so a runner
		// that hands back a fresh object every call cannot grow it without
		// limit.
		if (g_CodeEntries.size() < kMaxCensusEntries)
			g_CodeEntries.insert(static_cast<const void*>(Code));
	}

	bool CensusOpen()
	{
		return g_CensusOpen.load(std::memory_order_relaxed);
	}

	void NoteSafeWindow(int Depth, bool Safe)
	{
		// Called on every single code entry - thousands a second by the census's
		// own numbers - so this is relaxed atomics and nothing else. No lock, no
		// allocation, and emphatically no call into the game.
		g_WindowEntries.fetch_add(1, std::memory_order_relaxed);

		if (Safe)
			g_WindowSafe.fetch_add(1, std::memory_order_relaxed);

		int deepest = g_DeepestNesting.load(std::memory_order_relaxed);
		while (Depth > deepest &&
			!g_DeepestNesting.compare_exchange_weak(deepest, Depth,
				std::memory_order_relaxed))
		{
		}
	}

	void NoteDepthReset()
	{
		g_DepthResets.fetch_add(1, std::memory_order_relaxed);
	}

	void Report()
	{
		LogInfo("probe: ===== runtime discovery report =====");
		LogInfo("probe: room=%s", bridge::CurrentRoomName().c_str());

		ReportCodeCensus();
		ReportSafeWindow();
		ui::Report();
		runstate::Report();
		match::Report();
		ReportGameplayObject();
		ReportCombatantMembers();
		ReportGameplayMembers();
		ReportGlobals();

		LogInfo("probe: ===== end of report =====");
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();

		if (g_Started.time_since_epoch().count() == 0)
			g_Started = now;

		if (g_CensusOpen.load() && (now - g_Started) >= kCensusWindow)
		{
			g_CensusOpen.store(false);
			LogInfo("probe: code-entry census closed");
		}

		// One automatic report, once the census has closed, so a player who
		// never presses the probe hotkey still leaves a usable log behind.
		if (!g_Reported.load() && !g_CensusOpen.load())
		{
			g_Reported.store(true);
			Report();
		}
	}
}
