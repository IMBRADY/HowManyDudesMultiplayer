// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// HowManyDudesMultiplayer - asynchronous P2P multiplayer for "How Many Dudes?"
//
// Plugin entry point. Owns the YYTK interface handle, reads session config,
// installs the frame callback, and exposes the hotkeys a player uses to open
// or join a session.
//

// Where a player can obtain the corresponding source for this build, as
// AGPL-3.0 requires. Update this if the repository moves - it is the only
// place the URL appears in the running program.
#define HMD_SOURCE_URL "https://github.com/IMBRADY/HowManyDudesMultiplayer"

#include "GameBridge.h"
#include "Json.h"
#include "Log.h"
#include "Match.h"
#include "Net.h"
#include "Probe.h"
#include "Roster.h"
#include "RunState.h"
#include "Steam.h"
#include "Ui.h"

#include <chrono>
#include <cstdarg>
#include <fstream>
#include <string>

using namespace Aurie;
using namespace YYTK;

namespace hmd
{
	// The single definition of the interface handle declared in Log.h.
	YYTKInterface* g_Interface = nullptr;
}

namespace
{
	AurieModule* g_Module = nullptr;

	// Remembered so the boot log can be written from anywhere, not just from
	// ModuleInitialize. It is the only channel that survives a hard crash,
	// because each call opens, writes and closes the file.
	fs::path g_ModulePath;

	// Defined below, declared here so the diagnostics can be used above it.
	void BootLog(const fs::path& ModulePath, const char* Format, ...);

	// "auto" means "find the host on the local network" rather than naming an
	// address. It is the default so that a fresh install needs no editing at
	// all on a LAN.
	constexpr const char* kAutoAddress = "auto";

	struct Config
	{
		std::string peer_address = kAutoAddress;
		std::string session_key;
		unsigned short port = 47801;
		bool enable_discovery = true;
		bool auto_host = false;
		bool auto_join = false;

		// Rounds between duels. 20 is the length of an act, so by default the
		// duel lands exactly where the boss fight would have been.
		int duel_interval = 20;

		// Whether one player pressing "start run" starts the other's run too.
		bool sync_run_start = true;

		// Show the mod's messages on screen through the game's own info
		// stream. Turning this off falls back to log-only.
		bool on_screen_messages = true;
	};

	Config g_Config;

	bool JoiningByDiscovery()
	{
		return g_Config.peer_address.empty() ||
			g_Config.peer_address == kAutoAddress;
	}

	// Hotkeys. Read with GetAsyncKeyState so they work regardless of how the
	// game's own input system is configured.
	constexpr int kKeyInvite = VK_F7;
	constexpr int kKeyHost = VK_F9;
	constexpr int kKeyJoin = VK_F10;
	constexpr int kKeyDisconnect = VK_F11;
	constexpr int kKeyStatus = VK_F8;

	// Dumps everything the mod discovered about this build of the game. The
	// features that draw or that need a round number depend on things YYC left
	// undiscoverable in data.win, so when one of them misbehaves this is what
	// says which part did not resolve.
	constexpr int kKeyProbe = VK_F6;

	// Deliberately its own key rather than part of F6. It runs the game's own
	// matchup parser on the local machine, which loads a matchup - so it can
	// disturb the round the player is standing in. Diagnostics should be free
	// to press; this one is not, so it is not bundled with the ones that are.
	constexpr int kKeySelfTest = VK_F5;

	bool WasKeyPressed(int VirtualKey)
	{
		// GetAsyncKeyState's low bit reports "pressed since last call", which
		// gives edge detection without tracking previous state ourselves.
		return (GetAsyncKeyState(VirtualKey) & 1) != 0;
	}

	std::string Trim(const std::string& Text)
	{
		size_t first = Text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
			return {};
		size_t last = Text.find_last_not_of(" \t\r\n");
		return Text.substr(first, last - first + 1);
	}

