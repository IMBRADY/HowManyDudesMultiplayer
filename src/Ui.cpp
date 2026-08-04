// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Ui.h"
#include "GameBridge.h"
#include "GameHooks.h"
#include "Log.h"
#include "RunState.h"
#include "Sanitize.h"

#include <atomic>
#include <cstdarg>
#include <mutex>
#include <string>

using namespace YYTK;
using namespace Aurie;

namespace hmd::ui
{
	namespace
	{
		// Where the overlay hangs itself.
		//
		// Object Draw events cannot be used: YYC compiles them to native
		// functions with no resolvable name. So the anchor has to be a
		// top-level script that runs inside the draw pipeline.
		//
		// cf_draw looked like the obvious one - it is the cakeframe system's
		// draw entry point. It is never called. The hook installed cleanly and
		// its detour did not fire once in a whole session, which is why nothing
		// was drawn. Rather than guess again, several plausible anchors are
		// hooked and whichever the game actually calls wins. The probe reports
		// which one is delivering.
		struct DrawAnchor
		{
			const char* script;
			const char* hook_id;
			YYTK::PFUNC_YYGMLScript trampoline;
			unsigned long long calls;
		};

		DrawAnchor g_Anchors[] = {
			// The cakeframe sprite/text drawing helpers. These are what the
			// renderers call to put pixels down, so they are unambiguously
			// inside the draw pipeline.
			{ "gml_Script_cakeframe_draw_sprite_ext", "hmd_draw_a", nullptr, 0 },
			{ "gml_Script_cakeframe_draw_text_to_fit", "hmd_draw_b", nullptr, 0 },
			{ "gml_Script_cakeframe_draw_sprite_to_fit", "hmd_draw_c", nullptr, 0 },
			// Generic drawing helpers used across the game's own UI.
			{ "gml_Script_draw_sprite_centered_ext", "hmd_draw_d", nullptr, 0 },
			{ "gml_Script_drawtext", "hmd_draw_e", nullptr, 0 },
			// Kept for completeness; never observed firing on this build.
			{ "gml_Script_cf_draw", "hmd_draw_f", nullptr, 0 },
		};

		constexpr size_t kAnchorCount = sizeof(g_Anchors) / sizeof(g_Anchors[0]);

		// The game's own on-screen message stream.
		constexpr const char* kInfostreamScript = "gml_Script_hmd_infostream_spawn";

		// The "regular dude head" icon. Confirmed present in the sprite table.
		constexpr const char* kHeadSprite = "sp_icon_dude_regular";

		// GameMaker draw-state constants. Named here rather than inlined so the
		// save/restore below reads as what it is.
		constexpr double kAlignLeft = 0.0;
		constexpr double kAlignRight = 2.0;
		constexpr double kAlignTop = 0.0;
		constexpr double kAlignMiddle = 1.0;
		constexpr double kColourWhite = 16777215.0;
		constexpr double kColourBlack = 0.0;

		// cf_draw fires once per top-level frame, which is several times per
		// rendered frame. The overlay is drawn on more than one of them so that
		// it ends up over whatever the game drew after the first, and this
		// bounds the cost of doing that. Every draw is a dozen builtin calls,
		// so the budget is small on purpose.
		constexpr int kDrawBudgetPerTick = 4;

		std::mutex g_Mutex;
		Overlay g_Overlay;

		// Read on the draw path without taking the lock. The overlay is only
		// worth any work at all when a peer is connected, and that is by far
		// the most common reason to do nothing.
		std::atomic<bool> g_Connected{ false };

		// Cached once a tick rather than re-queried per cf_draw call, which
		// would otherwise cost two builtin calls every time.
		std::mutex g_RoomMutex;
		std::string g_Room;

		std::atomic<int> g_DrawBudget{ 0 };

		// Set while the overlay is drawing, so a draw call we make from inside
		// a draw-helper hook cannot re-enter the same hook and recurse.
		thread_local bool g_InsideOverlay = false;

