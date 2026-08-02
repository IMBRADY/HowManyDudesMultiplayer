// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Logging shim. Every lifecycle stage in the mod reports through here so that
// a player can diagnose a failed session from the log alone.
//
// Output goes through Aurie's DbgPrintEx rather than YYToolkit's interface.
// YYToolkit's "Zeus" generation dropped Print/PrintInfo/PrintWarning/PrintError
// from YYTKInterface and routes all logging through the framework instead, so
// everything now lands in aurie.log next to the game executable, tagged with a
// severity. That is also strictly more robust: these helpers no longer depend
// on the YYToolkit interface being resolved, so a failure during startup still
// gets reported.
//
#include <Aurie/shared.hpp>
#include <YYToolkit/YYTK_Shared.hpp>

namespace hmd
{
	// Set once during ModuleInitialize. Never null after a successful load.
	extern YYTK::YYTKInterface* g_Interface;

	// Lifecycle stages called out explicitly by the design brief.
	inline constexpr const char* kStageConnect   = "CONNECT";
	inline constexpr const char* kStageSerialize = "SERIALIZE";
	inline constexpr const char* kStageInject    = "INJECT";
	inline constexpr const char* kStageResolve   = "RESOLVE";

	namespace detail
	{
		// Aurie's logging routines are dispatched through a framework routine
		// table that only exists once the module has been loaded by Aurie.
		// g_ArInitialImage is set during that handshake, so it doubles as a
		// reliable "are we running under Aurie at all?" test. The offline test
		// harness links these units without a framework, and must not fault.
		inline bool FrameworkReady()
		{
			return Aurie::g_ArInitialImage != nullptr;
		}

		// One formatting path for every severity. The message is rendered into a
		// fixed buffer first so that a caller's format string can never be
		// interpreted twice, and so truncation is bounded rather than fatal.
		template <typename... Args>
		void Emit(
			Aurie::AurieLogSeverity Severity,
			const char* Prefix,
			const char* Format,
			Args... Arguments
		)
		{
			if (!FrameworkReady())
				return;

			char message[1024]{};
			_snprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format, Arguments...);
			Aurie::DbgPrintEx(Severity, "%s %s", Prefix, message);
		}
	}

	template <typename... Args>
	void LogStage(const char* Stage, const char* Format, Args... Arguments)
	{
		if (!detail::FrameworkReady())
			return;

		char message[1024]{};
		_snprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format, Arguments...);
		Aurie::DbgPrintEx(Aurie::LOG_SEVERITY_INFO, "[HMD-MP][%s] %s", Stage, message);
	}

	template <typename... Args>
	void LogInfo(const char* Format, Args... Arguments)
	{
		detail::Emit(Aurie::LOG_SEVERITY_INFO, "[HMD-MP]", Format, Arguments...);
	}

	template <typename... Args>
	void LogWarn(const char* Format, Args... Arguments)
	{
		detail::Emit(Aurie::LOG_SEVERITY_WARNING, "[HMD-MP]", Format, Arguments...);
	}

	template <typename... Args>
	void LogError(const char* Format, Args... Arguments)
	{
		detail::Emit(Aurie::LOG_SEVERITY_ERROR, "[HMD-MP]", Format, Arguments...);
	}
}
