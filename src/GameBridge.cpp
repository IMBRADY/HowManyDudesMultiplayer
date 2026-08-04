// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GameBridge.h"
#include "Log.h"

#include <map>
#include <set>

using namespace YYTK;
using namespace Aurie;

namespace hmd::bridge
{
	namespace
	{
		YYTKInterface* g_Api = nullptr;

		// Resolution caches. A name that has already failed is kept in
		// g_MissingScripts so callers on the frame path log at most once.
		std::map<std::string, int> g_ScriptIndices;
		std::map<std::string, void*> g_ScriptPointers;
		std::set<std::string> g_MissingScripts;
		std::map<std::string, int> g_AssetIndices;

		// Scripts whose first unproven call has already been announced.
		std::set<std::string> g_AnnouncedScripts;

		const char* KindName(int Kind)
		{
			switch (Kind)
			{
			case VALUE_REAL:      return "real";
			case VALUE_STRING:    return "string";
			case VALUE_ARRAY:     return "array";
			case VALUE_PTR:       return "ptr";
			case VALUE_UNDEFINED: return "undefined";
			case VALUE_OBJECT:    return "object";
			case VALUE_INT32:     return "int32";
			case VALUE_INT64:     return "int64";
			case VALUE_NULL:      return "null";
			case VALUE_BOOL:      return "bool";
			case VALUE_REF:       return "ref";
			default:              return "?";
			}
		}

		// Count an instance's members, stopping at the first one.
		//
		// Split in two so the guard below can exist: MSVC will not compile
		// __try in a function that needs C++ unwinding, and the std::function
		// this builds is exactly such an object.
		int CountMembersUnsafe(const RValue& Value)
		{
			int count = 0;

			g_Api->EnumInstanceMembers(
				Value,
				[&count](const char* Name, RValue*) -> bool
				{
					if (Name)
						count++;

					// Returning true stops the walk. One is proof enough.
					return true;
				}
			);

			return count;
		}

		// Members, or -1 if looking cost an access violation.
		//
		// The whole reason this exists: ToInstance() returns a NON-NULL pointer
		// for a VALUE_REF which is not a valid CInstance*, and every way of
		// checking whether a pointer is real involves reading through it. The
		// previous build resolved a ref, logged "resolve via 'runtime pointer'",
		// then died on the very next line enumerating its members.
		//
		// So the read is guarded and a fault is a return value. This is what
		// makes it safe to *try* a candidate pointer rather than having to be
		// right about it in advance - which four attempts have now shown is not
		// something that can be reasoned out from this side.
		int CountMembersGuarded(const RValue& Value)
		{
			__try
			{
				return CountMembersUnsafe(Value);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return -1;
			}
		}

