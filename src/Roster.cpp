// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Roster.h"
#include "GameBridge.h"
#include "Log.h"
#include "Sanitize.h"

#include <atomic>

using namespace YYTK;

namespace hmd::roster
{
	namespace
	{
		std::atomic<bool> g_SuppressDefaultWave{ false };

		// Enemies destroyed by ClearDefaultEnemyWave since the last reset. Read
		// when a duel is called off, to decide whether the round the player is
		// standing in still has anything in it. See EnemiesCleared.
		std::atomic<int> g_EnemiesCleared{ 0 };

		// YYC strips the VARI chunk, so the real instance member names are not
		// knowable from data.win. Each logical field is probed against a list of
		// plausible names and the first hit wins. The winning name is cached per
		// field after the first successful resolve.
		struct FieldProbe
		{
			const char* logical;
			std::vector<std::string> candidates;
			std::string resolved;
			bool attempted = false;
		};

		FieldProbe g_Fields[] = {
			{ "type",        { "dude_type", "type", "dude_type_id", "unit_type" } },
			{ "name",        { "display_name", "dude_name", "name" } },
			{ "level",       { "level", "lvl", "tier" } },
			{ "hp",          { "hp", "health", "hp_current", "current_hp" } },
			{ "max_hp",      { "max_hp", "hp_max", "health_max", "maxhp" } },
			{ "attack",      { "attack", "damage", "atk" } },
			{ "speed",       { "speed", "move_speed", "spd" } },
			{ "range",       { "range", "attack_range" } },
			{ "crit_chance", { "crit_chance" } },
			{ "crit_damage", { "crit_damage" } },
		};

		FieldProbe* Field(const char* Logical)
		{
			for (FieldProbe& probe : g_Fields)
			{
				if (strcmp(probe.logical, Logical) == 0)
					return &probe;
			}
			return nullptr;
		}

		// Resolve a logical field against an instance, remembering the answer.
		const std::string* ResolveField(const RValue& Instance, const char* Logical)
		{
			FieldProbe* probe = Field(Logical);
			if (!probe)
				return nullptr;

			if (!probe->resolved.empty())
				return &probe->resolved;

			auto found = bridge::FindMemberName(Instance, probe->candidates);
			if (found)
			{
				probe->resolved = *found;
				if (!probe->attempted)
					LogInfo("field '%s' resolved to instance member '%s'",
						Logical, probe->resolved.c_str());
				probe->attempted = true;
				return &probe->resolved;
			}

			if (!probe->attempted)
			{
				probe->attempted = true;
				LogWarn("field '%s' did not resolve to any known member name - "
					"it will serialise as its default", Logical);
			}
			return nullptr;
		}

		double ReadNumber(const RValue& Instance, const char* Logical, double Fallback = 0.0)
		{
			const std::string* member = ResolveField(Instance, Logical);
			if (!member)
				return Fallback;
			return bridge::GetMemberNumber(Instance, *member, Fallback);
		}

		std::string ReadString(const RValue& Instance, const char* Logical)
		{
			const std::string* member = ResolveField(Instance, Logical);
			if (!member)
				return {};

			// The type field is frequently a numeric id rather than a string;
			// render either form as text so the payload is self-describing.
			auto value = bridge::GetMember(Instance, *member);
			if (!value)
				return {};

			if (value->m_Kind == VALUE_STRING)
			{
				const char* text = value->ToCString();
				return text ? std::string(text) : std::string{};
			}

			if (value->m_Kind == VALUE_REAL || value->m_Kind == VALUE_INT32 ||
				value->m_Kind == VALUE_INT64)
			{
				char buffer[32]{};
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%lld",
					static_cast<long long>(value->ToDouble()));
				return buffer;
			}

			return {};
		}