		std::atomic<bool> g_NotificationsWork{ false };
		std::atomic<bool> g_NotificationsTried{ false };

		// Kill switch for the on-screen message stream. It calls a game routine
		// whose signature YYC did not preserve, so if it turns out to dislike
		// being called from here it can be turned off from the ini rather than
		// needing a new build. Messages still reach the log either way.
		std::atomic<bool> g_NotificationsEnabled{ true };


		// ---------------------------------------------------------------
		// Draw primitives
		// ---------------------------------------------------------------
		//
		// All of these go through builtins rather than the game's own drawing
		// helpers. The game's helpers have signatures YYC did not preserve, and
		// calling one with the wrong shape is exactly the crash-to-desktop the
		// brief forbids. Builtins have signatures that are part of GameMaker
		// itself and cannot drift.

		// The draw state cf_draw was in when we interrupted it. Restoring this
		// matters: the cakeframe renderers that run after us inherit whatever
		// alignment and colour are current, and leaving ours behind would
		// recolour the game's own UI.
		struct DrawState
		{
			double halign = kAlignLeft;
			double valign = kAlignTop;
			double colour = kColourWhite;
			double alpha = 1.0;
			bool captured = false;
		};

		double ReadDouble(const char* Builtin, double Fallback)
		{
			auto value = bridge::CallBuiltin(Builtin, {});
			if (!value)
				return Fallback;

			switch (value->m_Kind)
			{
			case VALUE_REAL:
			case VALUE_INT32:
			case VALUE_INT64:
			case VALUE_BOOL:
				return value->ToDouble();
			default:
				return Fallback;
			}
		}

		DrawState CaptureDrawState()
		{
			DrawState state;
			state.halign = ReadDouble("draw_get_halign", kAlignLeft);
			state.valign = ReadDouble("draw_get_valign", kAlignTop);
			state.colour = ReadDouble("draw_get_colour", kColourWhite);
			state.alpha = ReadDouble("draw_get_alpha", 1.0);
			state.captured = true;
			return state;
		}

		void RestoreDrawState(const DrawState& State)
		{
			if (!State.captured)
				return;

			bridge::CallBuiltin("draw_set_halign", { RValue(State.halign) });
			bridge::CallBuiltin("draw_set_valign", { RValue(State.valign) });
			bridge::CallBuiltin("draw_set_colour", { RValue(State.colour) });
			bridge::CallBuiltin("draw_set_alpha", { RValue(State.alpha) });
		}

		// Draws text with a hard drop shadow, which is what keeps it legible
		// over whatever the game happens to be drawing underneath. The font is
		// deliberately not set: at this point in the pipeline the current font
		// is one the game itself just used, so inheriting it is both the right
		// look and the option with no failure mode.
		void DrawText(
			double X,
			double Y,
			const std::string& Text,
			double Halign,
			double Valign,
			double Scale
		)
		{
			if (Text.empty())
				return;

			const RValue text(Text);

			bridge::CallBuiltin("draw_set_halign", { RValue(Halign) });
			bridge::CallBuiltin("draw_set_valign", { RValue(Valign) });
			bridge::CallBuiltin("draw_set_alpha", { RValue(1.0) });

			bridge::CallBuiltin("draw_set_colour", { RValue(kColourBlack) });
			bridge::CallBuiltin(
				"draw_text_transformed",
				{
					RValue(X + 2.0), RValue(Y + 2.0), text,
					RValue(Scale), RValue(Scale), RValue(0.0)
				}
			);

			bridge::CallBuiltin("draw_set_colour", { RValue(kColourWhite) });
			bridge::CallBuiltin(
				"draw_text_transformed",
				{
					RValue(X), RValue(Y), text,
					RValue(Scale), RValue(Scale), RValue(0.0)
				}
			);
		}

		int HeadSpriteIndex()
		{
			static int cached = -2;
			if (cached == -2)
				cached = bridge::AssetIndex(kHeadSprite);
			return cached;
		}

