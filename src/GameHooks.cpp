// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GameHooks.h"
#include "GameBridge.h"
#include "Log.h"

#include <cmath>
#include <cstring>
#include <set>

using namespace YYTK;
using namespace Aurie;

namespace hmd::hooks
{
	namespace
	{
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

		// Names whose signature has already been dumped, so a per-frame detour
		// logs once rather than sixty times a second.
		std::set<std::string> g_SignatureLogged;
	}

	PFUNC_YYGMLScript ResolveEntryPoint(const char* ScriptName)
	{
		if (!ScriptName)
			return nullptr;

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

	PFUNC_YYGMLScript Install(
		AurieModule* Module,
		const char* HookId,
		const char* ScriptName,
		PFUNC_YYGMLScript Detour
	)
	{
		if (!Module || !HookId || !ScriptName || !Detour)
			return nullptr;

		PFUNC_YYGMLScript target = ResolveEntryPoint(ScriptName);
		if (!target)
			return nullptr;

		PVOID trampoline = nullptr;
		AurieStatus status = MmCreateHook(
			Module,
			HookId,
			reinterpret_cast<PVOID>(target),
			reinterpret_cast<PVOID>(Detour),
			&trampoline
		);

		if (!AurieSuccess(status) || !trampoline)
		{
			LogWarn("MmCreateHook failed for '%s' (status %d)", ScriptName,
				static_cast<int>(status));
			return nullptr;
		}

		LogInfo("hooked %s", ScriptName);
		return reinterpret_cast<PFUNC_YYGMLScript>(trampoline);
	}

	void Remove(AurieModule* Module, const char* HookId)
	{
		if (!Module || !HookId)
			return;

		MmRemoveHook(Module, HookId);
	}

	const RValue* Argument(int ArgumentCount, RValue* Arguments[], int Index)
	{
		if (!Arguments || Index < 0 || Index >= ArgumentCount)
			return nullptr;

		// A high argument count on a garbled call frame would otherwise walk
		// off the end of a real array. No GML script this mod touches takes
		// anywhere near this many.
		constexpr int kMaxPlausibleArguments = 32;
		if (ArgumentCount > kMaxPlausibleArguments)
			return nullptr;

		return Arguments[Index];
	}

	std::string Describe(const RValue* Value)
	{
		if (!Value)
			return "<null>";

		char buffer[128]{};

		switch (Value->m_Kind)
		{
		case VALUE_REAL:
		case VALUE_INT32:
		case VALUE_INT64:
			_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "num(%g)",
				Value->ToDouble());
			return buffer;

		case VALUE_BOOL:
			return Value->ToBoolean() ? "bool(true)" : "bool(false)";

		case VALUE_STRING:
		{
			const char* text = Value->ToCString();
			// Peer-visible strings are never logged elsewhere in this mod, but
			// these are the game's own arguments, so a bounded excerpt is fine.
			_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "str(\"%.48s\")",
				text ? text : "");
			return buffer;
		}

		case VALUE_OBJECT: return "struct/instance";
		case VALUE_REF:    return "ref";
		case VALUE_ARRAY:  return "array";
		case VALUE_UNDEFINED: return "undefined";
		case VALUE_PTR:    return "ptr";
		default:
			_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "kind(%d)",
				static_cast<int>(Value->m_Kind));
			return buffer;
		}
	}

	void LogSignatureOnce(
		const char* ScriptName,
		int ArgumentCount,
		RValue* Arguments[],
		const RValue* Result
	)
	{
		if (!ScriptName)
			return;

		if (g_SignatureLogged.count(ScriptName))
			return;
		g_SignatureLogged.insert(ScriptName);

		std::string line;
		for (int i = 0; i < ArgumentCount; i++)
		{
			if (i > 0)
				line += ", ";
			line += Describe(Argument(ArgumentCount, Arguments, i));
		}

		LogInfo("signature: %s(%d args)%s%s -> %s",
			ScriptName,
			ArgumentCount,
			line.empty() ? "" : " = ",
			line.c_str(),
			Describe(Result).c_str());
	}

	bool FindIntegerArgument(
		int ArgumentCount,
		RValue* Arguments[],
		int Low,
		int High,
		int& OutValue
	)
	{
		for (int i = 0; i < ArgumentCount; i++)
		{
			const RValue* value = Argument(ArgumentCount, Arguments, i);
			if (!value)
				continue;

			if (value->m_Kind != VALUE_REAL &&
				value->m_Kind != VALUE_INT32 &&
				value->m_Kind != VALUE_INT64)
				continue;

			const double raw = value->ToDouble();

			// Reject anything that is not a whole number: a round index never
			// arrives as 3.5, but a scale or an alpha might.
			if (!std::isfinite(raw) || raw != std::floor(raw))
				continue;

			const int candidate = static_cast<int>(raw);
			if (candidate < Low || candidate > High)
				continue;

			OutValue = candidate;
			return true;
		}

		return false;
	}
}