		// Turn whatever instance_find handed back into a CInstance*.
		//
		// instance_find on this runner returns kind 15, VALUE_REF - confirmed
		// from a log, not assumed.
		//
		// Four attempts have been spent here. What each one learned:
		//
		//   1. Kept whatever instance_find returned. Members unreadable, and a
		//      value handed to GML aborted the game.
		//   2. Assumed ids, used GetInstanceObject. "could not be resolved".
		//   3. Read the union's m_Instance raw and "validated" it by enumerating
		//      members. Killed the process twice - enumerating IS a dereference,
		//      so the check was the crash. **A pointer cannot be validated by
		//      dereferencing it.**
		//   4. Used ToInstance(), believing RV_ToPointer understands refs. It
		//      returns a NON-NULL pointer that is not an instance. Logged
		//      "resolve via 'runtime pointer'" and died on the next line.
		//
		// So no candidate is trusted here, including the runtime's own. Each is
		// tried, and accepted only if reading through it produces members - with
		// that read guarded, so a wrong candidate costs a caught fault rather
		// than the process. Being wrong is now cheap, which is the only property
		// that has actually helped.
		//
		// Order is id-lookups first, pointer last: GetInstanceObject checks the
		// room's instance list and fails cleanly, whereas ToInstance() lies.
		//
		// ToInt32 is RV_ToInt32, the conversion that aborts the game with "I32
		// argument is undefined". It must never see an undefined, which is why
		// the default case converts nothing at all.
		CInstance* ResolveInstanceValue(const RValue& Value, const char*& HowOut)
		{
			// A candidate is only accepted if reading through it actually
			// produces members, and that read is guarded, so a wrong candidate
			// costs a caught fault instead of the process.
			auto accept = [&](CInstance* Candidate, const char* How) -> CInstance*
			{
				if (!Candidate)
					return nullptr;

				if (CountMembersGuarded(RValue(Candidate)) > 0)
				{
					HowOut = How;
					return Candidate;
				}

				return nullptr;
			};

			// An id that GetInstanceObject accepts is good, full stop.
			//
			// This deliberately does NOT require the result to enumerate any
			// members, and the previous build's insistence that it must was
			// throwing away correct pointers. EnumInstanceMembers works on the
			// global instance - it lists hundreds - and returns zero for an
			// ordinary instance, so "no members" says something about YYToolkit's
			// enumeration, not about whether the pointer is real.
			//
			// The validation that matters already happened inside
			// GetInstanceObject: it looks the id up in the current room's own
			// instance list and returns AURIE_OBJECT_NOT_FOUND otherwise. There
			// is nothing better available, and demanding more cost a session.
			auto by_id = [&](int64_t Id, const char* How, bool& LookupOk) -> CInstance*
			{
				if (Id < 0 || Id > 0x7FFFFFFF)
					return nullptr;

				CInstance* instance = nullptr;
				if (!AurieSuccess(g_Api->GetInstanceObject(
						static_cast<int32_t>(Id), instance)) || !instance)
					return nullptr;

				LookupOk = true;
				HowOut = How;
				return instance;
			};

			switch (Value.m_Kind)
			{
			case VALUE_OBJECT:
			case VALUE_PTR:
			case VALUE_REF:
			{
				// A ref carries the instance id in its low 32 bits. Confirmed
				// from a log, not inferred: o_gameplay came back as i32=100003
				// with i64=0x0400000100018 6A3, and 0x186A3 is 100003. The
				// 0x04000001 in the high dword is a type tag. GameMaker instance
				// ids start at 100000, so this is exactly what it looks like.
				//
				// Order: id lookups first, because GetInstanceObject validates
				// against the room's instance list. The runtime pointer comes
				// LAST and is the only candidate still required to prove itself,
				// because ToInstance() returns a non-null pointer for a ref that
				// is not an instance - trusting it is what killed the game.
				bool lookup_ok = false;

				if (CInstance* found = by_id(Value.m_i32, "raw id", lookup_ok))
					return found;

				if (CInstance* found = by_id(Value.ToInt32(), "converted id", lookup_ok))
					return found;

				if (CInstance* found = accept(Value.ToInstance(), "runtime pointer"))
					return found;

				return nullptr;
			}

			case VALUE_REAL:
			case VALUE_INT32:
			case VALUE_INT64:
			{
				// instance_find returns noone (-4) for a stale index.
				const int32_t id = Value.ToInt32();
				if (id < 0)
					return nullptr;

				bool lookup_ok = false;
				return by_id(id, "id lookup", lookup_ok);
			}

			default:
				// Undefined, unset, null and everything else. Not converted,
				// not dereferenced, not passed on.
				return nullptr;
			}
		}

		// Structured exception handling, deliberately, and not as a substitute
		// for the reasoning above.
		//
		// Everything this mod does runs inside the game's process, and a bad
		// pointer here does not raise a catchable error - it takes the whole
		// game down with no log line and no stack trace, which is precisely how
		// the last two test sessions ended. The functions above are now careful
		// enough that this should never fire; it exists so that being wrong
		// about the runtime's internals one more time costs a warning and a
		// disabled feature rather than someone's run.
		//
		// No C++ objects with destructors in this frame - MSVC will not compile
		// __try in a function that requires unwinding, and the guarded region is
		// kept to the single call for the same reason.
		CInstance* ResolveInstanceGuarded(const RValue& Value, const char*& HowOut)
		{
			__try
			{
				return ResolveInstanceValue(Value, HowOut);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				HowOut = "faulted";
				return nullptr;
			}
		}