		// Draws the dude head centred on (X, Y) at a given on-screen height.
		// Scaling from the sprite's real dimensions rather than assuming them
		// keeps the marker the right size whatever resolution the player is on.
		void DrawHead(double CenterX, double CenterY, double TargetHeight)
		{
			const int sprite = HeadSpriteIndex();
			if (sprite < 0)
				return;

			auto height = bridge::CallBuiltin(
				"sprite_get_height", { RValue(static_cast<double>(sprite)) });

			double scale = 1.0;
			if (height && height->ToDouble() > 0.0)
				scale = TargetHeight / height->ToDouble();

			bridge::CallBuiltin(
				"draw_sprite_ext",
				{
					RValue(static_cast<double>(sprite)),
					RValue(0.0),
					RValue(CenterX),
					RValue(CenterY),
					RValue(scale),
					RValue(scale),
					RValue(0.0),
					RValue(kColourWhite),
					RValue(1.0)
				}
			);
		}

		// ---------------------------------------------------------------
		// Overlay elements
		// ---------------------------------------------------------------

		// Feature 2: who you are connected to, top right of the main menu.
		void DrawPeerBadge(const Overlay& Snapshot, const runstate::Box& Space)
		{
			const double height = Space.Height();
			const double margin = height * 0.03;
			const double head_size = height * 0.06;

			const double right = Space.right - margin;
			const double top = Space.top + margin;

			// The head sits at the far right, the name to its left, so the
			// block grows leftwards and never runs off the edge.
			const double head_x = right - head_size * 0.5;
			const double centre_y = top + head_size * 0.5;

			DrawHead(head_x, centre_y, head_size);

			const std::string name = Snapshot.peer_name.empty()
				? std::string("OPPONENT")
				: Snapshot.peer_name;

			DrawText(
				head_x - head_size * 0.75,
				centre_y,
				name,
				kAlignRight,
				kAlignMiddle,
				height / 720.0
			);
		}

		// Why the opponent marker did or did not get drawn.
		//
		// The marker has never been seen on screen, and there are at least three
		// independent reasons it might not be - no cell measured, the peer not
		// tracked, or the cell being in a different coordinate space from the one
		// drawn in. Guessing between them from the outside is what this exists to
		// stop. Rate limited, and it reports immediately whenever the reason
		// changes so a transition is never hidden behind the interval.
		void NoteMarkerState(const char* Reason, const runstate::Box* Cell,
			const runstate::Box* Space, double DrawY)
		{
			using clock = std::chrono::steady_clock;
			constexpr auto kInterval = std::chrono::seconds(10);

			static const char* last_reason = nullptr;
			static clock::time_point last_logged{};

			const auto now = clock::now();
			const bool changed = (Reason != last_reason);

			if (!changed && (now - last_logged) < kInterval)
				return;

			last_reason = Reason;
			last_logged = now;

			if (!Cell || !Space)
			{
				LogInfo("marker: not drawn - %s", Reason);
				return;
			}

			// Both boxes on one line on purpose. If the cell numbers are not on
			// the same scale as the draw space, the marker is being drawn in a
			// coordinate system it does not belong to, and that is visible here
			// and nowhere else.
			LogInfo("marker: %s | cell %.0f,%.0f..%.0f,%.0f -> head at %.0f,%.0f "
				"| draw space %.0f,%.0f..%.0f,%.0f",
				Reason,
				Cell->left, Cell->top, Cell->right, Cell->bottom,
				Cell->CenterX(), DrawY,
				Space->left, Space->top, Space->right, Space->bottom);

			const bool outside =
				Cell->CenterX() < Space->left || Cell->CenterX() > Space->right ||
				DrawY < Space->top || DrawY > Space->bottom;

			if (outside)
			{
				LogWarn("marker: that lands OUTSIDE the drawable area - the cell "
					"box and the draw call are in different coordinate spaces, "
					"which no amount of nudging the offset will fix");
			}
		}