	// Writes a fully commented config next to the DLL, so a player never has to
	// create or rename one by hand. Called only when no config exists; an
	// existing file is never rewritten, so hand edits always survive.
	void WriteDefaultConfig(const fs::path& ConfigPath)
	{
		std::ofstream file(ConfigPath);
		if (!file.is_open())
		{
			// A read-only game folder is not fatal - the defaults are the same
			// either way, the player just cannot change them from a file.
			hmd::LogWarn("could not write a default config to %s - continuing "
				"with built-in defaults", ConfigPath.string().c_str());
			return;
		}

		file <<
			"; HowManyDudesMultiplayer session config.\n"
			"; Written automatically on first run. Edit freely - this file is\n"
			"; never overwritten once it exists.\n"
			"\n"
			"; Where the JOINING player looks for the host. Ignored by the host.\n"
			";   auto            - find the host automatically on the local\n"
			";                     network. No addresses to exchange. (default)\n"
			";   <ip or hostname> - dial that address instead. Needed over the\n"
			";                     internet, or if discovery is blocked.\n"
			"peer_address = auto\n"
			"\n"
			"; TCP port. Both players must agree. The host must have it\n"
			"; reachable. LAN discovery uses the next port up (default 47802).\n"
			"port = 47801\n"
			"\n"
			"; Shared passphrase. Both players must set the SAME value or the\n"
			"; connection is refused. Leave empty on a trusted LAN. Set it to\n"
			"; anything you both agree on before forwarding the port to the\n"
			"; internet, otherwise any host that can reach the port may join.\n"
			"; Note: traffic is not encrypted; this controls who connects, it\n"
			"; does not hide what is sent.\n"
			"session_key =\n"
			"\n"
			"; Answer and send local-network discovery probes. Turn off to make\n"
			"; the mod address-only.\n"
			"enable_discovery = true\n"
			"\n"
			"; Open a session automatically at load instead of waiting for\n"
			"; F9/F10. Set at most one, on the matching machine.\n"
			"auto_host = false\n"
			"auto_join = false\n"
			"\n"
			"; How many rounds between duels. Every this many rounds the boss\n"
			"; fight is replaced by a fight against your opponent's army. 20 is\n"
			"; the length of an act, so the duel lands where the boss would.\n"
			"; Whoever gets there first waits for the other.\n"
			"duel_interval = 20\n"
			"\n"
			"; Pressing 'start run' also starts your opponent's run, so you\n"
			"; both enter a run together. Turn off to start runs separately.\n"
			"sync_run_start = true\n"
			"\n"
			"; Show the mod's messages on screen using the game's own message\n"
			"; stream. Turn off if it misbehaves - everything the mod says\n"
			"; still goes to aurie.log either way.\n"
			"on_screen_messages = true\n";

		hmd::LogInfo("wrote a default config to %s", ConfigPath.string().c_str());
	}

	void ApplySetting(const std::string& Key, const std::string& Value)
	{
		if (Key == "peer_address")
		{
			g_Config.peer_address = Value;
		}
		else if (Key == "session_key")
		{
			g_Config.session_key = Value;
		}
		else if (Key == "port")
		{
			int parsed = atoi(Value.c_str());
			if (parsed > 0 && parsed < 65536)
				g_Config.port = static_cast<unsigned short>(parsed);
			else
				hmd::LogWarn("ignoring out-of-range port '%s'", Value.c_str());
		}
		else if (Key == "enable_discovery")
		{
			g_Config.enable_discovery = (Value == "1" || Value == "true");
		}
		else if (Key == "auto_host")
		{
			g_Config.auto_host = (Value == "1" || Value == "true");
		}
		else if (Key == "auto_join")
		{
			g_Config.auto_join = (Value == "1" || Value == "true");
		}
		else if (Key == "duel_interval")
		{
			int parsed = atoi(Value.c_str());
			if (parsed > 0 && parsed <= 500)
				g_Config.duel_interval = parsed;
			else
				hmd::LogWarn("ignoring out-of-range duel_interval '%s'",
					Value.c_str());
		}
		else if (Key == "sync_run_start")
		{
			g_Config.sync_run_start = (Value == "1" || Value == "true");
		}
		else if (Key == "on_screen_messages")
		{
			g_Config.on_screen_messages = (Value == "1" || Value == "true");
		}
	}

	bool ReadConfigFile(const fs::path& ConfigPath)
	{
		std::ifstream file(ConfigPath);
		if (!file.is_open())
			return false;

		std::string line;
		while (std::getline(file, line))
		{
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
				continue;

			size_t separator = line.find('=');
			if (separator == std::string::npos)
				continue;

			ApplySetting(
				Trim(line.substr(0, separator)),
				Trim(line.substr(separator + 1))
			);
		}

		return true;
	}