		bool ResolveScriptIndex(const std::string& ScriptName, int& Index)
		{
			if (!g_Api)
				return false;

			auto cached = g_ScriptIndices.find(ScriptName);
			if (cached != g_ScriptIndices.end())
			{
				Index = cached->second;
				return true;
			}

			if (g_MissingScripts.count(ScriptName))
				return false;

			int index = -1;
			AurieStatus status = g_Api->GetNamedRoutineIndex(ScriptName.c_str(), &index);

			if (!AurieSuccess(status) || index < 0)
			{
				g_MissingScripts.insert(ScriptName);
				LogWarn("script '%s' did not resolve - dependent behaviour disabled",
					ScriptName.c_str());
				return false;
			}

			g_ScriptIndices[ScriptName] = index;
			Index = index;
			return true;
		}
	}

	bool Initialize(YYTKInterface* Interface)
	{
		if (!Interface)
			return false;

		g_Api = Interface;
		g_ScriptIndices.clear();
		g_ScriptPointers.clear();
		g_MissingScripts.clear();
		g_AssetIndices.clear();
		g_AnnouncedScripts.clear();
		return true;
	}

	bool ScriptExists(const std::string& ScriptName)
	{
		int index = -1;
		return ResolveScriptIndex(ScriptName, index);
	}

	void* ScriptPointer(const std::string& ScriptName)
	{
		if (!g_Api)
			return nullptr;

		auto cached = g_ScriptPointers.find(ScriptName);
		if (cached != g_ScriptPointers.end())
			return cached->second;

		if (g_MissingScripts.count(ScriptName))
			return nullptr;

		PVOID pointer = nullptr;
		AurieStatus status = g_Api->GetNamedRoutinePointer(ScriptName.c_str(), &pointer);

		if (!AurieSuccess(status) || !pointer)
		{
			g_MissingScripts.insert(ScriptName);
			LogWarn("could not resolve a native pointer for '%s' - hook skipped",
				ScriptName.c_str());
			return nullptr;
		}

		g_ScriptPointers[ScriptName] = pointer;
		return pointer;
	}

	bool CallScript(
		const std::string& ScriptName,
		const std::vector<RValue>& Arguments,
		RValue& Result
	)
	{
		if (!g_Api)
			return false;

		// Confirm resolution first so a bad name is reported as a warning
		// rather than surfacing from inside the runtime.
		int index = -1;
		if (!ResolveScriptIndex(ScriptName, index))
			return false;

		CInstance* self = GlobalInstance();

		AurieStatus status = g_Api->CallGameScriptEx(
			Result,
			ScriptName,
			self,
			self,
			Arguments
		);

		if (!AurieSuccess(status))
		{
			LogWarn("call to '%s' failed (status %d)", ScriptName.c_str(),
				static_cast<int>(status));
			return false;
		}

		return true;
	}

	bool CallScript(const std::string& ScriptName, const std::vector<RValue>& Arguments)
	{
		RValue discarded;
		return CallScript(ScriptName, Arguments, discarded);
	}

	bool CallScriptAnnounced(
		const std::string& ScriptName,
		const std::vector<RValue>& Arguments,
		RValue& Result
	)
	{
		// Announce only the first call. After one has returned, the routine has
		// demonstrated it is callable and the announcement is just noise.
		if (!g_AnnouncedScripts.count(ScriptName))
		{
			g_AnnouncedScripts.insert(ScriptName);
			LogInfo("first call to '%s' (%zu arg(s)) - if the game stops here, "
				"this routine is not safe to call and the feature using it "
				"needs disabling",
				ScriptName.c_str(), Arguments.size());
		}

		return CallScript(ScriptName, Arguments, Result);
	}

	bool CallScriptAnnounced(
		const std::string& ScriptName,
		const std::vector<RValue>& Arguments
	)
	{
		RValue discarded;
		return CallScriptAnnounced(ScriptName, Arguments, discarded);
	}