		// Feature 4: where your opponent is on the round track.
		void DrawPeerRoundMarker(const Overlay& Snapshot, const runstate::Box& Space)
		{
			if (Snapshot.peer_round <= 0 || !Snapshot.peer_in_run)
			{
				NoteMarkerState("opponent is not in a run we can place",
					nullptr, nullptr, 0.0);
				return;
			}

			runstate::Box cell;
			if (!runstate::CellBox(Snapshot.peer_round, cell))
			{
				NoteMarkerState("no run-map cell has been measured for their round",
					nullptr, nullptr, 0.0);
				return;
			}

			const double head_size = cell.Height() * 0.9;

			// Below the cell rather than above it.
			//
			// Above was the original choice and it reads better - it points down
			// at the cell it means. It is also the edge the run map runs closest
			// to, so a marker on the top row had nowhere to go. Below is the
			// safer side, and the marker has never been seen on screen, so the
			// safer side is worth more than the nicer reading right now.
			const double head_y = cell.bottom + head_size * 0.6;

			NoteMarkerState("drawing", &cell, &Space, head_y);

			DrawHead(cell.CenterX(), head_y, head_size);
		}

		// Feature 3's visible half: why the arena is empty while you wait.
		void DrawBanner(const Overlay& Snapshot, const runstate::Box& Space)
		{
			if (Snapshot.banner.empty())
				return;

			DrawText(
				Space.CenterX(),
				Space.top + Space.Height() * 0.14,
				Snapshot.banner,
				1.0,               // fa_center
				kAlignMiddle,
				Space.Height() / 540.0
			);
		}

		void DrawOverlay()
		{
			Overlay snapshot;
			{
				std::lock_guard<std::mutex> lock(g_Mutex);
				snapshot = g_Overlay;
			}

			// The GUI surface's own dimensions. Two builtins, no guessing, and
			// correct wherever the interface is drawn - which is where the
			// anchors fire, since they are the UI's own drawing helpers.
			const double width = ReadDouble("display_get_gui_width", 0.0);
			const double height = ReadDouble("display_get_gui_height", 0.0);

			if (width < 2.0 || height < 2.0)
				return;

			runstate::Box space;
			space.left = 0.0;
			space.top = 0.0;
			space.right = width;
			space.bottom = height;
			space.valid = true;

			std::string room;
			{
				std::lock_guard<std::mutex> lock(g_RoomMutex);
				room = g_Room;
			}

			const DrawState saved = CaptureDrawState();

			if (room == "rm_mainmenu")
			{
				DrawPeerBadge(snapshot, space);
			}
			else
			{
				DrawPeerRoundMarker(snapshot, space);
				DrawBanner(snapshot, space);
			}

			RestoreDrawState(saved);
		}

		// ---------------------------------------------------------------
		// Hooks
		// ---------------------------------------------------------------

		// Shared body for every anchor. Called after the original has run, so
		// whatever we add lands on top of it.
		void OnDrawCall(size_t AnchorIndex)
		{
			g_Anchors[AnchorIndex].calls++;

			// Nothing below is worth doing without a peer, and no peer is the
			// overwhelmingly common case - the game spends most of its life
			// with this mod loaded and idle.
			if (!g_Connected.load(std::memory_order_relaxed))
				return;

			// The overlay draws by calling the game's own drawing builtins. If
			// one of those routes back through an anchor, this stops the hook
			// from re-entering itself and recursing until the stack runs out.
			if (g_InsideOverlay)
				return;

			// Budget is consumed whether or not anything is drawn, so a frame
			// with hundreds of draw calls cannot make this expensive. Floored
			// rather than left to run negative between ticks.
			int budget = g_DrawBudget.load(std::memory_order_relaxed);
			if (budget <= 0)
				return;
			g_DrawBudget.store(budget - 1, std::memory_order_relaxed);

			// The anchor's first argument is deliberately NOT inspected here.
			//
			// It used to be handed to the cakeframe box accessors on the theory
			// that a draw call might be given the frame it is drawing. Some
			// are; draw_sprite_centered_ext is handed a sprite reference, and
			// passing that to cf_get_box killed the game the instant a peer
			// connected and the overlay first ran. Same lesson as calling
			// wave_real to see what it returned: do not hand game code a value
			// it did not ask for. The overlay uses the GUI surface's own
            // dimensions instead, which need no guessing.
			g_InsideOverlay = true;
			DrawOverlay();
			g_InsideOverlay = false;
		}