		// Ask the game to serialise the current round through its own export
		// path, then read the result back out of the clipboard. Returns an empty
		// string if any step is unavailable - this is a bonus channel, not a
		// requirement.
		//
		// The clipboard round-trip is the reason this function is so defensive.
		// The export script is the only way to reach the game's own serialiser,
		// but it publishes its result to the clipboard, which means the value
		// read back is only OUR data if the script actually wrote something. If
		// it silently no-ops - which is exactly what it may do outside the
		// Custom Matchup screen - the read-back returns whatever the player had
		// copied: a password, a private message, anything. That must never
		// reach a peer, so the result is trusted only when it both differs from
		// what was already there and passes a structural check for a real
		// matchup export. Nothing about the clipboard's contents is ever
		// logged, for the same reason.
		// What the game's own exporter produced, structurally.
		//
		// The whole duel now rides on this format, and nothing has ever logged
		// it. Top-level keys and array sizes are enough to write the injection
		// side against - specifically whether "dudes" and "enemies" are separate
		// arrays, because our dudes have to arrive as the opponent's enemies and
		// that is a swap the mod has to perform.
		//
		// Values are deliberately not logged. This is a serialisation of the
		// player's run and it goes to the log, not to a peer.
		void DescribeMatchupExport(const std::string& Export)
		{
			json::Value root;
			if (!json::Parse(Export, root) || !root.IsObject())
			{
				LogWarn("the native export is not a JSON object - first 80 "
					"bytes: %.80s", Export.c_str());
				return;
			}

			LogInfo("--- native matchup export: structure ---");

			for (const auto& [name, member] : root.Members())
			{
				if (member.IsArray())
				{
					LogInfo("    %-16s array[%zu]", name.c_str(),
						member.Items().size());
				}
				else if (member.IsObject())
				{
					LogInfo("    %-16s object", name.c_str());
				}
				else
				{
					LogInfo("    %-16s scalar", name.c_str());
				}
			}

			// The two that matter. Our dudes have to arrive as the opponent's
			// enemies, and writing that transform needs to know how each side is
			// keyed and what an entry looks like.
			//
			// Keys and one entry's field names only - not values. This is a
			// serialisation of the player's whole run, and the schema is what is
			// actually needed.
			for (const char* section : { "dudes", "enemies" })
			{
				const json::Value& node = root[section];
				if (!node.IsObject())
					continue;

				LogInfo("--- '%s' keys ---", section);

				int shown = 0;
				for (const auto& [key, entry] : node.Members())
				{
					if (shown++ >= 8)
					{
						LogInfo("    ... and more");
						break;
					}

					LogInfo("    %s -> %s", key.c_str(),
						entry.IsObject() ? "object" :
						entry.IsArray() ? "array" : "scalar");
				}

				// One entry's shape, which is the schema an injected unit has to
				// match.
				for (const auto& [key, entry] : node.Members())
				{
					if (!entry.IsObject())
						break;

					LogInfo("    fields of '%s':", key.c_str());
					for (const auto& [field, value] : entry.Members())
					{
						LogInfo("        %-20s %s", field.c_str(),
							value.IsObject() ? "object" :
							value.IsArray() ? "array" : "scalar");
					}
					break;
				}
			}

			LogInfo("--- end of structure ---");
		}