	std::optional<RValue> CallBuiltin(
		const std::string& FunctionName,
		const std::vector<RValue>& Arguments
	)
	{
		if (!g_Api)
			return std::nullopt;

		RValue result;
		AurieStatus status = g_Api->CallBuiltinEx(
			result,
			FunctionName.c_str(),
			GlobalInstance(),
			nullptr,
			Arguments
		);

		if (!AurieSuccess(status))
			return std::nullopt;

		return result;
	}

	CInstance* GlobalInstance()
	{
		if (!g_Api)
			return nullptr;

		CInstance* instance = nullptr;
		if (!AurieSuccess(g_Api->GetGlobalInstance(&instance)))
			return nullptr;

		return instance;
	}

	std::optional<RValue> GetGlobal(const std::string& Name)
	{
		CInstance* global = GlobalInstance();
		if (!global)
			return std::nullopt;

		// Use the provided constructor rather than poking m_Kind/m_Pointer by
		// hand, so the RValue participates in the runtime's reference counting.
		return GetMember(RValue(global), Name);
	}

	std::optional<RValue> GetMember(
		const RValue& Instance,
		const std::string& MemberName
	)
	{
		if (!g_Api)
			return std::nullopt;

		// Only object-like RValues carry members; anything else would make the
		// runtime dereference a non-pointer.
		if (Instance.m_Kind != VALUE_OBJECT && Instance.m_Kind != VALUE_REF)
			return std::nullopt;

		if (!Instance.m_Pointer)
			return std::nullopt;

		RValue* member = nullptr;
		AurieStatus status = g_Api->GetInstanceMember(
			Instance,
			MemberName.c_str(),
			member
		);

		if (!AurieSuccess(status) || !member)
			return std::nullopt;

		return *member;
	}

	bool SetMember(
		const RValue& Instance,
		const std::string& MemberName,
		const RValue& NewValue
	)
	{
		if (!g_Api)
			return false;

		if (Instance.m_Kind != VALUE_OBJECT && Instance.m_Kind != VALUE_REF)
			return false;

		if (!Instance.m_Pointer)
			return false;

		RValue* member = nullptr;
		AurieStatus status = g_Api->GetInstanceMember(
			Instance,
			MemberName.c_str(),
			member
		);

		if (!AurieSuccess(status) || !member)
			return false;

		// GetInstanceMember yields a pointer into the instance's own variable
		// slot, so assigning through it writes the live value.
		*member = NewValue;
		return true;
	}

	std::optional<std::string> FindMemberName(
		const RValue& Instance,
		const std::vector<std::string>& Candidates
	)
	{
		for (const std::string& candidate : Candidates)
		{
			if (GetMember(Instance, candidate).has_value())
				return candidate;
		}
		return std::nullopt;
	}

	double GetMemberNumber(
		const RValue& Instance,
		const std::string& MemberName,
		double Fallback
	)
	{
		auto member = GetMember(Instance, MemberName);
		if (!member)
			return Fallback;

		switch (member->m_Kind)
		{
		case VALUE_REAL:
		case VALUE_INT32:
		case VALUE_INT64:
		case VALUE_BOOL:
			return member->ToDouble();
		default:
			return Fallback;
		}
	}

	std::string GetMemberString(
		const RValue& Instance,
		const std::string& MemberName,
		const std::string& Fallback
	)
	{
		auto member = GetMember(Instance, MemberName);
		if (!member || member->m_Kind != VALUE_STRING)
			return Fallback;

		const char* text = member->ToCString();
		return text ? std::string(text) : Fallback;
	}

	int AssetIndex(const std::string& AssetName)
	{
		auto cached = g_AssetIndices.find(AssetName);
		if (cached != g_AssetIndices.end())
			return cached->second;

		auto result = CallBuiltin("asset_get_index", { RValue(AssetName) });
		int index = result ? result->ToInt32() : -1;

		if (index < 0)
			LogWarn("asset '%s' not found", AssetName.c_str());

		g_AssetIndices[AssetName] = index;
		return index;
	}