		// One detour per anchor. They differ only in which trampoline they call
		// and which index they report, but each needs its own function address
		// to hook, so the repetition is unavoidable and kept mechanical.
		#define HMD_DEFINE_DRAW_DETOUR(Index)                                  \
			RValue& DrawDetour##Index(                                         \
				CInstance* Self, CInstance* Other, RValue& Result,             \
				int ArgumentCount, RValue* Arguments[])                        \
			{                                                                  \
				if (!g_Anchors[Index].trampoline)                              \
				{                                                              \
					Result = RValue();                                         \
					return Result;                                             \
				}                                                              \
				RValue& returned = g_Anchors[Index].trampoline(                \
					Self, Other, Result, ArgumentCount, Arguments);            \
				HMD_LOG_SIGNATURE_ONCE(g_Anchors[Index].script,                \
					ArgumentCount, Arguments, &returned);                      \
				OnDrawCall(Index);                                             \
				return returned;                                               \
			}

		HMD_DEFINE_DRAW_DETOUR(0)
		HMD_DEFINE_DRAW_DETOUR(1)
		HMD_DEFINE_DRAW_DETOUR(2)
		HMD_DEFINE_DRAW_DETOUR(3)
		HMD_DEFINE_DRAW_DETOUR(4)
		HMD_DEFINE_DRAW_DETOUR(5)

		#undef HMD_DEFINE_DRAW_DETOUR