	// Loads the session config from beside the DLL. Every field is optional and
	// anything missing keeps its default, so the mod is fully playable on a LAN
	// with no config file at all.
	//
	// Three cases, in order:
	//   1. HowManyDudesMultiplayer.ini exists      -> read it.
	//   2. only the shipped .ini.example exists    -> read that, then write a
	//      real .ini, so copying the example without renaming it still works.
	//   3. neither exists                          -> write a default .ini.
	void LoadConfig(const fs::path& ModulePath)
	{
		fs::path config_path = ModulePath;
		config_path.replace_extension(".ini");

		if (!ReadConfigFile(config_path))
		{
			fs::path example_path = config_path;
			example_path += ".example";

			if (ReadConfigFile(example_path))
			{
				hmd::LogInfo("read settings from %s",
					example_path.filename().string().c_str());
			}

			WriteDefaultConfig(config_path);
		}

		hmd::LogInfo("config: peer=%s port=%u discovery=%d key=%s "
			"auto_host=%d auto_join=%d duel_interval=%d sync_run_start=%d",
			g_Config.peer_address.c_str(),
			g_Config.port,
			g_Config.enable_discovery ? 1 : 0,
			// Never log the passphrase itself.
			g_Config.session_key.empty() ? "none" : "set",
			g_Config.auto_host ? 1 : 0,
			g_Config.auto_join ? 1 : 0,
			g_Config.duel_interval,
			g_Config.sync_run_start ? 1 : 0);
	}

	// Joining is either discovery-driven or address-driven, and both hotkeys
	// and auto_join need the same decision.
	void BeginJoin()
	{
		if (JoiningByDiscovery())
		{
			if (!g_Config.enable_discovery)
			{
				hmd::LogError("peer_address is '%s' but enable_discovery is off "
					"- set one or the other in the ini",
					g_Config.peer_address.c_str());
				return;
			}

			hmd::LogStage(hmd::kStageConnect,
				"searching the local network for a host on port %u",
				g_Config.port);
			hmd::net::JoinDiscovered(g_Config.port);
			return;
		}

		hmd::LogStage(hmd::kStageConnect, "joining %s:%u",
			g_Config.peer_address.c_str(), g_Config.port);
		hmd::net::Join(g_Config.peer_address, g_Config.port);
	}

	void PrintStatus()
	{
		hmd::match::Lives lives = hmd::match::CurrentLives();
		const std::string peer = hmd::net::PeerAddress();
		const hmd::match::PeerPresence presence = hmd::match::CurrentPeerPresence();
		const std::string room = hmd::bridge::CurrentRoomName();
		const bool local_in_run = hmd::match::LocalInRun();

		const char* opponent =
			!presence.known ? "no word from them" :
			presence.in_run ? "in a run" : "not in a run";

		// Their beacon first, then whoever Steam says is in the lobby with us.
		// The second is what stops this reading "opponent ?" when the link is
		// up but their messages are not arriving - the name is knowable from
		// the lobby alone, and printing "?" made a transport problem look like
		// an identity problem.
		std::string opponent_name = presence.name;
		if (opponent_name.empty())
			opponent_name = hmd::steam::PeerName();

		hmd::LogInfo("status: link=%s sending=%s steam=%s peer=%s phase=%s "
			"lives local=%d remote=%d room=%s",
			hmd::net::StateName(),
			// Spelled out because the two are independent: incoming messages are
			// drained whatever our own state is, so "I can see my opponent" is
			// not evidence that they can see me.
			hmd::net::IsConnected() ? "yes" : "NO",
			hmd::steam::Available() ? hmd::steam::StateName() : "unavailable",
			peer.empty() ? "none" : peer.c_str(),
			hmd::match::PhaseName(),
			lives.local,
			lives.remote,
			room.c_str());

		hmd::LogInfo("status: you are %s (round %d) | opponent %s is %s "
			"(act %d, round %d)",
			local_in_run ? "in a run" : "not in a run",
			hmd::runstate::CurrentRound(),
			opponent_name.empty() ? "?" : opponent_name.c_str(),
			opponent,
			presence.act,
			presence.round);

		hmd::LogInfo("status: duels every %d rounds | next duel at round %d | "
			"on-screen messages %s",
			hmd::runstate::DuelInterval(),
			hmd::runstate::NextDuelRound(hmd::runstate::CurrentRound() + 1),
			hmd::ui::NotificationsAvailable() ? "working" : "not confirmed");

		std::string error = hmd::net::LastError();
		if (!error.empty())
			hmd::LogInfo("last network error: %s", error.c_str());
	}