	// Live instances of an object, as values the rest of the mod can actually
	// read members from and hand to game scripts.
	//
	// This used to be instance_number + instance_find in a loop, keeping
	// whatever RValue came back. Those RValues were not usable. The evidence,
	// from a duel at round 2:
	//
	//     --- members of o_dude ---
	//     --- 0 member(s) listed, 0 runtime-internal name(s) hidden ---
	//     field 'type' did not resolve to any known member name
	//     ... (all ten fields)
	//     I32 argument is undefined
	//     - gml_Script_dude_is_knocked_out:238
	//
	// Nothing could be enumerated off them, every field read failed, and handing
	// one to a game script aborted the game outright. The old loop only ever
	// rejected a negative VALUE_REAL, so an undefined sailed straight through
	// into the array and out the other side into GML.
	//
	// InvokeWithObject is YYToolkit's own instance enumeration and it hands back
	// CInstance pointers rather than ids. RValue has a constructor for those
	// which produces a VALUE_OBJECT, which is what GetInstanceMember and
	// EnumInstanceMembers want and what the old path never produced.
	std::vector<RValue> FindInstances(const std::string& ObjectName)
	{
		std::vector<RValue> instances;

		if (!g_Api)
			return instances;

		int object_index = AssetIndex(ObjectName);
		if (object_index < 0)
			return instances;

		// instance_number / instance_find, then GetInstanceObject on each id.
		//
		// InvokeWithObject was tried here and finds nothing when handed an
		// object index as a VALUE_REAL - FindInstances("o_gameplay") came back
		// empty from inside rm_gameplay, which made ArenaIsPopulated permanently
		// false and stopped every duel from triggering. The builtin pair does
		// find the instances; what it hands back is just ids.
		//
		// So the ids are what gets asked for, and GetInstanceObject turns each
		// one into the CInstance the rest of the mod actually needs. That is the
		// step that was missing all along: the old code pushed the raw id and
		// then tried to read members off a number, which is why every field
		// lookup reported "did not resolve" and why nothing could be enumerated.
		auto count_value = CallBuiltin(
			"instance_number",
			{ RValue(static_cast<double>(object_index)) }
		);
		if (!count_value)
			return instances;

		int count = count_value->ToInt32();
		if (count <= 0)
			return instances;

		// A plausible arena never holds this many combatants, and a bogus count
		// would otherwise mean a very long loop on the frame path.
		constexpr int kMaxInstances = 4096;
		if (count > kMaxInstances)
		{
			LogWarn("instance_number(%s) returned %d - clamping to %d",
				ObjectName.c_str(), count, kMaxInstances);
			count = kMaxInstances;
		}

		instances.reserve(static_cast<size_t>(count));

		int unresolved = 0;
		int faulted = 0;
		int kept_refs = 0;
		int observed_kind = -1;

		// Kept so a failure can report what it was actually looking at.
		int32_t sample_i32 = 0;
		int64_t sample_i64 = 0;
		uint32_t sample_flags = 0;

		for (int i = 0; i < count; i++)
		{
			auto found = CallBuiltin(
				"instance_find",
				{ RValue(static_cast<double>(object_index)), RValue(static_cast<double>(i)) }
			);

			if (!found)
				continue;

			observed_kind = static_cast<int>(found->m_Kind);
			sample_i32 = found->m_i32;
			sample_i64 = found->m_i64;
			sample_flags = found->m_Flags;

			const char* how = "?";
			CInstance* instance = ResolveInstanceGuarded(*found, how);

			if (!instance)
			{
				if (how && strcmp(how, "faulted") == 0)
					faulted++;

				unresolved++;

				// Keep the ref itself rather than dropping it.
				//
				// Converting a ref to a CInstance* has now failed every way it
				// can be attempted - GetInstanceObject rejects the id,
				// ToInstance() returns a pointer that faults. But conversion was
				// never actually required: GetInstanceMember takes an RValue and
				// GetMember already accepts VALUE_REF, so a ref can be read from
				// directly without ever becoming a pointer.
				//
				// This is also what the original code did, and it never crashed
				// - every crash in this area came from something I added to
				// "fix" it. The field reads failed back then with "did not
				// resolve to any known member name", which was read as proof the
				// ref was useless. It is equally consistent with the NAMES being
				// wrong, and that reading was never tested.
				//
				// Nothing here is dereferenced. If reads work, the mod works; if
				// they do not, the name probe in LogInstanceMembers says so
				// using x/y/id as controls.
				if (found->m_Kind == VALUE_REF && found->m_Pointer)
				{
					kept_refs++;
					instances.push_back(*found);
				}

				continue;
			}

			// Which reading of the union turned out to be the right one. Worth
			// exactly one line per object for the whole session, and it is the
			// line that closes this question for good.
			static std::set<std::string> reported;
			if (reported.insert(ObjectName).second)
			{
				// Two separate facts, reported separately, because conflating
				// them is what caused the last round of wrong conclusions.
				// Resolution succeeding says the pointer is real. Enumeration
				// is a different question: it returns hundreds of members for
				// the global instance and zero for an ordinary one, so a zero
				// here means member *listing* is unavailable, NOT that the
				// instance is bad. Field reads go through GetInstanceMember and
				// may well work regardless.
				const int members = CountMembersGuarded(RValue(instance));

				LogInfo("instances of %s resolve via '%s' (instance_find kind "
					"%d); member enumeration yields %s",
					ObjectName.c_str(), how, observed_kind,
					members < 0 ? "a fault" :
					members > 0 ? "members" : "nothing (listing unavailable)");
			}

			instances.emplace_back(instance);
		}

		if (unresolved > 0)
		{
			static std::set<std::string> complained;
			if (complained.insert(ObjectName).second)
			{
				if (kept_refs > 0)
				{
					LogInfo("%s: %d instance(s) kept as refs and read directly - "
						"no pointer conversion needed, and none possible on this "
						"runner", ObjectName.c_str(), kept_refs);
				}

				// The kind is the whole point of this line. Two rewrites of this
				// function have now been built on a guess about what
				// instance_find hands back; naming it means the next one is not.
				LogWarn("%d of %d %s instance(s) could not be resolved to an "
					"object - instance_find returned kind %d (%s). They are "
					"skipped rather than passed on.",
					unresolved, count, ObjectName.c_str(),
					observed_kind, KindName(observed_kind));

				if (faulted > 0)
				{
					LogError("%d of those FAULTED - resolving them raised an "
						"access violation that would have killed the game. The "
						"guard held. Do not remove it.", faulted);
				}

				// Raw facts only. The previous version of this line asserted
				// that GetInstanceObject had rejected the ids, which it had no
				// way of knowing - the lookup may well have succeeded and been
				// discarded by an over-strict acceptance test downstream. An
				// overconfident diagnostic is worse than none, because it is
				// believed. State what was seen, not what it means.
				LogWarn("  the unresolved value raw: i32=%d i64=0x%llX "
					"flags=0x%X (low 32 bits of i64 are the instance id)",
					sample_i32,
					static_cast<unsigned long long>(sample_i64),
					sample_flags);
			}
		}

		return instances;
	}

