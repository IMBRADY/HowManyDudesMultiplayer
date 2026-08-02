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
#define HMD_SOURCE_URL "https://github.com/YOUR-USERNAME/how-many-dudes-multiplayer"

#include "GameBridge.h"
#include "Json.h"
#include "Log.h"
#include "Match.h"
#include "Net.h"
#include "Roster.h"
#include "Steam.h"

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
			"auto_join = false\n";

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
			"auto_host=%d auto_join=%d",
			g_Config.peer_address.c_str(),
			g_Config.port,
			g_Config.enable_discovery ? 1 : 0,
			// Never log the passphrase itself.
			g_Config.session_key.empty() ? "none" : "set",
			g_Config.auto_host ? 1 : 0,
			g_Config.auto_join ? 1 : 0);
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

		hmd::LogInfo("status: link=%s steam=%s peer=%s phase=%s lives local=%d "
			"remote=%d room=%s",
			hmd::net::StateName(),
			hmd::steam::Available() ? hmd::steam::StateName() : "unavailable",
			peer.empty() ? "none" : peer.c_str(),
			hmd::match::PhaseName(),
			lives.local,
			lives.remote,
			room.c_str());

		hmd::LogInfo("status: you are %s | opponent is %s (act %d)",
			local_in_run ? "in a run" : "not in a run",
			opponent,
			presence.act);

		std::string error = hmd::net::LastError();
		if (!error.empty())
			hmd::LogInfo("last network error: %s", error.c_str());
	}

	void HandleHotkeys()
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

		if (WasKeyPressed(kKeyStatus))
			PrintStatus();
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
	void PumpOnce()
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

		HandleHotkeys();
		hmd::steam::Tick();
		hmd::match::Tick();
	}

	// Observe-only. Not calling Call() leaves YYToolkit to invoke the original,
	// so the game's own code runs exactly as it would without the mod.
	void CodeCallback(FWCodeEvent& CodeContext)
	{
		UNREFERENCED_PARAMETER(CodeContext);
		PumpOnce();
	}

	// Kept registered in case a future YYToolkit does hook Present. Both paths
	// share the throttle above, so being called from both costs nothing.
	void FrameCallback(FWFrame& FrameContext)
	{
		UNREFERENCED_PARAMETER(FrameContext);
		PumpOnce();
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

	if (!hmd::match::Initialize(Module))
	{
		hmd::LogError("match subsystem failed to start");
		hmd::net::Shutdown();
		return AURIE_MODULE_INITIALIZATION_FAILED;
	}

	// The code-execution callback is the one that must succeed - see PumpOnce.
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
			"F11 disconnect | F8 status");
		hmd::LogInfo("easiest way to play: press F7 twice - once to open a "
			"lobby, again to pick a friend. No ports, works over the internet.");
	}
	else
	{
		hmd::LogInfo("ready. F9 host | F10 join | F11 disconnect | F8 status");

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
	hmd::steam::Shutdown();
	hmd::net::Shutdown();

	hmd::g_Interface = nullptr;
	g_Module = nullptr;

	return AURIE_SUCCESS;
}
