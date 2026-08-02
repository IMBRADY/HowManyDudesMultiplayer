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

	std::vector<RValue> FindInstances(const std::string& ObjectName)
	{
		std::vector<RValue> instances;

		int object_index = AssetIndex(ObjectName);
		if (object_index < 0)
			return instances;

		// GML numbers are reals; pass doubles so the runtime never has to
		// coerce an int64 argument.
		auto count_value = CallBuiltin(
			"instance_number",
			{ RValue(static_cast<double>(object_index)) }
		);
		if (!count_value)
			return instances;

		int count = count_value->ToInt32();
		if (count <= 0)
			return instances;

		// Sanity bound: a plausible arena never holds this many combatants, and
		// a bogus count here would otherwise mean a very long loop on the frame
		// thread.
		constexpr int kMaxInstances = 4096;
		if (count > kMaxInstances)
		{
			LogWarn("instance_number(%s) returned %d - clamping to %d",
				ObjectName.c_str(), count, kMaxInstances);
			count = kMaxInstances;
		}

		instances.reserve(static_cast<size_t>(count));

		for (int i = 0; i < count; i++)
		{
			auto instance = CallBuiltin(
				"instance_find",
				{ RValue(static_cast<double>(object_index)), RValue(static_cast<double>(i)) }
			);

			if (!instance)
				continue;

			// instance_find returns noone (-4) when the index is stale.
			if (instance->m_Kind == VALUE_REAL && instance->ToInt32() < 0)
				continue;

			instances.push_back(*instance);
		}

		return instances;
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

		LogInfo("--- members of %s ---", Label ? Label : "instance");

		int printed = 0;
		g_Api->EnumInstanceMembers(
			Instance,
			[&printed](const char* MemberName, RValue* Value) -> bool
			{
				if (!MemberName)
					return false;

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
				// a diagnostic, not a dump.
				return printed >= 256;
			}
		);

		LogInfo("--- %d member(s) ---", printed);
	}
}