	bool AsInstance(const RValue& Value, RValue& Out)
	{
		if (!g_Api)
			return false;

		// Already resolved - FindInstances hands these out.
		if (Value.m_Kind == VALUE_OBJECT && Value.m_Pointer)
		{
			Out = Value;
			return true;
		}

		const char* how = "?";
		CInstance* instance = ResolveInstanceGuarded(Value, how);
		if (!instance)
			return false;

		Out = RValue(instance);
		return true;
	}

	bool IsUsableInstance(const RValue& Value)
	{
		// Refs count. They are what this runner hands out for every instance and
		// what FindInstances now keeps when conversion fails, which is always.
		// GetInstanceMember accepts them, so they are readable even though they
		// are not pointers.
		//
		// What this still excludes is the thing it was written for: undefined,
		// and anything else that is not instance-shaped. Those must never reach
		// a game script - dude_is_knocked_out aborts the whole game on one.
		const bool instance_shaped =
			Value.m_Kind == VALUE_OBJECT || Value.m_Kind == VALUE_REF;

		return instance_shaped && Value.m_Pointer != nullptr;
	}

	std::string CurrentRoomName()
	{
		if (!g_Api)
			return {};

		// "room" is a BUILTIN variable, not an instance member. Reading it with
		// GetInstanceMember - which is what this used to do, via GetGlobal -
		// crashes the game outright on this runner. GetBuiltin is the accessor
		// meant for builtins and is safe here.
		RValue room;
		AurieStatus status = g_Api->GetBuiltin("room", GlobalInstance(), 0, room);

		if (!AurieSuccess(status))
			return {};

		auto name = CallBuiltin("room_get_name", { room });
		if (!name || name->m_Kind != VALUE_STRING)
			return {};

		const char* text = name->ToCString();
		return text ? std::string(text) : std::string{};
	}