	// Hotkeys are split across two functions, and the split is not cosmetic.
	//
	// WasKeyPressed reads GetAsyncKeyState's "pressed since last call" bit, so
	// a key polled from two places has its edge consumed by whichever poll runs
	// first and swallowed for the other. Each key is therefore polled in exactly
	// one of these, and which one it lives in decides whether it still works
	// when the safe window for calling into the game closes.
	//
	// Connection keys go here because none of them touch the game: Steam.cpp and
	// Net.cpp between them make zero bridge:: calls. That matters most for F11 -
	// if the game-facing pump ever stalls, disconnecting has to still work.
	void HandleConnectionHotkeys()
	{
		// F7 is the whole Steam flow in one key: open a lobby if there isn't
		// one, then show Steam's own invite dialog. The friend clicks the
		// invite and both sides connect over Steam's relay - no ports, no
		// addresses, and it works over the internet.
		if (WasKeyPressed(kKeyInvite))
		{
			if (!hmd::steam::Available())
			{
				hmd::LogWarn("Steam is unavailable - use F9/F10 for a direct "
					"session instead");
			}
			else if (hmd::steam::State() == hmd::steam::LobbyState::None)
			{
				hmd::LogStage(hmd::kStageConnect,
					"F7 - opening a Steam lobby, press F7 again to invite");
				hmd::steam::HostSession();
			}
			else
			{
				hmd::steam::OpenInviteOverlay();
			}
		}

		if (WasKeyPressed(kKeyHost))
		{
			hmd::LogStage(hmd::kStageConnect, "F9 - hosting on port %u", g_Config.port);
			hmd::net::Host(g_Config.port);
		}

		if (WasKeyPressed(kKeyJoin))
			BeginJoin();

		if (WasKeyPressed(kKeyDisconnect))
		{
			hmd::LogStage(hmd::kStageConnect, "F11 - disconnecting");
			hmd::steam::LeaveSession();
			hmd::net::Disconnect();
		}
	}

	// The reporting keys. Both read the game - PrintStatus through
	// bridge::CurrentRoomName and the round tracker, probe::Report through a
	// pile of builtins - so they are only polled from inside the safe window.
	void HandleReportingHotkeys()
	{
		if (WasKeyPressed(kKeyStatus))
			PrintStatus();

		if (WasKeyPressed(kKeyProbe))
			hmd::probe::Report();

		if (WasKeyPressed(kKeySelfTest))
		{
			if (hmd::bridge::CurrentRoomName() != "rm_gameplay")
			{
				hmd::LogWarn("F5 - the duel self-test needs a live round; press "
					"it during a fight");
			}
			else
			{
				hmd::roster::SelfTestDuelPayload();
			}
		}
	}

	// Everything the mod does per-tick funnels through here.
	//
	// It is driven by EVENT_OBJECT_CALL rather than EVENT_FRAME. EVENT_FRAME is
	// an IDXGISwapChain::Present hook, and YYToolkit's Zeus generation does not
	// install one against this game - aurie.log shows the runner-interface and
	// code-execution hooks going in and nothing for the swapchain, and a probe
	// confirmed the frame callback is never delivered. EVENT_OBJECT_CALL fires
	// from the Code_Execute hook, which Zeus definitely installs.
	//
	// That event fires many times per frame, so the work is throttled: the
	// state machines want to run at roughly frame rate, not once per GML call.
	//
	// ---------------------------------------------------------------------
	// Why this is split in two
	// ---------------------------------------------------------------------
	//
	// EVENT_OBJECT_CALL fires before EVERY code entry, including ones nested
	// deep inside engine primitives. The whole pump used to run from all of
	// them, which meant the mod could call back into the runtime at any point
	// inside any engine operation. That killed a tester's game:
	//
	//     argument is not a method, unable to call
	//     - gml_Object_o_combatant_Destroy_0:2
	//     - gml_Object_o_enemy_Destroy_0:2
	//     - gml_Script_instance_create:13
	//     - gml_Script_enemy_spawn:97
	//     - gml_Object_o_gameplay_Create_0:263
	//
	// The engine was inside instance_create_depth, running a fresh instance's
	// event, when the hook fired the pump and the mod called straight back in.
	// The half-built o_enemy was destroyed before its methods existed.
	//
	// So the work is now sorted by whether it touches the game:
	//
	//   PumpConnection - Steam and the transport. Steam.cpp and Net.cpp make
	//                    zero bridge:: calls between them, so none of this can
	//                    re-enter the runtime and it runs from every code entry
	//                    exactly as before.
	//
	//   PumpGame       - the state machines, the overlay and the probe. All of
	//                    them call into the game, so this runs only when the
	//                    engine is between top-level code entries.
	//
	// The cost of being wrong in the safe direction is a feature going quiet.
	// The cost of being wrong in the other direction is the stack above.
	void PumpConnection()
	{
		using clock = std::chrono::steady_clock;
		constexpr auto kInterval = std::chrono::milliseconds(16);

		static clock::time_point last{};
		const auto now = clock::now();
		if (now - last < kInterval)
			return;
		last = now;

		HandleConnectionHotkeys();
		hmd::steam::Tick();
	}