		std::string CaptureGameNativeExport()
		{
			constexpr const char* kExport = "gml_Script_current_round_to_custom_matchup_clipboard";

			if (!bridge::ScriptExists(kExport))
				return {};

			// Preserve whatever the player had on the clipboard.
			std::string previous;
			bool had_previous = false;
			auto had_text = bridge::CallBuiltin("clipboard_has_text", {});
			if (had_text && had_text->ToBoolean())
			{
				auto existing = bridge::CallBuiltin("clipboard_get_text", {});
				if (existing && existing->m_Kind == VALUE_STRING)
				{
					const char* text = existing->ToCString();
					if (text)
					{
						previous = text;
						had_previous = true;
					}
				}
			}

			// Restores the player's clipboard on every exit path below.
			auto restore = [&]()
			{
				if (had_previous)
					bridge::CallBuiltin("clipboard_set_text", { RValue(previous) });
			};

			if (!bridge::CallScriptAnnounced(kExport, {}))
			{
				restore();
				return {};
			}

			std::string exported;
			auto clipboard = bridge::CallBuiltin("clipboard_get_text", {});
			if (clipboard && clipboard->m_Kind == VALUE_STRING)
			{
				const char* text = clipboard->ToCString();
				if (text)
					exported = text;
			}

			restore();

			if (exported.empty())
			{
				LogWarn("native matchup export produced no clipboard text - "
					"using the per-unit snapshot only");
				return {};
			}

			// An unchanged clipboard used to mean "the exporter did not run", and
			// the contents were discarded as the player's own.
			//
			// That test misfires, and it was seen misfiring: press F6 twice in
			// the same round and the second export is byte-identical to the
			// first, which is still sitting on the clipboard. A perfectly good
			// export was thrown away with "left the clipboard unchanged".
			//
			// What actually matters is not whether the text changed but whether
			// it is a matchup payload, and the check below already answers that
			// - a player's clipboard is overwhelmingly unlikely to contain
			// something carrying the exporter's own keys. So unchanged text is
			// merely noted, and the structural test decides.
			if (had_previous && exported == previous)
			{
				LogInfo("the export matches what was already on the clipboard - "
					"expected when exporting the same round twice; the payload "
					"check below decides whether it is real");
			}

			if (!sanitize::IsMatchupPayload(exported))
			{
				LogWarn("clipboard contents after export do not look like a "
					"matchup payload (%zu bytes) - discarding rather than "
					"transmitting them", exported.size());
				return {};
			}

			return exported;
		}
	}

	// -----------------------------------------------------------------------
	// Unit
	// -----------------------------------------------------------------------
	json::Value Unit::ToJson() const
	{
		json::Value object = json::Value::Object();
		object.Set("type", json::Value(type));
		object.Set("name", json::Value(name));
		object.Set("x", json::Value(x));
		object.Set("y", json::Value(y));
		object.Set("level", json::Value(level));
		object.Set("hp", json::Value(hp));
		object.Set("max_hp", json::Value(max_hp));
		object.Set("attack", json::Value(attack));
		object.Set("speed", json::Value(speed));
		object.Set("range", json::Value(range));
		object.Set("crit_chance", json::Value(crit_chance));
		object.Set("crit_damage", json::Value(crit_damage));
		object.Set("knocked_out", json::Value(knocked_out));
		return object;
	}

	// Every field arriving here came off a socket, so nothing is trusted on the
	// way in. The peer is a second copy of this mod in the normal case, but it
	// is trivially modifiable and the wire format is plaintext - an unbounded
	// hp or a NaN attack would otherwise go straight onto a live instance.
	Unit Unit::FromJson(const json::Value& Object)
	{
		using namespace sanitize;

		Unit unit;
		unit.type = ClampText(Object["type"].AsString());
		unit.name = ClampText(Object["name"].AsString());
		unit.x = ClampNumber(Object["x"].AsNumber(), -kMaxCoordinate, kMaxCoordinate);
		unit.y = ClampNumber(Object["y"].AsNumber(), -kMaxCoordinate, kMaxCoordinate);
		unit.level = ClampNumber(Object["level"].AsNumber(), 0.0, kMaxLevel);
		unit.hp = ClampNumber(Object["hp"].AsNumber(), 0.0, kMaxHealth);
		unit.max_hp = ClampNumber(Object["max_hp"].AsNumber(), 0.0, kMaxHealth);
		unit.attack = ClampNumber(Object["attack"].AsNumber(), 0.0, kMaxAttack);
		unit.speed = ClampNumber(Object["speed"].AsNumber(), 0.0, kMaxSpeed);
		unit.range = ClampNumber(Object["range"].AsNumber(), 0.0, kMaxRange);
		unit.crit_chance = ClampNumber(Object["crit_chance"].AsNumber(), 0.0, kMaxCritChance);
		unit.crit_damage = ClampNumber(Object["crit_damage"].AsNumber(), 0.0, kMaxCritDamage);
		unit.knocked_out = Object["knocked_out"].AsBool();
		return unit;
	}

	// -----------------------------------------------------------------------
	// Snapshot
	// -----------------------------------------------------------------------
	std::string Snapshot::Serialize() const
	{
		json::Value root = json::Value::Object();
		root.Set("proto", json::Value(protocol));
		root.Set("act", json::Value(act));
		root.Set("lives", json::Value(lives));
		root.Set("matchup", json::Value(matchup));

		json::Value array = json::Value::Array();
		for (const Unit& unit : units)
			array.Push(unit.ToJson());
		root.Set("units", array);

		return root.Serialize();
	}

	bool Snapshot::Deserialize(const std::string& Text, Snapshot& Out)
	{
		json::Value root;
		if (!json::Parse(Text, root))
		{
			LogWarn("peer payload is not valid JSON (%zu bytes) - ignoring",
				Text.size());
			return false;
		}

		if (!root.IsObject())
		{
			LogWarn("peer payload is not a JSON object - ignoring");
			return false;
		}

		Snapshot snapshot;
		snapshot.protocol = root["proto"].AsInt(0);

		// Version gate first: nothing below is meaningful if the peer is not
		// speaking this protocol.
		if (snapshot.protocol != 1)
		{
			LogWarn("peer speaks protocol %d, this build speaks 1 - ignoring payload",
				snapshot.protocol);
			return false;
		}

		snapshot.act = static_cast<int>(
			sanitize::ClampNumber(root["act"].AsInt(0), 0.0, 1000.0));
		snapshot.lives = static_cast<int>(
			sanitize::ClampNumber(root["lives"].AsInt(0), 0.0, 99.0));

		// The matchup string is the one field that gets handed to the game's own
		// parser, so it is structure-checked before it is even stored. A payload
		// that fails here is dropped and the per-unit path is used instead.
		const std::string matchup = root["matchup"].AsString();
		if (!matchup.empty())
		{
			if (sanitize::IsMatchupPayload(matchup))
			{
				snapshot.matchup = matchup;
			}
			else
			{
				LogWarn("peer's matchup field is not a well-formed matchup "
					"payload (%zu bytes) - ignoring it and using the per-unit "
					"army instead", matchup.size());
			}
		}

		const json::Value& array = root["units"];
		if (array.IsArray())
		{
			for (const json::Value& item : array.Items())
			{
				if (snapshot.units.size() >= sanitize::kMaxUnits)
				{
					LogWarn("peer sent more than %zu units - ignoring the rest",
						sanitize::kMaxUnits);
					break;
				}

				if (item.IsObject())
					snapshot.units.push_back(Unit::FromJson(item));
			}
		}

		Out = std::move(snapshot);
		return true;
	}

	// Turn JSON text into a GML struct the game's own routines will accept.
	//
	// custom_matchup_parse is named for what it does to a matchup, not to a
	// string: hand it text and it dies at struct_merge_shallow:23 with
	// "struct_get_names argument 1 incorrect type (string) expecting a Number".
	// It wants the struct, already parsed. json_parse is the game's own way of
	// producing one.
	//
	// Returns nothing rather than an undefined RValue on failure. Undefined is
	// the argument that aborts the runtime, so it must never leave here.
	std::optional<RValue> ParseJsonToStruct(const std::string& Text)
	{
		if (Text.empty())
			return std::nullopt;

		auto parsed = bridge::CallBuiltin("json_parse", { RValue(Text) });
		if (!parsed)
			return std::nullopt;

		// A struct arrives as an object or, on this runner, a ref.
		const bool struct_shaped =
			parsed->m_Kind == VALUE_OBJECT || parsed->m_Kind == VALUE_REF;

		if (!struct_shaped || !parsed->m_Pointer)
		{
			LogWarn("json_parse returned kind %d rather than a struct - not "
				"passing it on", static_cast<int>(parsed->m_Kind));
			return std::nullopt;
		}

		return *parsed;
	}

	std::string BuildDuelPayload(const std::string& Export)
	{
		json::Value root;
		if (!json::Parse(Export, root) || !root.IsObject())
			return {};

		const json::Value& dudes = root["dudes"];
		if (!dudes.IsObject() || dudes.Members().empty())
		{
			LogWarn("the export has no 'dudes' to turn into an opposing army");
			return {};
		}

		// The whole duel, as a data transform.
		//
		// Both 'dudes' and 'enemies' are maps of type name to count - {"basic":1}
		// and {"toddler":9}, matching NUM_DUDES_ACTIVE and NUM_ENEMIES_ACTIVE
		// exactly. So "fight my army" is precisely: put my dudes where your
		// enemies go.
		//
		// The one thing this cannot verify from here is whether a dude type name
		// is meaningful in the enemies map. They may be separate namespaces, in
		// which case the parse will reject it or spawn nothing - which is what
		// SelfTestDuelPayload exists to find out, on one machine, before two
		// people spend a session on it.
		json::Value out = root;
		out.Set("enemies", dudes);

		// Only the army crosses. The boss and the alternate enemy list belong to
		// whatever round the sender happened to be on, and a duel replaces the
		// boss rather than adding to it.
		out.Set("non_boss_enemies", json::Value::Object());
		out.Set("boss_fight_id", json::Value(0.0));

		// The receiver keeps their own dudes - they are fighting with their army,
		// not adopting ours.
		out.Set("dudes", json::Value::Object());

		return out.Serialize();
	}

	void SelfTestDuelPayload()
	{
		LogInfo("--- duel payload self-test ---");

		const std::string exported = CaptureGameNativeExport();
		if (exported.empty())
		{
			LogWarn("self-test: nothing exported - cannot continue");
			return;
		}

		const std::string payload = BuildDuelPayload(exported);
		if (payload.empty())
		{
			LogWarn("self-test: the transform produced nothing");
			return;
		}

		LogInfo("self-test: transformed %zu bytes into a %zu byte duel payload",
			exported.size(), payload.size());

		if (!sanitize::IsMatchupPayload(payload))
		{
			LogWarn("self-test: the transformed payload no longer looks like a "
				"matchup - the transform broke its shape");
			return;
		}

		// Describe what is about to be handed over, so a rejection can be read
		// against what caused it.
		DescribeMatchupExport(payload);

		if (!bridge::ScriptExists("gml_Script_custom_matchup_parse"))
		{
			LogWarn("self-test: custom_matchup_parse does not exist - the "
				"injection half of the duel has no route at all");
			return;
		}

		// json_parse first. custom_matchup_parse wants the struct, not the text.
		auto as_struct = ParseJsonToStruct(payload);
		if (!as_struct)
		{
			LogWarn("self-test: the payload could not be turned into a struct - "
				"custom_matchup_parse cannot be called without one");
			return;
		}

		LogInfo("self-test: handing it to the game's own parser now. If the game "
			"stops here, dude types are not valid enemy types and the duel needs "
			"a different shape.");

		RValue parsed;
		if (!bridge::CallScriptAnnounced(
				"gml_Script_custom_matchup_parse", { *as_struct }, parsed))
		{
			LogWarn("self-test: custom_matchup_parse could not be called");
			return;
		}

		LogInfo("self-test: parser returned kind %d - %s",
			static_cast<int>(parsed.m_Kind),
			parsed.m_Kind == VALUE_UNDEFINED
				? "undefined, meaning it REJECTED the payload"
				: "something, meaning it ACCEPTED the payload");

		LogInfo("--- end of duel payload self-test ---");
	}

	void ProbeNativeExport()
	{
		const std::string exported = CaptureGameNativeExport();

		if (exported.empty())
		{
			LogWarn("probe: the native matchup export produced nothing. With "
				"instance members unreadable on this build, that leaves no way "
				"to capture an army at all.");
			return;
		}

		LogInfo("probe: native matchup export produced %zu bytes",
			exported.size());

		LogInfo("probe: it %s a plausible matchup payload by the sanitiser's "
			"own test", sanitize::IsMatchupPayload(exported) ? "IS" : "is NOT");

		DescribeMatchupExport(exported);
	}

	// -----------------------------------------------------------------------
	// Capture
	// -----------------------------------------------------------------------
	bool CaptureLocalArmy(Snapshot& Out)
	{
		std::vector<RValue> dudes = bridge::FindInstances("o_dude");

		if (dudes.empty())
		{
			LogWarn("no live o_dude instances found - cannot capture army "
				"(are we in rm_gameplay?)");
			return false;
		}

		// One-time diagnostic: the real member layout is only observable live.
		static bool logged_layout = false;
		if (!logged_layout)
		{
			logged_layout = true;
			bridge::LogInstanceMembers(dudes.front(), "o_dude");
		}

		Out.units.clear();
		Out.units.reserve(dudes.size());

		int skipped_unusable = 0;

		for (const RValue& dude : dudes)
		{
			// Never hand the runtime something it cannot use. dude_is_knocked_out
			// aborts the entire game on a bad argument rather than returning an
			// error - "I32 argument is undefined" at dude_is_knocked_out:238 is
			// what ended a test session - so the check sits before the loop body
			// rather than only in front of the call.
			if (!bridge::IsUsableInstance(dude))
			{
				skipped_unusable++;
				continue;
			}

			Unit unit;
			unit.type = ReadString(dude, "type");
			unit.name = ReadString(dude, "name");
			unit.x = bridge::GetMemberNumber(dude, "x");
			unit.y = bridge::GetMemberNumber(dude, "y");
			unit.level = ReadNumber(dude, "level");
			unit.hp = ReadNumber(dude, "hp");
			unit.max_hp = ReadNumber(dude, "max_hp");
			unit.attack = ReadNumber(dude, "attack");
			unit.speed = ReadNumber(dude, "speed");
			unit.range = ReadNumber(dude, "range");
			unit.crit_chance = ReadNumber(dude, "crit_chance");
			unit.crit_damage = ReadNumber(dude, "crit_damage");

			// Not asked for. dude_is_knocked_out is a confirmed script and it
			// aborts the game outright on an argument it does not like:
			//
			//     I32 argument is undefined
			//     - gml_Script_dude_is_knocked_out:238
			//
			// It was handed an instance and died, which means it wants something
			// other than an instance - a dude id or a struct out of the roster,
			// most likely. Guessing again costs another test session, and the
			// cost of not knowing is small: an unconscious dude joins the army
			// it is sent with. Wrong, but harmless and invisible next to a crash.
			//
			// The member dump above now runs against a real CInstance, so the
			// next duel log names the actual members of o_dude. Read
			// knocked-out state off one of those and delete this note.
			unit.knocked_out = false;

			Out.units.push_back(std::move(unit));
		}

		if (skipped_unusable > 0)
		{
			LogWarn("%d of %zu o_dude instance(s) were not readable and were "
				"skipped - the army sent will be short by that many",
				skipped_unusable, dudes.size());
		}

		// The native export is the army now, not a bonus on top of one.
		//
		// Reading per-unit stats off instances is finished as an approach on
		// this build: instance refs cannot be converted to pointers, and
		// GetInstanceMember returns nothing for a ref - not for `type` or
		// `level`, and not for `x`, `y` or `id` either. Nought of thirty-nine
		// probed names exist. The per-unit path below therefore produces a
		// correctly-shaped list of units with default stats, which is worse than
		// useless if it is what gets sent.
		//
		// current_round_to_custom_matchup_clipboard is the game serialising its
		// own round - dudes, enemies, relics, cash, the lot - and
		// custom_matchup_parse reads that format back. Neither needs the mod to
		// understand a single field. InjectOpponentArmy already prefers this
		// path; it has simply never been reached, because capture kept failing
		// before it got here.
		// Transformed before it is stored, so what crosses the wire is already
		// "fight this army" rather than "here is my whole round". The receiver
		// hands it straight to custom_matchup_parse and does not have to know
		// anything about the format.
		const std::string exported = CaptureGameNativeExport();
		Out.matchup = exported.empty() ? std::string{} : BuildDuelPayload(exported);

		if (!Out.matchup.empty())
		{
			LogStage(kStageSerialize,
				"native export captured: %zu bytes, %zu unit shell(s) alongside "
				"it. The export is what gets sent.",
				Out.matchup.size(), Out.units.size());

			// What the exporter actually produced, so the injection side can be
			// written against the real shape rather than a guess at it. Logged
			// once - it is large.
			static bool described = false;
			if (!described)
			{
				described = true;
				DescribeMatchupExport(Out.matchup);
			}

			return true;
		}

		// No export and no readable members leaves nothing worth sending.
		// Reporting success here would transmit an army of zero-stat units and
		// hand the opponent a free win, which is worse than calling the duel off.
		LogWarn("the native matchup export produced nothing, and per-unit stats "
			"cannot be read on this build - there is no army to send");

		return false;
	}

	// -----------------------------------------------------------------------
	// Injection
	// -----------------------------------------------------------------------
	bool IsDefaultWaveSuppressed()
	{
		return g_SuppressDefaultWave.load();
	}

	void SetDefaultWaveSuppressed(bool Suppressed)
	{
		g_SuppressDefaultWave.store(Suppressed);
	}

	void ClearDefaultEnemyWave()
	{
		SetDefaultWaveSuppressed(true);

		std::vector<RValue> enemies = bridge::FindInstances("o_enemy");
		if (enemies.empty())
		{
			LogInfo("no default enemies present to clear");
			return;
		}

		int destroyed = 0;
		for (const RValue& enemy : enemies)
		{
			if (bridge::CallBuiltin("instance_destroy", { enemy }))
				destroyed++;
		}

		g_EnemiesCleared.fetch_add(destroyed);

		LogStage(kStageInject, "cleared %d default enemy instance(s)", destroyed);
	}

	int EnemiesCleared()
	{
		return g_EnemiesCleared.load();
	}

	void ResetEnemiesCleared()
	{
		g_EnemiesCleared.store(0);
	}

	int InjectOpponentArmy(const Snapshot& Peer)
	{
		// Preferred path: hand the peer's payload to the game's own importer,
		// which validates and spawns using engine-native logic.
		//
		// Snapshot::Deserialize has already structure-checked this string, but
		// it is re-checked here because this is the exact point where peer bytes
		// cross out of the mod and into the game's own parser. That parser was
		// written for text a player pastes in, and it is the only part of this
		// path we do not control - so the check sits at the boundary rather than
		// only at the point of receipt.
		if (!Peer.matchup.empty() &&
			sanitize::IsMatchupPayload(Peer.matchup) &&
			bridge::ScriptExists("gml_Script_custom_matchup_parse"))
		{
			// Struct, not text. Handing the string straight over dies inside
			// struct_merge_shallow - see ParseJsonToStruct. The structural check
			// above still runs on the TEXT, before any of it reaches the game,
			// which is where a peer-supplied payload should be judged.
			auto as_struct = ParseJsonToStruct(Peer.matchup);

			RValue parsed;
			if (as_struct &&
				bridge::CallScriptAnnounced(
					"gml_Script_custom_matchup_parse",
					{ *as_struct },
					parsed) &&
				parsed.m_Kind != VALUE_UNDEFINED)
			{
				LogStage(kStageInject,
					"peer army handed to the game's own matchup parser");
				return static_cast<int>(Peer.units.size());
			}

			LogWarn("native matchup parse rejected the peer payload - falling "
				"back to per-unit spawning");
		}

		// Fallback path: spawn each unit directly as an o_enemy and stamp the
		// serialised attributes onto it.
		int object_index = bridge::AssetIndex("o_enemy");
		if (object_index < 0)
		{
			LogError("o_enemy is not a known object - cannot inject opponent army");
			return 0;
		}

		int spawned = 0;
		for (const Unit& unit : Peer.units)
		{
			// Knocked-out units do not join the opposing wave.
			if (unit.knocked_out)
				continue;

			// Mirror the unit across the arena so the peer's army arrives on
			// the opposing side rather than on top of the local roster.
			double spawn_x = -unit.x;
			double spawn_y = unit.y;

			auto created = bridge::CallBuiltin(
				"instance_create_depth",
				{
					RValue(spawn_x),
					RValue(spawn_y),
					RValue(0.0),
					RValue(static_cast<double>(object_index))
				}
			);

			if (!created || created->m_Kind == VALUE_UNDEFINED)
			{
				LogWarn("failed to create o_enemy for peer unit '%s'",
					unit.type.c_str());
				continue;
			}

			// instance_create_depth returns a VALUE_REF on this runner, exactly
			// as instance_find does, and a ref cannot be written to. Without
			// this the spawn succeeds and every single attribute write below
			// silently does nothing - an arena full of default enemies that
			// look like the peer's army and are not.
			RValue target;
			if (!bridge::AsInstance(*created, target))
			{
				static bool warned = false;
				if (!warned)
				{
					warned = true;
					LogWarn("spawned enemies cannot be resolved to instances - "
						"the opponent's army will arrive with default stats");
				}

				spawned++;
				continue;
			}

			// Best-effort attribute transfer. Each write is independent: a
			// member the runtime does not expose is skipped, not fatal.
			auto write = [&](const char* logical, double value)
			{
				const std::string* member = ResolveField(target, logical);
				if (member)
					bridge::SetMember(target, *member, RValue(value));
			};

			write("level", unit.level);
			write("max_hp", unit.max_hp);
			write("hp", unit.hp > 0.0 ? unit.hp : unit.max_hp);
			write("attack", unit.attack);
			write("speed", unit.speed);
			write("range", unit.range);
			write("crit_chance", unit.crit_chance);
			write("crit_damage", unit.crit_damage);

			spawned++;
		}

		LogStage(kStageInject, "spawned %d/%zu peer unit(s) as the opponent wave",
			spawned, Peer.units.size());

		return spawned;
	}
}