	void LogInstanceMembers(const RValue& Instance, const char* Label)
	{
		if (!g_Api)
			return;

		// Look before walking. The full enumeration below is not guarded - it
		// takes a capturing lambda and cannot be - so the guarded one-member
		// probe runs first and this returns rather than dying if the value is
		// not really readable. A build that printed this header and then killed
		// the process is exactly why.
		const int probe = CountMembersGuarded(Instance);
		if (probe < 0)
		{
			LogError("cannot list members of %s - reading it raised an access "
				"violation. The value is kind %d (%s) and is not a real "
				"instance, whatever it claims to be.",
				Label ? Label : "instance",
				static_cast<int>(Instance.m_Kind),
				KindName(static_cast<int>(Instance.m_Kind)));
			return;
		}

		if (probe == 0)
		{
			// Enumeration is unavailable for ordinary instances on this runner -
			// it lists hundreds for the global instance and nothing here. So
			// names cannot be DISCOVERED. They can still be TESTED one at a
			// time through GetInstanceMember, which is a different API and may
			// work perfectly well.
			//
			// The built-ins come first and they are the control: every GameMaker
			// instance has x, y, id and object_index. If those read, member
			// access works and only the stat names below are wrong. If they do
			// not, member access is unavailable entirely and no name will ever
			// resolve - which is a completely different problem and worth not
			// confusing with the first.
			LogInfo("--- %s: enumeration unavailable, probing names directly ---",
				Label ? Label : "instance");

			static const char* kProbeNames[] = {
				// Controls - every instance has these.
				"x", "y", "id", "object_index", "depth", "sprite_index",
				"image_index", "image_xscale", "visible",
				// What the mod actually wants.
				"type", "name", "level", "hp", "max_hp", "health", "max_health",
				"hitpoints", "attack", "damage", "power", "speed", "move_speed",
				"range", "attack_range", "crit_chance", "crit_damage",
				"knocked_out", "unconscious", "is_knocked_out", "downed",
				"dude_type", "dude", "team", "owner", "stats", "base_stats",
				"dude_id", "index", "slot",
			};

			int found = 0;
			for (const char* candidate : kProbeNames)
			{
				auto value = GetMember(Instance, candidate);
				if (!value)
					continue;

				found++;

				char rendered[96]{};
				switch (value->m_Kind)
				{
				case VALUE_REAL:
				case VALUE_INT32:
				case VALUE_INT64:
				case VALUE_BOOL:
					_snprintf_s(rendered, sizeof(rendered) - 1, _TRUNCATE,
						"%g", value->ToDouble());
					break;
				default:
					_snprintf_s(rendered, sizeof(rendered) - 1, _TRUNCATE,
						"<%s>", KindName(static_cast<int>(value->m_Kind)));
					break;
				}

				LogInfo("    %-16s %-8s = %s", candidate,
					KindName(static_cast<int>(value->m_Kind)), rendered);
			}

			LogInfo("--- %d of %zu probed name(s) exist on %s ---",
				found, sizeof(kProbeNames) / sizeof(kProbeNames[0]),
				Label ? Label : "instance");

			if (found == 0)
			{
				LogWarn("not even x/y/id read back - member access is "
					"unavailable on this instance entirely, which is a "
					"different problem from having the wrong names. Raw: "
					"kind %d (%s), i32 %d, i64 0x%llX.",
					static_cast<int>(Instance.m_Kind),
					KindName(static_cast<int>(Instance.m_Kind)),
					Instance.m_i32,
					static_cast<unsigned long long>(Instance.m_i64));
			}

			return;
		}

		LogInfo("--- members of %s ---", Label ? Label : "instance");

		int printed = 0;
		int skipped = 0;
		g_Api->EnumInstanceMembers(
			Instance,
			[&printed, &skipped](const char* MemberName, RValue* Value) -> bool
			{
				if (!MemberName)
					return false;

				// The global scope is dominated by the runtime's anonymous
				// struct closures - "___struct___1234" and friends, thousands
				// of them. They sort first alphabetically, so without this the
				// listing cap is spent entirely on them and the dump never
				// reaches a single name a human chose. That made this
				// diagnostic worthless the first time it was needed.
				// Returning true STOPS the enumeration - the callback is a
				// search predicate, not a "keep going" flag. Skipping means
				// returning false.
				if (strncmp(MemberName, "___struct___", 12) == 0 ||
					strncmp(MemberName, "gml_Script_", 11) == 0)
				{
					skipped++;
					return false;
				}

				const char* kind = "?";
				if (Value)
				{
					switch (Value->m_Kind)
					{
					case VALUE_REAL:   kind = "real";   break;
					case VALUE_STRING: kind = "string"; break;
					case VALUE_ARRAY:  kind = "array";  break;
					case VALUE_OBJECT: kind = "struct"; break;
					case VALUE_BOOL:   kind = "bool";   break;
					case VALUE_INT32:  kind = "int32";  break;
					case VALUE_INT64:  kind = "int64";  break;
					case VALUE_UNDEFINED: kind = "undefined"; break;
					default: break;
					}
				}

				LogInfo("    %-32s %s", MemberName, kind);
				printed++;

				// Stop after a sane cap; some structs are enormous and this is
				// a diagnostic, not a dump. Raised well past the interesting
				// range now that the runtime's own noise is filtered out.
				return printed >= 600;
			}
		);

		LogInfo("--- %d member(s) listed, %d runtime-internal name(s) hidden ---",
			printed, skipped);
	}

