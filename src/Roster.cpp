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

			if (!bridge::CallScript(kExport, {}))
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

			// Unchanged clipboard means the exporter did not run. Whatever is
			// sitting there belongs to the player, not to this mod.
			if (had_previous && exported == previous)
			{
				LogWarn("native matchup export left the clipboard unchanged - "
					"discarding it rather than transmitting the player's own "
					"clipboard contents");
				return {};
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

		for (const RValue& dude : dudes)
		{
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

			// dude_is_knocked_out is a confirmed top-level script; prefer it
			// over guessing at a member name.
			RValue knocked;
			if (bridge::CallScript("gml_Script_dude_is_knocked_out", { dude }, knocked))
				unit.knocked_out = knocked.ToBoolean();

			Out.units.push_back(std::move(unit));
		}

		Out.matchup = CaptureGameNativeExport();

		LogStage(kStageSerialize,
			"captured %zu unit(s); native export %s (%zu bytes)",
			Out.units.size(),
			Out.matchup.empty() ? "unavailable" : "captured",
			Out.matchup.size());

		return true;
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

		LogStage(kStageInject, "cleared %d default enemy instance(s)", destroyed);
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
			RValue parsed;
			if (bridge::CallScript(
					"gml_Script_custom_matchup_parse",
					{ RValue(Peer.matchup) },
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

			// Best-effort attribute transfer. Each write is independent: a
			// member the runtime does not expose is skipped, not fatal.
			auto write = [&](const char* logical, double value)
			{
				const std::string* member = ResolveField(*created, logical);
				if (member)
					bridge::SetMember(*created, *member, RValue(value));
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