	void PumpGame()
	{
		using clock = std::chrono::steady_clock;
		constexpr auto kInterval = std::chrono::milliseconds(16);

		static clock::time_point last{};
		const auto now = clock::now();
		if (now - last < kInterval)
			return;
		last = now;

		static bool announced = false;
		if (!announced)
		{
			announced = true;
			hmd::LogInfo("tick is live - hotkeys active");
		}

		HandleReportingHotkeys();

		// Order matters here. runstate ages out the run-map cells the overlay
		// draws against, ui refreshes the per-frame draw budget, and match is
		// what decides what the overlay should be showing - so match runs last
		// and its output is drawn on the frames that follow.
		hmd::runstate::Tick();
		hmd::ui::Tick();
		hmd::match::Tick();
		hmd::probe::Tick();
	}

	// ---------------------------------------------------------------------
	// The safe window
	// ---------------------------------------------------------------------
	//
	// How deep inside the engine's GML execution this thread currently is. Zero
	// means the engine is between top-level code entries, which is the only
	// point at which it is safe for the mod to call back into it.
	//
	// Maintaining this requires calling the original ourselves rather than
	// leaving YYToolkit to do it, which is the one behavioural change here: the
	// game's code still runs exactly as it would, but it now runs inside our
	// frame instead of after it.
	thread_local int g_CodeDepth = 0;

	// Set while PumpGame is running, because the calls it makes into the game
	// come straight back through this callback. Without it the pump would
	// re-enter itself at what still looks like depth zero.
	thread_local bool g_InPumpGame = false;

	// The gate is only as good as its assumption that depth-zero entries
	// actually arrive, and that assumption is about a runner nobody can read the
	// bytecode of - so it is measured rather than trusted. probe::NoteSafeWindow
	// keeps the counts and F6 reports them.

	// A depth counter that only ever counts up is a mod that goes permanently
	// silent. That is not hypothetical: the decrement below is skipped if the
	// runtime leaves our frame without returning through it, which is exactly
	// what GameMaker's own fatal error handling does. No legitimate nest of GML
	// calls stays open for a second, so one that has is assumed leaked and the
	// counter is put back to zero.
	constexpr auto kDepthLeakTimeout = std::chrono::seconds(1);

	// How long the game-facing pump may go unrun before saying so. Being quiet
	// is a survivable outcome and a crash is not, so this reports rather than
	// forcing the pump - but it must report, because a mod that silently does
	// nothing is indistinguishable from one that is broken.
	constexpr auto kPumpStarvedAfter = std::chrono::seconds(5);