	void LogMatchingMembers(
		const RValue& Instance,
		const char* Label,
		const std::vector<std::string>& Substrings
	)
	{
		if (!g_Api)
			return;

		LogInfo("--- %s members matching the search ---",
			Label ? Label : "instance");

		int found = 0;
		g_Api->EnumInstanceMembers(
			Instance,
			[&found, &Substrings](const char* MemberName, RValue* Value) -> bool
			{
				if (!MemberName)
					return false;

				// Case-insensitive containment against every term.
				std::string lowered(MemberName);
				for (char& character : lowered)
					character = static_cast<char>(tolower(
						static_cast<unsigned char>(character)));

				bool matches = false;
				for (const std::string& term : Substrings)
				{
					if (lowered.find(term) != std::string::npos)
					{
						matches = true;
						break;
					}
				}

				if (!matches)
					return false;

				// The value matters as much as the name here: the point is to
				// spot which candidate actually holds the number we want, and
				// a struct-valued global called "round_something" is not it.
				if (Value && (Value->m_Kind == VALUE_REAL ||
					Value->m_Kind == VALUE_INT32 ||
					Value->m_Kind == VALUE_INT64 ||
					Value->m_Kind == VALUE_BOOL))
				{
					LogInfo("    %-40s = %g", MemberName, Value->ToDouble());
				}
				else if (Value && Value->m_Kind == VALUE_STRING)
				{
					const char* text = Value->ToCString();
					LogInfo("    %-40s = \"%.32s\"", MemberName, text ? text : "");
				}
				else
				{
					// Skipped, not printed. The overwhelming majority of
					// matches are the game's own functions stored as global
					// methods, and listing them spends the cap before the
					// enumeration reaches the letter R - where every name
					// worth having happens to start.
					return false;
				}

				found++;

				// Returning true STOPS enumeration; keep going until the cap.
				return found >= 120;
			}
		);

		LogInfo("--- %d match(es) ---", found);
	}
}
