// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Null-safe adapter over YYTKInterface.
//
// Every entry point here is total: it returns a status/fallback instead of
// throwing or dereferencing something the runtime did not give us. The brief
// requires that a missing address or absent instance produce a logged warning
// and vanilla behaviour, never a crash-to-desktop, and this is where that
// guarantee is actually enforced.
//
#include <YYToolkit/YYTK_Shared.hpp>

#include <optional>
#include <string>
#include <vector>

namespace hmd::bridge
{
	// Must be called once from ModuleInitialize before anything else here.
	bool Initialize(YYTK::YYTKInterface* Interface);

	// True if a top-level GML script of this name resolves in the runtime.
	// Results are cached; a name that fails to resolve is remembered as absent
	// so a per-frame caller cannot spam the log.
	bool ScriptExists(const std::string& ScriptName);

	// Resolve the native entry point of a top-level GML script. Under YYC this
	// is a real function pointer suitable for inline hooking. Null on failure.
	void* ScriptPointer(const std::string& ScriptName);

	// Call a GML script by name. Returns false (and logs once) if the script
	// does not resolve or the runtime reports failure.
	bool CallScript(
		const std::string& ScriptName,
		const std::vector<YYTK::RValue>& Arguments,
		YYTK::RValue& Result
	);

	// Fire-and-forget variant for scripts whose return value is not needed.
	bool CallScript(
		const std::string& ScriptName,
		const std::vector<YYTK::RValue>& Arguments = {}
	);

	// Call a script this mod has not proven safe to invoke, announcing the
	// first such call to the log immediately before making it.
	//
	// Why this exists: YYC strips the bytecode, so a routine's name is the only
	// thing we have to go on, and a name does not say what a routine IS. A
	// diagnostic in this mod once called gml_Script_wave_real to see what it
	// returned; it turned out to be a constructor, and invoking it raised a GML
	// error that took the game down on startup. A GML-level error cannot be
	// caught from here - there is no try/except that reaches it - so the only
	// defence available is to make the log say what we were about to do. If the
	// game dies, the last line names the routine responsible.
	//
	// Use this for any script the mod calls that it has not observed the game
	// calling itself. Prefer builtins, which have signatures that are part of
	// GameMaker and cannot drift.
	bool CallScriptAnnounced(
		const std::string& ScriptName,
		const std::vector<YYTK::RValue>& Arguments,
		YYTK::RValue& Result
	);

	bool CallScriptAnnounced(
		const std::string& ScriptName,
		const std::vector<YYTK::RValue>& Arguments = {}
	);

	// Call a GameMaker builtin by name. Returns nullopt on failure.
	std::optional<YYTK::RValue> CallBuiltin(
		const std::string& FunctionName,
		const std::vector<YYTK::RValue>& Arguments = {}
	);

	// The global scope instance ("global." in GML). Null if unavailable.
	YYTK::CInstance* GlobalInstance();

	// Read a global variable. Returns nullopt if the global scope or the member
	// is unavailable.
	std::optional<YYTK::RValue> GetGlobal(const std::string& Name);

	// Read a member off an instance/struct RValue without assuming it exists.
	std::optional<YYTK::RValue> GetMember(
		const YYTK::RValue& Instance,
		const std::string& MemberName
	);

	// Write a member on an instance/struct. Only succeeds if the member already
	// exists, since GetInstanceMember hands back a pointer to the live slot;
	// this mod never invents new instance variables on game objects.
	bool SetMember(
		const YYTK::RValue& Instance,
		const std::string& MemberName,
		const YYTK::RValue& NewValue
	);

	// Try a list of candidate member names and return the first that resolves.
	// YYC strips the VARI chunk, so the mod cannot know statically whether a
	// stat is called "hp", "health" or "hp_current"; this probes and reports
	// which name actually exists on the live instance.
	std::optional<std::string> FindMemberName(
		const YYTK::RValue& Instance,
		const std::vector<std::string>& Candidates
	);

	// Convenience readers that collapse a missing/mistyped member to a default.
	double GetMemberNumber(
		const YYTK::RValue& Instance,
		const std::string& MemberName,
		double Fallback = 0.0
	);

	std::string GetMemberString(
		const YYTK::RValue& Instance,
		const std::string& MemberName,
		const std::string& Fallback = {}
	);

	// Asset index for an object/sprite/etc. by name. -1 if not found.
	int AssetIndex(const std::string& AssetName);

	// All live instances of an object, by object name. Empty on any failure.
	//
	// Every returned value is a VALUE_OBJECT wrapping a live CInstance, so it
	// can be read with GetMember. The ids come from instance_number /
	// instance_find and are converted with GetInstanceObject; the conversion is
	// the step that used to be missing, which is why member reads all failed.
	// See the comment on the implementation - InvokeWithObject was tried in its
	// place and finds nothing on this runner.
	std::vector<YYTK::RValue> FindInstances(const std::string& ObjectName);

	// Resolve any instance-shaped RValue into one members can be written to.
	//
	// Needed because every builtin that hands back an instance on this runner -
	// instance_find, instance_create_depth - returns a VALUE_REF, and a ref
	// cannot be read from or written to directly. Anything that creates an
	// instance and then sets fields on it must go through this first.
	bool AsInstance(const YYTK::RValue& Value, YYTK::RValue& Out);

	// True when Value is something the runtime will accept as an instance.
	// Guard anything crossing back into GML with this: a game script handed a
	// bad value aborts the whole game rather than returning an error.
	bool IsUsableInstance(const YYTK::RValue& Value);

	// Name of the room the game is currently in. Empty on failure.
	std::string CurrentRoomName();

	// Dump an instance's member names and kinds to the log. Used once on the
	// first o_dude the mod sees, because YYC strips the VARI chunk and the real
	// member layout is only discoverable at runtime.
	void LogInstanceMembers(const YYTK::RValue& Instance, const char* Label);

	// Log only the members whose name contains one of the given substrings,
	// with their values.
	//
	// The global scope has thousands of members and an alphabetical dump never
	// gets past the letter B before any sane cap stops it - which is exactly
	// how the first attempt to find the run's round counter failed. Searching
	// beats dumping when you already know what you are looking for.
	void LogMatchingMembers(
		const YYTK::RValue& Instance,
		const char* Label,
		const std::vector<std::string>& Substrings
	);
}