	void CodeCallback(FWCodeEvent& CodeContext)
	{
		using clock = std::chrono::steady_clock;

		// The census is how the mod learns whether this runner exposes named
		// code entries at all, which decides whether object Draw events are a
		// viable hook target on this build. It closes itself after a few
		// seconds and costs nothing afterwards.
		// FWCodeEvent wraps bool(CInstance*, CInstance*, CCode*, int, RValue*),
		// so the code entry is the third element of the argument tuple.
		if (hmd::probe::CensusOpen())
			hmd::probe::NoteCodeEntry(std::get<2>(CodeContext.Arguments()));

		const auto now = clock::now();

		// Recover a leaked counter before reading it, so the recovery cannot
		// itself be starved by the condition it exists to clear.
		static thread_local clock::time_point depth_opened_at{};
		if (g_CodeDepth > 0 && (now - depth_opened_at) > kDepthLeakTimeout)
		{
			g_CodeDepth = 0;
			g_InPumpGame = false;
			hmd::probe::NoteDepthReset();
		}

		// Never gated. Nothing in here can re-enter the runtime.
		PumpConnection();

		// Both of these are only ever touched from this callback, which is only
		// ever entered on the game thread.
		static clock::time_point last_safe_pump = now;
		static clock::time_point last_starve_warning = now;

		const bool safe = (g_CodeDepth == 0) && !g_InPumpGame;

		hmd::probe::NoteSafeWindow(g_CodeDepth, safe);

		if (safe)
		{
			last_safe_pump = now;

			g_InPumpGame = true;
			PumpGame();
			g_InPumpGame = false;
		}
		else if ((now - last_safe_pump) > kPumpStarvedAfter &&
			(now - last_starve_warning) > kPumpStarvedAfter)
		{
			last_starve_warning = now;
			hmd::LogWarn("no safe window to talk to the game has opened in %llds "
				"- duel logic and the overlay are idle. F7/F9/F10/F11 still "
				"work. Press F6 for the entry counts.",
				static_cast<long long>(
					std::chrono::duration_cast<std::chrono::seconds>(
						now - last_safe_pump).count()));
		}

		// Track the nest around the original. The guard is deliberately plain
		// rather than RAII: a C++ destructor would not run on the paths that
		// actually skip this decrement (the runtime's fatal handler does not
		// unwind), so the leak timeout above is the real safety net and a
		// scope guard here would only make it look otherwise.
		if (g_CodeDepth == 0)
			depth_opened_at = now;

		g_CodeDepth++;

		CodeContext.Call();

		g_CodeDepth--;
	}

	// Kept registered in case a future YYToolkit does hook Present. It is not
	// delivered on this build - see the comment on PumpConnection.
	//
	// Connection work only, deliberately. EVENT_FRAME is an
	// IDXGISwapChain::Present hook, and nothing here has established which
	// thread that runs on for this runner. Calling into the game from the wrong
	// one would be a fresh crash of exactly the kind this change exists to
	// remove, and the game-facing pump loses nothing by being driven solely
	// from the depth gate in CodeCallback.
	void FrameCallback(FWFrame& FrameContext)
	{
		UNREFERENCED_PARAMETER(FrameContext);

		PumpConnection();
	}

	// Diagnostic log written beside the DLL.
	//
	// Every other logging path in this mod goes through the YYToolkit
	// interface, which is useless for diagnosing the one failure that matters
	// most at load time: not being able to obtain that interface at all. When
	// that happens Aurie purges the module and the only evidence is a single
	// generic line in aurie.log. This file is the fallback channel.
	void BootLog(const fs::path& ModulePath, const char* Format, ...)
	{
		fs::path log_path = ModulePath;
		log_path.replace_extension(".boot.log");

		// Truncate on the first write of each run so the file always describes
		// the launch the player just made, rather than growing forever.
		static bool started = false;
		std::ofstream file(log_path, started ? std::ios::app : std::ios::trunc);
		started = true;

		if (!file.is_open())
			return;

		char message[1024]{};
		va_list arguments;
		va_start(arguments, Format);
		_vsnprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format, arguments);
		va_end(arguments);

		file << message << '\n';
	}
}