		PFUNC_YYGMLScript g_Detours[kAnchorCount] = {
			&DrawDetour0, &DrawDetour1, &DrawDetour2,
			&DrawDetour3, &DrawDetour4, &DrawDetour5,
		};
	}

	// ---------------------------------------------------------------------
	// Public surface
	// ---------------------------------------------------------------------
	bool Initialize(AurieModule* Module)
	{
		int installed = 0;
		for (size_t i = 0; i < kAnchorCount; i++)
		{
			g_Anchors[i].trampoline = hooks::Install(
				Module, g_Anchors[i].hook_id, g_Anchors[i].script, g_Detours[i]);

			if (g_Anchors[i].trampoline)
				installed++;
		}

		if (installed == 0)
		{
			LogWarn("no draw anchor could be hooked - the opponent badge and "
				"round marker will not be drawn. Everything else still works, "
				"and status is still reported on screen and through F8.");
		}
		else
		{
			LogInfo("%d draw anchor(s) hooked - F6 reports which one the game "
				"actually calls", installed);
		}

		// Resolve the head sprite up front so a missing asset is reported once
		// at load rather than silently every frame.
		if (HeadSpriteIndex() < 0)
			LogWarn("'%s' did not resolve - opponent markers will not be drawn",
				kHeadSprite);

		return true;
	}

	void Shutdown(AurieModule* Module)
	{
		for (size_t i = 0; i < kAnchorCount; i++)
		{
			if (!g_Anchors[i].trampoline)
				continue;

			hooks::Remove(Module, g_Anchors[i].hook_id);
			g_Anchors[i].trampoline = nullptr;
		}

		std::lock_guard<std::mutex> lock(g_Mutex);
		g_Overlay = Overlay{};
	}

	void Tick()
	{
		g_DrawBudget.store(kDrawBudgetPerTick);

		// Only worth querying while there is an overlay to place.
		if (!g_Connected.load())
			return;

		std::string room = bridge::CurrentRoomName();

		std::lock_guard<std::mutex> lock(g_RoomMutex);
		g_Room = std::move(room);
	}

	void SetOverlay(const Overlay& NewOverlay)
	{
		{
			std::lock_guard<std::mutex> lock(g_Mutex);
			g_Overlay = NewOverlay;
		}

		g_Connected.store(NewOverlay.connected);
	}

	void Notify(const char* Format, ...)
	{
		char message[512]{};

		va_list arguments;
		va_start(arguments, Format);
		_vsnprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format, arguments);
		va_end(arguments);

		// The log always gets it, whether or not the on-screen path works.
		LogInfo("%s", message);

		if (!g_NotificationsEnabled.load())
			return;

		if (!bridge::ScriptExists(kInfostreamScript))
			return;

		// The game's markup parser gets a scrubbed copy, never the raw message.
		// The log above already has the original, so nothing is lost by being
		// strict here - and cf_parse aborts the game rather than complaining
		// when it dislikes its input.
		const std::string safe = sanitize::ClampNotification(message);
		if (safe.empty())
		{
			// Nothing renderable survived. Calling the game with an empty string
			// is the exact input that kills cf_parse, so this returns instead.
			return;
		}

		g_NotificationsTried.store(true);

		// Announced, because this is a game routine the mod calls rather than
		// one it has watched the game call - see CallScriptAnnounced.
		if (bridge::CallScriptAnnounced(kInfostreamScript, { RValue(safe) }))
			g_NotificationsWork.store(true);
	}

	bool NotificationsAvailable()
	{
		return g_NotificationsWork.load();
	}

	void SetNotificationsEnabled(bool Enabled)
	{
		g_NotificationsEnabled.store(Enabled);

		if (!Enabled)
			LogInfo("on-screen messages are disabled - the log still has them");
	}

	void Report()
	{
		LogInfo("probe: head sprite %s, info stream %s",
			HeadSpriteIndex() >= 0 ? "resolved" : "MISSING",
			!bridge::ScriptExists(kInfostreamScript) ? "MISSING"
				: g_NotificationsWork.load() ? "working" : "untested");

		// The space every overlay draw call lands in. Printed next to the
		// run-map cell boxes that runstate::Report lists immediately after, so
		// the two can be read against each other: the opponent marker is placed
		// from a cell box and drawn into this, and nothing in the code checks
		// that they are the same coordinate system.
		LogInfo("probe: gui draw space is %.0f x %.0f - run-map cell boxes below "
			"must be on this scale for the opponent marker to land on screen",
			ReadDouble("display_get_gui_width", 0.0),
			ReadDouble("display_get_gui_height", 0.0));

		// Which anchors exist, and - the part that matters - which are actually
		// being called. An anchor that hooks cleanly and never fires draws
		// nothing, which is exactly how the first attempt at this failed.
		unsigned long long total = 0;
		for (size_t i = 0; i < kAnchorCount; i++)
		{
			LogInfo("probe:   anchor %-42s %s, %llu call(s)",
				g_Anchors[i].script,
				g_Anchors[i].trampoline ? "hooked" : "NOT HOOKED",
				g_Anchors[i].calls);
			total += g_Anchors[i].calls;
		}

		if (total == 0)
		{
			LogWarn("probe: no draw anchor has ever been called - nothing can "
				"be drawn on screen, and a different anchor script is needed");
		}

		// Which font the overlay is inheriting. Useful if the text renders in
		// the wrong face - it names what was current when we drew.
		auto font = bridge::CallBuiltin("draw_get_font", {});
		if (font)
		{
			auto name = bridge::CallBuiltin("font_get_name", { *font });
			if (name && name->m_Kind == VALUE_STRING)
			{
				const char* text = name->ToCString();
				LogInfo("probe: current draw font is '%s'", text ? text : "?");
			}
		}
	}
}
