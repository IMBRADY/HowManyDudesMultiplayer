// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Shared inline-hooking helper for top-level GML scripts.
//
// This is the resolution and safety logic that used to live privately inside
// Match.cpp. Three more subsystems now need to hook scripts - the UI overlay,
// the run-start sync and the round tracker - and every one of them needs the
// same guarantee Match.cpp already made: never install a hook on an address we
// have not proved is the script we asked for.
//
// The proof is a layout check. GetNamedRoutinePointer hands back a CScript*,
// and the native entry point lives at m_Functions->m_ScriptFunction. Those
// offsets are only correct if this runner matches what YYToolkit expects, so
// before trusting any of it we read CScript::m_Name back and require it to
// equal the name we looked up. A wrong layout fails that compare (or faults
// inside the guarded probe) and we decline to hook rather than redirecting a
// bogus address.
//
#include <Aurie/shared.hpp>
#include <YYToolkit/YYTK_Shared.hpp>

#include <string>
#include <vector>

namespace hmd::hooks
{
	// Resolve a script's YYC entry point, or null if it cannot be proven safe.
	// Logs a warning describing which check failed.
	YYTK::PFUNC_YYGMLScript ResolveEntryPoint(const char* ScriptName);

	// Install an inline hook on a top-level GML script.
	//
	// HookId must be unique within the process - Aurie keys its hook table on
	// it. Returns the trampoline (call this to reach the original) or null if
	// the script could not be resolved or the hook could not be created. A null
	// return is never fatal: callers are expected to carry on with whatever
	// their vanilla-behaviour fallback is.
	YYTK::PFUNC_YYGMLScript Install(
		Aurie::AurieModule* Module,
		const char* HookId,
		const char* ScriptName,
		YYTK::PFUNC_YYGMLScript Detour
	);

	// Remove a hook installed by Install. Safe to call for an id that was never
	// installed.
	void Remove(Aurie::AurieModule* Module, const char* HookId);

	// Name the script whose native code contains an address.
	//
	// GetScriptData walks the runner's whole script table by id, so every
	// script's name and YYC entry point can be listed without knowing any name
	// in advance. Given a return address captured inside a detour, the owning
	// script is the one with the greatest entry point at or below it.
	//
	// This is how "what calls enemy_spawn?" gets answered. YYC left no call
	// graph and discovered_mappings.json is a list of guesses; the return
	// address is the fact.
	//
	// Returns an empty string when nothing plausible owns the address, and
	// writes the offset into the winner when one is found. Read-only: it
	// resolves and compares pointers, and calls none of them.
	std::string OwningScriptName(const void* Address, size_t& OutOffset);

	// Every script whose name contains Fragment, case-insensitively.
	//
	// The same table walk as OwningScriptName, used for the other thing YYC took
	// away: the ability to find out what a routine is actually called. Every
	// name this project has tried to use came from discovered_mappings.json,
	// which is a list of plausible guesses - 'enemy_wave_spawner' does not exist
	// and cost several sessions to stop believing in.
	//
	// Read-only. It reads names and calls nothing.
	std::vector<std::string> ScriptNamesContaining(
		const std::string& Fragment,
		size_t Limit = 60
	);

	// --- Argument helpers -------------------------------------------------
	//
	// Hook detours receive RValue* Arguments[] with no type information, and
	// YYC gives us no way to know a script's signature statically. These read
	// defensively out of that array so a detour can inspect what it was handed
	// without ever dereferencing something that is not there.

	// The nth argument, or null if the index is out of range or the slot is
	// empty.
	const YYTK::RValue* Argument(
		int ArgumentCount,
		YYTK::RValue* Arguments[],
		int Index
	);

	// Render an RValue as a short human-readable string for the log. Total:
	// every kind, including ones we do not understand, produces something.
	std::string Describe(const YYTK::RValue* Value);

	// Log a one-line summary of a detour's arguments. Used once per script, the
	// first time it is called, to discover signatures that YYC stripped.
	//
	// Call sites on a per-frame path should guard this with a local flag - see
	// HMD_LOG_SIGNATURE_ONCE. The de-duplication here builds a std::string and
	// hits a set, which is not something a draw hook should do sixty times a
	// second for the rest of the session.
	void LogSignatureOnce(
		const char* ScriptName,
		int ArgumentCount,
		YYTK::RValue* Arguments[],
		const YYTK::RValue* Result
	);

	// Scan the argument list for the first argument that is a whole number
	// inside [Low, High]. This is how a round number is picked out of a script
	// whose signature we cannot know. Returns false if nothing qualifies.
	bool FindIntegerArgument(
		int ArgumentCount,
		YYTK::RValue* Arguments[],
		int Low,
		int High,
		int& OutValue
	);
}

// Log a detour's signature exactly once, at the cost of one bool read on every
// call afterwards. Detours run on the game's own thread, so a plain static is
// sufficient and an atomic would only add a fence to a hot path.
#define HMD_LOG_SIGNATURE_ONCE(ScriptName, ArgumentCount, Arguments, Result) \
	do                                                                       \
	{                                                                        \
		static bool hmd_signature_logged = false;                            \
		if (!hmd_signature_logged)                                           \
		{                                                                    \
			hmd_signature_logged = true;                                     \
			::hmd::hooks::LogSignatureOnce(                                   \
				(ScriptName), (ArgumentCount), (Arguments), (Result));        \
		}                                                                    \
	} while (0)