EXPORTED AurieStatus ModuleInitialize(
	IN AurieModule* Module,
	IN const fs::path& ModulePath
)
{
	g_Module = Module;
	g_ModulePath = ModulePath;

	BootLog(ModulePath, "--- ModuleInitialize ---");

	// GetInterface() knows which interface name this SDK generation expects
	// ("YYTK_ZeusMain" as of the Zeus rewrite), so resolve through it rather
	// than naming the interface here and drifting the next time it changes.
	hmd::g_Interface = YYTK::GetInterface();

	BootLog(ModulePath, "YYTK::GetInterface() -> %p",
		reinterpret_cast<void*>(hmd::g_Interface));

	if (!hmd::g_Interface)
	{
		BootLog(ModulePath, "giving up: YYToolkit interface unavailable");
		return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;
	}

	BootLog(ModulePath, "YYToolkit interface acquired");

	hmd::LogInfo("HowManyDudesMultiplayer loading...");
	hmd::LogInfo("Copyright (C) 2026 Braden Atzert. Free software under AGPL-3.0-or-later,");
	hmd::LogInfo("with ABSOLUTELY NO WARRANTY. Source: " HMD_SOURCE_URL);

	if (!hmd::bridge::Initialize(hmd::g_Interface))
	{
		hmd::LogError("could not initialise the game bridge");
		return AURIE_MODULE_INITIALIZATION_FAILED;
	}

	LoadConfig(ModulePath);

	hmd::net::Configure(hmd::net::Options{
		g_Config.session_key,
		g_Config.enable_discovery
	});

	if (!hmd::net::Initialize())
	{
		hmd::LogError("networking failed to start - multiplayer unavailable");
		return AURIE_MODULE_INITIALIZATION_FAILED;
	}

	// Optional by design: a failure here costs the invite flow, not the mod.
	// Direct TCP sessions keep working either way.
	hmd::steam::Initialize();

	// Run position and the overlay come up before the match layer, because
	// match hands both of them work on its very first tick.
	hmd::runstate::SetDuelInterval(g_Config.duel_interval);
	hmd::runstate::Initialize(Module);
	hmd::ui::Initialize(Module);
	hmd::ui::SetNotificationsEnabled(g_Config.on_screen_messages);

	hmd::match::SetSyncRunStart(g_Config.sync_run_start);

	if (!hmd::match::Initialize(Module))
	{
		hmd::LogError("match subsystem failed to start");
		hmd::ui::Shutdown(Module);
		hmd::runstate::Shutdown(Module);
		hmd::net::Shutdown();
		return AURIE_MODULE_INITIALIZATION_FAILED;
	}

	// The code-execution callback is the one that must succeed. It is the only
	// thing that drives either pump on this build, and it is also where the
	// safe window is measured - see CodeCallback.
	AurieStatus status = hmd::g_Interface->CreateCallback(
		Module,
		EVENT_OBJECT_CALL,
		CodeCallback,
		0
	);

	if (!AurieSuccess(status))
	{
		hmd::LogError("could not register the code callback (status %d) - "
			"the mod cannot run", static_cast<int>(status));
		hmd::match::Shutdown(Module);
		hmd::ui::Shutdown(Module);
		hmd::runstate::Shutdown(Module);
		hmd::steam::Shutdown();
		hmd::net::Shutdown();
		return AURIE_MODULE_INITIALIZATION_FAILED;
	}

	// Best-effort. Not delivered by the current YYToolkit, and not required.
	hmd::g_Interface->CreateCallback(Module, EVENT_FRAME, FrameCallback, 0);

	if (g_Config.auto_host)
		hmd::net::Host(g_Config.port);
	else if (g_Config.auto_join)
		BeginJoin();

	if (hmd::steam::Available())
	{
		hmd::LogInfo("ready. F7 invite a Steam friend | F9 host | F10 join | "
			"F11 disconnect | F8 status | F6 diagnostics | F5 duel self-test");
		hmd::LogInfo("easiest way to play: press F7 twice - once to open a "
			"lobby, again to pick a friend. No ports, works over the internet.");
	}
	else
	{
		hmd::LogInfo("ready. F9 host | F10 join | F11 disconnect | F8 status | "
			"F6 diagnostics");

		if (JoiningByDiscovery() && g_Config.enable_discovery)
		{
			hmd::LogInfo("one player presses F9, the other presses F10 - "
				"no addresses needed on a local network.");
		}
	}

	return AURIE_SUCCESS;
}

EXPORTED AurieStatus ModuleUnload(
	IN AurieModule* Module,
	IN const fs::path& ModulePath
)
{
	UNREFERENCED_PARAMETER(ModulePath);

	hmd::LogInfo("unloading");

	hmd::match::Shutdown(Module);
	hmd::ui::Shutdown(Module);
	hmd::runstate::Shutdown(Module);
	hmd::steam::Shutdown();
	hmd::net::Shutdown();

	hmd::g_Interface = nullptr;
	g_Module = nullptr;

	return AURIE_SUCCESS;
}
