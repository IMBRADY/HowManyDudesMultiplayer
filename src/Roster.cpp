// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Roster.h"
#include "GameBridge.h"
#include "GameHooks.h"
#include "RunState.h"
#include "Log.h"
#include "Sanitize.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

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

		// Where this DLL lives. Anything that has to outlive the game process -
		// the bisect's stage marker, its transcript, the enemy-type vocabulary -
		// goes on disk next to the mod rather than in a static.
		std::filesystem::path ModuleDirectory()
		{
			HMODULE self = nullptr;
			const BOOL found = GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ModuleDirectory),
				&self
			);

			if (!found || !self)
				return {};

			wchar_t path[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
			if (length == 0 || length >= MAX_PATH)
				return {};

			std::error_code error;
			auto parent = std::filesystem::path(path).parent_path();
			return std::filesystem::exists(parent, error) ? parent : std::filesystem::path{};
		}

		// ------------------------------------------------------------------
		// The enemy-type vocabulary
		// ------------------------------------------------------------------
		//
		// Measured, from a controlled pair that differed in one word:
		//
		//     "enemies":{"toddler":1}   parsed
		//     "enemies":{"basic":1}     killed the game
		//
		// Dude types and enemy types are separate namespaces. An army cannot
		// cross the wire as itself; it has to be expressed in enemy types.
		//
		// The names cannot be read out of data.win - YYC stripped the VARI
		// chunk - but every round's export names the enemies that round is
		// fighting, and those are real by construction. So they are collected as
		// they are seen and kept in a file, and the vocabulary grows with play.
		//
		// This is deliberately a harvest rather than a lookup: nothing is called
		// on the game to obtain it, so it cannot abort the runtime.
		// A type name as the exports write them. The vocabulary file sits next to
		// the DLL in plain text, so anything hand-edited into it that is not
		// identifier-shaped is ignored rather than passed to the game.
		bool IsTypeNameLike(const std::string& Name)
		{
			if (Name.empty() || Name.size() > sanitize::kMaxTextField)
				return false;

			for (unsigned char c : Name)
			{
				const bool allowed =
					(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_';

				if (!allowed)
					return false;
			}

			return true;
		}

		std::filesystem::path EnemyVocabularyPath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"enemy_types_seen.txt";
		}

		std::vector<std::string> LoadEnemyVocabulary()
		{
			std::vector<std::string> names;

			const auto path = EnemyVocabularyPath();
			if (path.empty())
				return names;

			std::ifstream file(path);
			if (!file)
				return names;

			std::string line;
			while (std::getline(file, line))
			{
				while (!line.empty() &&
					(line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
					line.pop_back();

				if (!line.empty() && !IsTypeNameLike(line))
					continue;

				if (!line.empty() &&
					std::find(names.begin(), names.end(), line) == names.end())
					names.push_back(line);
			}

			return names;
		}

		// Adds any name not already on file. Order is insertion order, so the
		// mapping below is stable across runs for a given vocabulary.
		void RememberEnemyTypes(const std::vector<std::string>& Names)
		{
			if (Names.empty())
				return;

			const auto path = EnemyVocabularyPath();
			if (path.empty())
				return;

			std::vector<std::string> known = LoadEnemyVocabulary();

			std::vector<std::string> fresh;
			for (const std::string& name : Names)
			{
				if (!IsTypeNameLike(name))
					continue;

				if (std::find(known.begin(), known.end(), name) != known.end())
					continue;

				if (std::find(fresh.begin(), fresh.end(), name) != fresh.end())
					continue;

				fresh.push_back(name);
			}

			if (fresh.empty())
				return;

			std::ofstream file(path, std::ios::app);
			if (!file)
				return;

			for (const std::string& name : fresh)
				file << name << "\n";

			LogInfo("learned %zu new enemy type name(s); the duel vocabulary is "
				"now %zu name(s)", fresh.size(), known.size() + fresh.size());
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
		// Top-level keys and array sizes. Note that "scalar" does not
		// distinguish a string from a number, which is how boss_fight_id being
		// a string went unnoticed for nine sessions - the self-test logs the
		// export verbatim for exactly that reason.
		//
		// Values are deliberately not logged here. This is a serialisation of
		// the player's run and it goes to the log, not to a peer.
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
		// exactly. So "fight my army" wants to be: put my dudes where your
		// enemies go.
		//
		// It cannot be, and this is measured rather than assumed. Two bisect
		// stages differing in one word:
		//
		//     "enemies":{"toddler":1}   parsed, game lived
		//     "enemies":{"basic":1}     I32 argument is undefined, game died
		//
		// Dude types are not enemy types. The army has to be translated into the
		// enemy vocabulary on the way out.
		const json::Value& enemies = root["enemies"];

		// Learn from this export before using it. The round the sender is
		// standing in names the enemies it is fighting, and those are real by
		// construction.
		std::vector<std::string> from_this_round;
		for (const auto& [type, count] : enemies.Members())
			from_this_round.push_back(type);
		for (const auto& [type, count] : root["non_boss_enemies"].Members())
			from_this_round.push_back(type);

		RememberEnemyTypes(from_this_round);

		// Prefer what this round is already fighting - those names are known
		// good in a live round right now - and fall back to everything seen in
		// earlier rounds.
		std::vector<std::string> vocabulary = from_this_round;
		for (const std::string& name : LoadEnemyVocabulary())
		{
			if (std::find(vocabulary.begin(), vocabulary.end(), name) ==
				vocabulary.end())
				vocabulary.push_back(name);
		}

		if (vocabulary.empty())
		{
			LogWarn("no enemy type name has been seen yet, so this army cannot "
				"be expressed as a wave - no duel payload this round");
			return {};
		}

		// Each dude type takes an enemy type, round-robin, in a stable order.
		// Counts are preserved, so the wave the opponent fights has the size and
		// the shape of the sending army even though the identities differ.
		//
		// This is an approximation and is meant to read as one: a five-dude army
		// arrives as five enemies in the same groupings, not as five of its own
		// dudes. Per-type fidelity needs a real dude-to-enemy correspondence,
		// which needs the enemy namespace enumerated, which the vocabulary file
		// is slowly accumulating.
		json::Value translated = json::Value::Object();
		size_t next = 0;

		for (const auto& [dude_type, count] : dudes.Members())
		{
			const std::string& enemy_type = vocabulary[next % vocabulary.size()];
			next++;

			const double already = translated[enemy_type].AsNumber();
			translated.Set(enemy_type, json::Value(already + count.AsNumber()));
		}

		json::Value out = root;
		out.Set("enemies", translated);

		// Only the army crosses. The boss and the alternate enemy list belong to
		// whatever round the sender happened to be on, and a duel replaces the
		// boss rather than adding to it.
		out.Set("non_boss_enemies", json::Value::Object());

		// "No boss" is the empty STRING in this format, not 0. A live export
		// reads "boss_fight_id":"", and writing 0 there is what killed the game
		// at custom_matchup_refresh_difficulty:150 - the difficulty pass took 0
		// for a real boss id, resolved it to nothing, and aborted on the I32
		// conversion. The bisect stage that set only this field reproduced it
		// exactly, with every other field left alone.
		out.Set("boss_fight_id", json::Value(std::string{}));

		// The receiver keeps their own dudes - they are fighting with their army,
		// not adopting ours.
		//
		// roster_order goes with them. It is the dudes' ordering - a live export
		// pairs "dudes":{"basic":1.0} with "roster_order":["basic"] - so leaving
		// it populated beside an empty roster describes an army that is not
		// there. Whether the game minds is what stage 4 asks; it is cleared here
		// because a document that contradicts itself has no reason to be sent.
		out.Set("dudes", json::Value::Object());
		out.Set("roster_order", json::Value::Array());

		return out.Serialize();
	}

	// -----------------------------------------------------------------------
	// The staged self-test
	// -----------------------------------------------------------------------
	//
	// The single-payload version of this test established that
	// custom_matchup_parse accepts the struct, merges it, and then dies inside
	// matchup_calculate_difficulty_score with "I32 argument is undefined".
	//
	// That is a real result, but it does not say which part of the transform
	// caused it. BuildDuelPayload changes four things at once - enemies, dudes,
	// non_boss_enemies and boss_fight_id - and any one of them could be what the
	// difficulty pass cannot resolve. One crash, four suspects.
	//
	// So the test is now a bisect: one payload per change, run in sequence
	// against a single export snapshot. A stage that survives logs what it did
	// to the live enemy and dude counts. A stage that does not survive takes the
	// game with it - which is exactly the signal, because the log names the
	// stage before the call and nothing after it.
	//
	// Stages 5 and 6 are the controlled pair the whole feature rests on. They
	// differ in one thing only: the type names in the enemies map. If a known
	// enemy type passes where a dude type dies, the namespaces are separate and
	// the answer is a type mapping, not a redesign.
	namespace
	{
		std::filesystem::path StageMarkerPath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"selftest_stage.txt";
		}

		// aurie.log is truncated on every launch, and a bisect whose failure
		// mode is "the game exits" therefore destroys its own evidence: the run
		// that established stages 0-6 was unreadable by the time the next launch
		// finished booting, and all that survived was the marker.
		//
		// So the self-test keeps its own transcript, appended and never
		// truncated. It is the record of the whole bisect rather than of the
		// last launch.
		std::filesystem::path TranscriptPath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"selftest_transcript.log";
		}

		void AppendToTranscript(const char* Text)
		{
			const auto path = TranscriptPath();
			if (path.empty())
				return;

			std::ofstream file(path, std::ios::app);
			if (!file)
				return;

			SYSTEMTIME now{};
			GetLocalTime(&now);

			char stamp[32]{};
			_snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "[%02u:%02u:%02u] ",
				now.wHour, now.wMinute, now.wSecond);

			file << stamp << Text << "\n";
		}

		// Everything the self-test says goes to both the log and the transcript.
		// A line that only reaches aurie.log is a line that will not survive the
		// crash it is describing.
		template <typename... Args>
		void SelfTestLog(const char* Format, Args... Arguments)
		{
			char message[1024]{};
			_snprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format,
				Arguments...);

			LogInfo("%s", message);
			AppendToTranscript(message);
		}

		template <typename... Args>
		void SelfTestWarn(const char* Format, Args... Arguments)
		{
			char message[1024]{};
			_snprintf_s(message, sizeof(message) - 1, _TRUNCATE, Format,
				Arguments...);

			LogWarn("%s", message);
			AppendToTranscript(message);
		}

		// The payload bisect and the apply probe are separate sequences and fail
		// independently, so they keep separate markers.
		std::filesystem::path ApplyMarkerPath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"selftest_apply.txt";
		}

		// The marker is written before a step calls into the game and removed
		// the moment that step survives. So a marker present at startup means
		// precisely one thing: that step was attempted and never came back.
		void MarkAttempted(const std::filesystem::path& Path, int Index)
		{
			if (Path.empty())
				return;

			std::ofstream file(Path, std::ios::trunc);
			if (file)
				file << Index;
		}

		void ClearMarker(const std::filesystem::path& Path)
		{
			if (Path.empty())
				return;

			std::error_code error;
			std::filesystem::remove(Path, error);
		}

		// -1 when nothing is on record.
		int ReadIndexThatDidNotReturn(const std::filesystem::path& Path)
		{
			if (Path.empty())
				return -1;

			std::ifstream file(Path);
			if (!file)
				return -1;

			int index = -1;
			if (!(file >> index))
				return -1;

			return index >= 0 ? index : -1;
		}

		void MarkStageAttempted(int Stage)
		{
			MarkAttempted(StageMarkerPath(), Stage);
		}

		void ClearStageMarker()
		{
			ClearMarker(StageMarkerPath());
		}

		int ReadStageThatDidNotReturn()
		{
			return ReadIndexThatDidNotReturn(StageMarkerPath());
		}

		// An RValue rendered as kind AND value.
		//
		// Logging only the kind is how an entire probe was wasted:
		// custom_matchup_fightable is a predicate, it came back "kind 13", and
		// kind 13 is bool - which does not say true or false, and true or false
		// was the whole question.
		//
		// Only kinds whose conversion is a field read are converted. ToDouble
		// and ToInt32 are runtime conversions that abort on an undefined, so
		// nothing unrecognised is touched.
		std::string DescribeValue(const RValue& Value)
		{
			char buffer[128]{};

			switch (Value.m_Kind)
			{
			case VALUE_UNDEFINED:
				return "undefined (normal for a GML routine that returns nothing)";

			case VALUE_BOOL:
				return Value.ToBoolean() ? "TRUE" : "FALSE";

			case VALUE_REAL:
			case VALUE_INT32:
			case VALUE_INT64:
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%g",
					Value.ToDouble());
				return buffer;

			case VALUE_STRING:
			{
				const char* text = Value.ToCString();
				if (!text)
					return "string (null)";

				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "\"%.100s\"", text);
				return buffer;
			}

			case VALUE_OBJECT:
				return "struct or instance";

			case VALUE_REF:
				return "ref";

			case VALUE_ARRAY:
				return "array";

			default:
				_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "kind %d",
					static_cast<int>(Value.m_Kind));
				return buffer;
			}
		}

		// A numeric global, or -1 when it is absent or not a number.
		//
		// The kind is checked before any conversion. ToDouble on an undefined is
		// a runtime conversion, and this project has already killed the game
		// twice by treating one as a field read.
		int ReadGlobalCount(const char* Name)
		{
			auto value = bridge::GetGlobal(Name);
			if (!value)
				return -1;

			if (value->m_Kind != VALUE_REAL && value->m_Kind != VALUE_INT32 &&
				value->m_Kind != VALUE_INT64)
				return -1;

			const double raw = value->ToDouble();
			if (!std::isfinite(raw))
				return -1;

			return static_cast<int>(raw);
		}

		// The log formatter renders into a 1 KB buffer, and an export grows with
		// the size of the army. Slicing keeps a large one readable instead of
		// silently truncated.
		//
		// The text is passed as an argument and never as the format string: a
		// payload containing a percent sign would otherwise be interpreted.
		void LogTextInSlices(const char* Label, const std::string& Text)
		{
			constexpr size_t kSlice = 240;

			for (size_t offset = 0; offset < Text.size(); offset += kSlice)
			{
				SelfTestLog("    %s[%zu] %s", Label, offset,
					Text.substr(offset, kSlice).c_str());
			}
		}

		struct SelfTestStage
		{
			std::string name;
			std::string asks;
			std::string payload;
		};

		// One export snapshot, one payload per change under test.
		std::vector<SelfTestStage> BuildSelfTestStages(const std::string& Export)
		{
			std::vector<SelfTestStage> stages;

			json::Value root;
			if (!json::Parse(Export, root) || !root.IsObject())
				return stages;

			stages.push_back({
				"the raw export, unmodified",
				"whether custom_matchup_parse survives being handed the game's "
				"own document. A failure here means the crash was never about "
				"the transform at all",
				Export });

			stages.push_back({
				"round-tripped through the mod's JSON, nothing changed",
				"whether the mod's own parser and serialiser damage the "
				"document on the way through",
				root.Serialize() });

			// Settled: boss_fight_id = 0 kills the game at
			// custom_matchup_refresh_difficulty:150. A live export writes
			// "boss_fight_id":"", so this stage now tests the corrected value
			// rather than re-running a known fatal one.
			{
				json::Value stage = root;
				stage.Set("boss_fight_id", json::Value(std::string{}));
				stages.push_back({
					"boss_fight_id = \"\" (empty string), nothing else",
					"whether the empty string is the 'no boss' sentinel this "
					"format expects. 0 in this field is already known to be "
					"fatal",
					stage.Serialize() });
			}

			{
				json::Value stage = root;
				stage.Set("non_boss_enemies", json::Value::Object());
				stages.push_back({
					"non_boss_enemies = {}, nothing else",
					"whether the difficulty pass needs the filler list to be "
					"populated",
					stage.Serialize() });
			}

			{
				json::Value stage = root;
				stage.Set("dudes", json::Value::Object());
				stages.push_back({
					"dudes = {} with roster_order left stale",
					"whether an army of nobody is legal, and whether the game "
					"minds roster_order still naming dudes that the roster no "
					"longer contains",
					stage.Serialize() });
			}

			{
				json::Value stage = root;
				stage.Set("dudes", json::Value::Object());
				stage.Set("roster_order", json::Value::Array());
				stages.push_back({
					"dudes = {} and roster_order = [] together",
					"the same question with the document made self-consistent. "
					"If this passes where the stage above fails, the ordering "
					"has to travel with the roster",
					stage.Serialize() });
			}

			const json::Value& enemies = root["enemies"];
			const json::Value& dudes = root["dudes"];

			// The control for the stage below it. Same rewrite of the enemies
			// map, with a type name the round demonstrably already contains, so
			// the only thing left varying between the two is the name itself.
			if (enemies.IsObject() && !enemies.Members().empty())
			{
				double army = 0.0;
				for (const auto& [type, count] : dudes.Members())
					army += count.AsNumber();

				if (army < 1.0)
					army = 1.0;

				const std::string& known = enemies.Members().begin()->first;

				json::Value rewritten = json::Value::Object();
				rewritten.Set(known, json::Value(army));

				json::Value stage = root;
				stage.Set("enemies", rewritten);
				stages.push_back({
					"enemies = { " + known + ": " +
						std::to_string(static_cast<long long>(army)) +
						" }, a type this round already has",
					"whether rewriting the enemies map is legal at all when the "
					"type name is known to be an enemy",
					stage.Serialize() });
			}

			// SETTLED, and deliberately not re-run: "enemies" = my dudes
			// verbatim kills the game. That stage and stage 6 above were
			// byte-identical but for "toddler" against "basic", and only the
			// dude type was fatal. Re-running a payload already known to abort
			// the runtime would cost a launch and teach nothing.
			//
			// The translation it forced is carried by the full-transform stage
			// at the end, which calls the real BuildDuelPayload.

			// The one assumption the translation rests on: that an enemy type
			// name learned in an earlier round is still legal in this one.
			//
			// It should be - custom_matchup_parse backs the Custom Matchup
			// screen, whose entire purpose is composing arbitrary matchups - but
			// "should be" is what the last four sessions were made of. This
			// stage only exists once the vocabulary holds a name the current
			// round is not using, and then it tests exactly that.
			{
				std::vector<std::string> in_this_round;
				for (const auto& [type, count] : enemies.Members())
					in_this_round.push_back(type);

				std::string from_another_round;
				for (const std::string& name : LoadEnemyVocabulary())
				{
					if (std::find(in_this_round.begin(), in_this_round.end(),
							name) == in_this_round.end())
					{
						from_another_round = name;
						break;
					}
				}

				if (!from_another_round.empty() && dudes.IsObject())
				{
					double army = 0.0;
					for (const auto& [type, count] : dudes.Members())
						army += count.AsNumber();

					if (army < 1.0)
						army = 1.0;

					json::Value rewritten = json::Value::Object();
					rewritten.Set(from_another_round, json::Value(army));

					json::Value stage = root;
					stage.Set("enemies", rewritten);
					stages.push_back({
						"enemies = { " + from_another_round +
							": my dude count }, a type from a DIFFERENT round",
						"whether the enemy vocabulary is round-independent. The "
						"translation reuses names across rounds, so a failure "
						"here means it may only use what the round already has",
						stage.Serialize() });
				}
			}

			const std::string full = BuildDuelPayload(Export);
			if (!full.empty())
			{
				stages.push_back({
					"the full duel transform, all four changes at once",
					"whether the combination fails even when the parts pass",
					full });
			}

			return stages;
		}

		// True when the stage was reached and returned. False only means it was
		// skipped before the game was involved - a stage the game rejects fatally
		// never returns at all.
		bool RunSelfTestStage(const SelfTestStage& Stage, int Index, int Last)
		{
			SelfTestLog("--- stage %d of %d: %s ---", Index, Last,
				Stage.name.c_str());
			SelfTestLog("    asks: %s", Stage.asks.c_str());
			SelfTestLog("    payload is %zu bytes", Stage.payload.size());

			// The payload itself, so a stage's result can be read against what
			// it actually contained rather than against what it was meant to.
			LogTextInSlices("payload", Stage.payload);

			if (!sanitize::IsMatchupPayload(Stage.payload))
			{
				SelfTestWarn("    SKIPPED BY THE MOD - this no longer looks like "
					"a matchup, so the mod built it wrong and the game never saw "
					"it. This is not a result about the game");
				return false;
			}

			auto as_struct = ParseJsonToStruct(Stage.payload);
			if (!as_struct)
			{
				SelfTestWarn("    SKIPPED BY THE MOD - json_parse would not turn "
					"it into a struct, so custom_matchup_parse was never called. "
					"This is not a result about the game");
				return false;
			}

			const int enemies_before = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int dudes_before = ReadGlobalCount("NUM_DUDES_ACTIVE");

			// On disk before the call, because the call is the thing that may
			// not return. If the game dies here, the next F5 press reads this
			// and resumes past it.
			MarkStageAttempted(Index);

			SelfTestLog("    HANDED TO THE GAME - if the transcript ends here, "
				"this stage is the one that kills it");

			RValue parsed;
			if (!bridge::CallScriptAnnounced(
					"gml_Script_custom_matchup_parse", { *as_struct }, parsed))
			{
				SelfTestWarn("    SKIPPED - custom_matchup_parse could not be "
					"called at all");
				ClearStageMarker();
				return false;
			}

			ClearStageMarker();

			const int enemies_after = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int dudes_after = ReadGlobalCount("NUM_DUDES_ACTIVE");

			SelfTestLog("    SURVIVED. parser returned %s",
				DescribeValue(parsed).c_str());

			// A parse that "succeeded" without moving these did nothing, and
			// that is not success.
			SelfTestLog("    NUM_ENEMIES_ACTIVE %d -> %d, NUM_DUDES_ACTIVE %d -> %d",
				enemies_before, enemies_after, dudes_before, dudes_after);

			return true;
		}
	}

	// -----------------------------------------------------------------------
	// The apply probe
	// -----------------------------------------------------------------------
	//
	// The payload bisect ended with every stage surviving and every stage
	// reporting NUM_ENEMIES_ACTIVE 10 -> 10. Fifteen successful parses, no
	// effect on the live round - including the final one, which asked for a
	// single toddler in a round holding ten.
	//
	// So custom_matchup_parse parses. It does not apply. The payload format is
	// settled and the injection step is a different routine entirely.
	//
	// discovered_mappings.json names the rest of the chain, and the evidence
	// string "Imported custom matchup from clipboard." says the Import button
	// runs custom_matchup_import rather than the parser this mod has been
	// calling. Each candidate is tried one per press, marked before the call, so
	// a fatal one is skipped next time rather than blocking the ones behind it.
	namespace
	{
		struct ApplyCandidate
		{
			const char* script;
			const char* what;
			bool wants_payload_on_clipboard;
		};

		// matchup_simulator_spawn was candidate 3 and killed the game. It is on
		// the do-not-call list now and is not retried.
		//
		// The other three survived and moved nothing. All are custom-matchup
		// routines, and all were called from rm_gameplay - which is very
		// probably the point: that screen composes a matchup and starts a run
		// from it. None of them is an "inject into the fight already running"
		// routine, because the game has no reason to have one.
		const ApplyCandidate kApplyCandidates[] = {
			{ "gml_Script_custom_matchup_fightable",
			  "a predicate. Its VALUE - not its kind - says whether the parse "
			  "left anything staged. Previously logged as 'kind 13', which is "
			  "bool, which does not answer the question it was asked",
			  false },
			{ "gml_Script_custom_matchup_load",
			  "the other half of save/load. If parse only fills a struct, this "
			  "may be what installs it",
			  false },
			{ "gml_Script_custom_matchup_import",
			  "the Import button's own path, with the payload on the clipboard. "
			  "Survived and did nothing from rm_gameplay; worth re-reading now "
			  "that return values are logged",
			  true },
			{ "gml_Script_custom_matchup_begin",
			  "starts the loaded matchup. Survived and did nothing, which fits "
			  "a routine meant to start a run rather than alter one",
			  false },
		};

		// ------------------------------------------------------------------
		// Phase 3: the direct route
		// ------------------------------------------------------------------
		//
		// Every matchup routine parses, returns, and leaves the live round
		// alone. That is consistent with the Custom Matchup screen being a
		// "compose a fight and start it" feature rather than an injection API,
		// in which case no amount of probing it will ever move a running round.
		//
		// The mod already destroys enemy instances successfully -
		// ClearDefaultEnemyWave has a working count. Whatever can be destroyed
		// can very likely be created, and instance_create_depth is already on
		// the known-safe list.
		//
		// The missing piece is the object behind a type name. asset_get_index
		// answers that and is completely safe: an unknown name returns -1, no
		// abort, no side effect. So the whole namespace can be probed for free.
		const char* const kObjectPrefixes[] = {
			"o_", "obj_", "o_enemy_", "obj_enemy_", "enemy_", "o_e_", "",
		};

		// Resolve a type name to an object index, or -1.
		double ResolveObjectForType(const std::string& TypeName, std::string& OutName)
		{
			for (const char* prefix : kObjectPrefixes)
			{
				const std::string candidate = std::string(prefix) + TypeName;

				auto index = bridge::CallBuiltin(
					"asset_get_index", { RValue(candidate) });

				if (!index)
					continue;

				if (index->m_Kind != VALUE_REAL && index->m_Kind != VALUE_INT32 &&
					index->m_Kind != VALUE_INT64)
					continue;

				const double resolved = index->ToDouble();
				if (resolved >= 0.0)
				{
					OutName = candidate;
					return resolved;
				}
			}

			return -1.0;
		}

		// ------------------------------------------------------------------
		// The spawn-code vocabulary
		// ------------------------------------------------------------------
		//
		// Observed, from the game calling itself:
		//
		//     gml_Script_enemy_spawn(3 args)
		//         = str("wxj"), num(-385.668), num(104.149) -> ref
		//
		// enemy_spawn(type, x, y), returning the instance it made. Note the
		// type: "wxj", not "toddler". The export's enemy names and the spawner's
		// type codes are DIFFERENT NAMESPACES - a third one, after dude types
		// and enemy types.
		//
		// Nothing readable connects them, so the codes are harvested exactly as
		// the enemy names were: watch the game spawn its own waves and write
		// down what it passed. The observed coordinates come with them, because
		// x was negative - enemies enter from off-screen - and inventing a
		// position would be a guess where a measurement is free.
		// The round number the GAME is using, taken from its own call to
		// enemies_get_available_for_round(round, ?) before each wave.
		//
		// Deliberately not runstate::CurrentRound(). That is the mod's own
		// inference, it has a long history of being wrong, and if it disagrees
		// with the game by even one the round match below never succeeds and the
		// probe declines forever with no way to tell why. Stamping and matching
		// with the same number the game passed itself removes the question.
		//
		// -1 until a wave has been seen this session.
		std::atomic<int> g_GameWaveRound{ -1 };

		// The return address of whoever called enemy_spawn, captured once by the
		// observer and named later, in the safe window. Null until a wave has
		// spawned this session.
		std::atomic<const void*> g_SpawnCallerAddress{ nullptr };

		// The most recent id the game passed to enemy_spawn. Written by the
		// observer and read by the spawn-plan probe, both on the game's thread.
		std::string g_LastSpawnId;

		int CurrentWaveRound()
		{
			const int seen = g_GameWaveRound.load();
			return seen >= 0 ? seen : runstate::CurrentRound();
		}

		std::filesystem::path SpawnCodePath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"spawn_codes_seen.txt";
		}

		struct SpawnCall
		{
			std::string code;
			double x = 0.0;
			double y = 0.0;
			int round = 0;
		};

		std::vector<SpawnCall> LoadSpawnCalls()
		{
			std::vector<SpawnCall> calls;

			const auto path = SpawnCodePath();
			if (path.empty())
				return calls;

			std::ifstream file(path);
			if (!file)
				return calls;

			// Line by line rather than a streamed field chain. A chain stops at
			// the first line that does not match and takes every later line with
			// it, so one short line - a file written before the round column
			// existed, say - reads as an empty harvest rather than a partial
			// one. A line without a round is kept with round -1, which matches
			// no live round and is therefore never replayed.
			std::string entry;
			while (std::getline(file, entry))
			{
				std::istringstream fields(entry);

				SpawnCall call;
				call.round = -1;

				if (!(fields >> call.code >> call.x >> call.y))
					continue;

				fields >> call.round;

				if (!IsTypeNameLike(call.code))
					continue;

				const bool known = std::any_of(calls.begin(), calls.end(),
					[&](const SpawnCall& seen) { return seen.code == call.code; });

				if (!known)
					calls.push_back(call);
			}

			return calls;
		}

		// Called from a detour, on the game's thread, during wave spawning.
		// Everything here is defensive: an argument that is not the kind it is
		// expected to be is ignored rather than converted.
		void RememberSpawnCall(int ArgumentCount, RValue** Arguments)
		{
			const RValue* type = hooks::Argument(ArgumentCount, Arguments, 0);
			if (!type || type->m_Kind != VALUE_STRING)
				return;

			const char* raw = type->ToCString();
			if (!raw)
				return;

			const std::string code = raw;
			if (!IsTypeNameLike(code))
				return;

			// Kept regardless of whether it is new, so the spawn-plan probe
			// always has a key the game used moments ago rather than one from
			// an earlier round.
			g_LastSpawnId = code;

			const auto known = LoadSpawnCalls();
			if (std::any_of(known.begin(), known.end(),
					[&](const SpawnCall& seen) { return seen.code == code; }))
				return;

			double x = 0.0;
			double y = 0.0;

			const auto number = [&](int index, double& out)
			{
				const RValue* value = hooks::Argument(ArgumentCount, Arguments, index);
				if (!value)
					return;

				if (value->m_Kind != VALUE_REAL && value->m_Kind != VALUE_INT32 &&
					value->m_Kind != VALUE_INT64)
					return;

				const double raw_number = value->ToDouble();
				if (std::isfinite(raw_number))
					out = raw_number;
			};

			number(1, x);
			number(2, y);

			const auto path = SpawnCodePath();
			if (path.empty())
				return;

			// The round is recorded because spawn codes are round-gated:
			// replaying one from another round is what killed the game. The
			// reader requires this column, so a line written without it is
			// silently unreadable - which is exactly what happened the first
			// time this was written and the harvest looked empty.
			std::ofstream file(path, std::ios::app);
			if (file)
				file << code << " " << x << " " << y << " "
					<< CurrentWaveRound() << "\n";
		}

		// A live count of enemy instances, independent of the game's own
		// bookkeeping global.
		//
		// This is the difference between "an instance was created" and "the game
		// considers it part of the wave", and the last run showed those are not
		// the same thing: enemy_spawn returned a ref, NUM_ENEMIES_ACTIVE did not
		// move, and the player was left standing in a finished round with a
		// toddler still walking around. The instance existed. The wave did not
		// know about it.
		int CountEnemyInstances()
		{
			const int object_index = bridge::AssetIndex("o_enemy");
			if (object_index < 0)
				return -1;

			auto count = bridge::CallBuiltin("instance_number",
				{ RValue(static_cast<double>(object_index)) });

			if (!count)
				return -1;

			if (count->m_Kind != VALUE_REAL && count->m_Kind != VALUE_INT32 &&
				count->m_Kind != VALUE_INT64)
				return -1;

			const double raw = count->ToDouble();
			if (!std::isfinite(raw) || raw < 0.0)
				return -1;

			return static_cast<int>(raw);
		}

		std::filesystem::path SpawnProbeMarkerPath()
		{
			const auto directory = ModuleDirectory();
			return directory.empty()
				? std::filesystem::path{}
				: directory / L"selftest_spawn.txt";
		}

		void RunDirectSpawnProbe()
		{
			SelfTestLog("=== direct spawn probe: call the game's own enemy_spawn "
				"the way the game calls it ===");

			const std::vector<SpawnCall> calls = LoadSpawnCalls();
			if (calls.empty())
			{
				SelfTestWarn("    no spawn code has been observed yet. Play a "
					"round with this build loaded - the observers write one "
					"down as the game spawns its wave - then press F5 again");
				return;
			}

			const int round = CurrentWaveRound();

			// Codes are round-gated. This is why the second press crashed where
			// the first survived: round 1's code "x1t" was replayed in round 2,
			// whose wave is a different set entirely. The game asks
			// enemies_get_available_for_round before every wave, which is only
			// worth doing if the answer varies - so a code is only replayed in
			// the round it was seen in.
			const SpawnCall* chosen = nullptr;
			for (const SpawnCall& call : calls)
			{
				if (call.round == round)
					chosen = &call;
			}

			SelfTestLog("    %zu spawn code(s) learned; current round is %d",
				calls.size(), round);

			if (!chosen)
			{
				SelfTestWarn("    none of them was seen in round %d. Replaying a "
					"code from another round is what killed the game last time, "
					"so nothing is called. Play this round's wave in and press "
					"F5 again", round);
				return;
			}

			// A code that was attempted and never returned is not attempted
			// again. The marker is this probe's own - sharing the apply marker
			// meant a crash here disabled the apply phase instead.
			std::ifstream previous(SpawnProbeMarkerPath());
			std::string died_on;
			if (previous && (previous >> died_on) && died_on == chosen->code)
			{
				SelfTestWarn("    \"%s\" was attempted and never returned last "
					"time. Not retrying it. Delete selftest_spawn.txt to force "
					"another attempt", died_on.c_str());
				return;
			}

			const int enemies_before = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int instances_before = CountEnemyInstances();

			SelfTestLog("    calling enemy_spawn(\"%s\", %g, %g), seen in this "
				"round. If the transcript ends here, that is what to skip",
				chosen->code.c_str(), chosen->x, chosen->y);

			{
				std::ofstream marker(SpawnProbeMarkerPath(), std::ios::trunc);
				if (marker)
					marker << chosen->code;
			}

			RValue spawned;
			const bool called = bridge::CallScriptAnnounced(
				"gml_Script_enemy_spawn",
				{ RValue(chosen->code), RValue(chosen->x), RValue(chosen->y) },
				spawned);

			{
				std::error_code error;
				std::filesystem::remove(SpawnProbeMarkerPath(), error);
			}

			if (!called)
			{
				SelfTestWarn("    enemy_spawn could not be called");
				return;
			}

			const int enemies_after = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int instances_after = CountEnemyInstances();

			SelfTestLog("    SURVIVED. enemy_spawn returned %s",
				DescribeValue(spawned).c_str());

			// Two different questions, and last time they had two different
			// answers.
			SelfTestLog("    o_enemy instances    %d -> %d%s",
				instances_before, instances_after,
				instances_after > instances_before
					? "   <<< THE UNIT EXISTS"
					: "   (nothing was created)");

			SelfTestLog("    NUM_ENEMIES_ACTIVE   %d -> %d%s",
				enemies_before, enemies_after,
				enemies_after > enemies_before
					? "   <<< AND THE WAVE COUNTS IT - INJECTION WORKS"
					: "   (the wave does not count it)");

			// Clean up after the experiment.
			//
			// A unit the wave does not know about is not merely uncounted, it is
			// half-built: the wave builder does more to a fresh enemy than
			// enemy_spawn does, and whatever that is has not happened here. The
			// tester's game died on the recruit screen after the round this
			// probe ran in, with no GML error and nothing further in the log -
			// a hard fault, which is what walking a half-initialised instance
			// looks like.
			//
			// So the probe no longer leaves its subject lying in the round. A
			// diagnostic that breaks the save it was run in is not one anybody
			// can afford to press.
			if (instances_after > instances_before &&
				spawned.m_Kind != VALUE_UNDEFINED)
			{
				auto destroyed = bridge::CallBuiltin("instance_destroy", { spawned });

				const int instances_final = CountEnemyInstances();

				SelfTestLog("    cleaned up: %s, o_enemy instances now %d",
					destroyed ? "instance_destroy called" : "instance_destroy "
					"could not be called", instances_final);

				if (instances_final > instances_before)
				{
					SelfTestWarn("    THE SPAWNED UNIT COULD NOT BE REMOVED. "
						"Finish this run rather than continuing it - the round "
						"is carrying a unit the game did not build");
				}
			}
		}

		// ------------------------------------------------------------------
		// Phase 4: ask the game what may spawn in this round
		// ------------------------------------------------------------------
		//
		// enemies_get_available_for_round(round, ?) -> array, observed from the
		// game calling it before every wave. It is a getter, and it is the
		// authoritative source for the spawn-code namespace - better than the
		// harvest, which only ever learns what happened to be spawned.
		//
		// Its contents also decide whether interception is possible. If the
		// wave is built by picking from this array, then returning a different
		// array is how the opponent's army becomes the wave - and the game
		// builds it, correctly, with none of the half-initialised units this
		// session's crash was made of.
		// ------------------------------------------------------------------
		// Phase 6: find the spawn plan
		// ------------------------------------------------------------------
		//
		// Where things stand, all measured:
		//
		//   - enemy_spawn(id, x, y) carries no type. Ten identical toddlers
		//     produced ten different first arguments.
		//   - self.id on those calls reads VALUE_UNSET (0x0ffffff), and .type
		//     and .name do not read at all. The type does not come from self
		//     either; self is the o_gameplay instance whose Create event defines
		//     the caller, and instance member reads are dead on this build.
		//
		// So the type is reached THROUGH the id. The id is a key, and something
		// holds a table mapping it to what to build. That single theory explains
		// every result this project has had from spawning:
		//
		//   - a key from the current round replayed -> a unit appears, but
		//     half-built, because the entry was already consumed;
		//   - a key from another round replayed -> a hard crash, because there
		//     is no entry at all.
		//
		// The table has to be reachable from globals, and the global instance is
		// the one thing on this build that CAN be enumerated rather than
		// guessed. So: take a key the game itself just used, and find which
		// global contains it.
		void RunSpawnPlanProbe()
		{
			SelfTestLog("=== spawn-plan probe: which global holds the spawn id "
				"the game just used? ===");

			if (g_LastSpawnId.empty())
			{
				SelfTestWarn("    no spawn id seen this session - let a wave in "
					"first");
				return;
			}

			SelfTestLog("    looking for the key \"%s\"", g_LastSpawnId.c_str());

			const std::vector<std::string> globals = bridge::GlobalMemberNames();
			if (globals.empty())
			{
				SelfTestWarn("    the global instance enumerated no members");
				return;
			}

			SelfTestLog("    %zu globals to check", globals.size());

			const RValue key(g_LastSpawnId);

			int hits = 0;
			int structs = 0;

			for (const std::string& name : globals)
			{
				auto value = bridge::GetGlobal(name);
				if (!value)
					continue;

				// Structs only. A ds_map is a bare number, and calling
				// ds_map_exists on a number that is not a map id is exactly the
				// kind of thing that has killed this game before.
				const bool struct_shaped = value->m_Kind == VALUE_OBJECT ||
					value->m_Kind == VALUE_REF;

				if (!struct_shaped || !value->m_Pointer)
					continue;

				structs++;

				auto exists = bridge::CallBuiltin("variable_struct_exists",
					{ *value, key });

				if (!exists || exists->m_Kind != VALUE_BOOL || !exists->ToBoolean())
					continue;

				hits++;

				auto entry = bridge::CallBuiltin("variable_struct_get",
					{ *value, key });

				SelfTestLog("    FOUND: global.%s[\"%s\"] = %s", name.c_str(),
					g_LastSpawnId.c_str(),
					entry ? DescribeValue(*entry).c_str() : "unreadable");

				// If the entry is itself a struct, its fields are the plan.
				if (entry && (entry->m_Kind == VALUE_OBJECT ||
					entry->m_Kind == VALUE_REF) && entry->m_Pointer)
				{
					auto names = bridge::CallBuiltin("struct_get_names", { *entry });
					auto length = names && names->m_Kind == VALUE_ARRAY
						? bridge::CallBuiltin("array_length", { *names })
						: std::nullopt;

					const int fields = (length && (length->m_Kind == VALUE_REAL ||
						length->m_Kind == VALUE_INT32 ||
						length->m_Kind == VALUE_INT64))
						? static_cast<int>(length->ToDouble())
						: 0;

					for (int f = 0; f < fields && f < 32; f++)
					{
						auto field = bridge::CallBuiltin("array_get",
							{ *names, RValue(static_cast<double>(f)) });

						if (!field || field->m_Kind != VALUE_STRING)
							continue;

						const char* field_name = field->ToCString();
						if (!field_name)
							continue;

						auto field_value = bridge::CallBuiltin("variable_struct_get",
							{ *entry, *field });

						SelfTestLog("        .%s = %s", field_name,
							field_value ? DescribeValue(*field_value).c_str()
								: "unreadable");
					}
				}
			}

			SelfTestLog("    checked %d struct-valued global(s); %d contained the "
				"key", structs, hits);

			if (hits == 0)
			{
				SelfTestWarn("    no global struct holds it. The table is either "
					"a ds_map, a local of the wave builder, or the id is not a "
					"key at all - and in the last case the type must be an "
					"argument the observer is not seeing");
			}
		}

		// ------------------------------------------------------------------
		// Phase 8: the real injection
		// ------------------------------------------------------------------
		//
		// Observed, from the game calling itself:
		//
		//     gml_Script_animal_generate(1 args) = str("toddler") -> struct
		//     gml_Script_Animal(1 args)          = str("toddler") -> undefined
		//
		// animal_generate takes an enemy TYPE NAME - the same namespace as the
		// export's enemies map and the descriptors' .id - and returns a record.
		// That record is what global.ANIMALS holds, keyed by its own .id, and
		// enemy_spawn builds the instance from it.
		//
		// So the whole injection is two calls, both the game's own:
		//
		//     record = animal_generate(type)
		//     enemy_spawn(record.id, x, y)
		//
		// Nothing is invented. The stats, the callbacks and the sprites all come
		// from the game's own construction, which is the failure mode of every
		// previous attempt and the reason a spawned unit hard-faulted the game.
		void RunRealInjectionProbe()
		{
			SelfTestLog("=== injection probe: animal_generate + enemy_spawn ===");

			if (!bridge::ScriptExists("gml_Script_animal_generate"))
			{
				SelfTestWarn("    animal_generate does not exist on this build");
				return;
			}

			// A type this round legitimately contains, so nothing depends on
			// guessing what is legal. The export names them and the game
			// validated them by fighting them.
			const std::string exported = CaptureGameNativeExport();
			json::Value root;
			std::string type;

			if (json::Parse(exported, root) && root.IsObject())
			{
				const json::Value& enemies = root["enemies"];
				if (enemies.IsObject() && !enemies.Members().empty())
					type = enemies.Members().begin()->first;
			}

			if (type.empty())
			{
				SelfTestWarn("    could not read an enemy type from this round's "
					"export - nothing safe to generate");
				return;
			}

			// Real coordinates, from a spawn the game performed this round.
			const std::vector<SpawnCall> calls = LoadSpawnCalls();
			const int round = CurrentWaveRound();

			double x = 0.0;
			double y = 0.0;
			bool have_position = false;

			for (const SpawnCall& call : calls)
			{
				if (call.round == round)
				{
					x = call.x;
					y = call.y;
					have_position = true;
				}
			}

			if (!have_position)
			{
				SelfTestWarn("    no spawn position observed in round %d - let "
					"this round's wave spawn first", round);
				return;
			}

			const int instances_before = CountEnemyInstances();
			const int enemies_before = ReadGlobalCount("NUM_ENEMIES_ACTIVE");

			SelfTestLog("    calling animal_generate(\"%s\")", type.c_str());

			RValue record;
			if (!bridge::CallScriptAnnounced("gml_Script_animal_generate",
					{ RValue(type) }, record))
			{
				SelfTestWarn("    animal_generate could not be called");
				return;
			}

			SelfTestLog("    it returned %s", DescribeValue(record).c_str());

			const bool record_shaped = record.m_Kind == VALUE_OBJECT ||
				record.m_Kind == VALUE_REF;

			if (!record_shaped || !record.m_Pointer)
			{
				SelfTestWarn("    that is not a record - stopping before "
					"enemy_spawn rather than handing it something unusable");
				return;
			}

			auto id = bridge::CallBuiltin("variable_struct_get",
				{ record, RValue("id") });

			if (!id || id->m_Kind != VALUE_STRING)
			{
				SelfTestWarn("    the record has no string .id - the key the "
					"spawner needs is not where it was expected");
				return;
			}

			const char* raw_id = id->ToCString();
			if (!raw_id)
				return;

			const std::string key = raw_id;
			SelfTestLog("    the record registered itself as \"%s\"", key.c_str());

			SelfTestLog("    calling enemy_spawn(\"%s\", %g, %g)", key.c_str(),
				x, y);

			RValue spawned;
			if (!bridge::CallScriptAnnounced("gml_Script_enemy_spawn",
					{ *id, RValue(x), RValue(y) }, spawned))
			{
				SelfTestWarn("    enemy_spawn could not be called");
				return;
			}

			const int instances_after = CountEnemyInstances();
			const int enemies_after = ReadGlobalCount("NUM_ENEMIES_ACTIVE");

			SelfTestLog("    SURVIVED. enemy_spawn returned %s",
				DescribeValue(spawned).c_str());

			SelfTestLog("    o_enemy instances    %d -> %d", instances_before,
				instances_after);

			SelfTestLog("    NUM_ENEMIES_ACTIVE   %d -> %d%s",
				enemies_before, enemies_after,
				enemies_after > enemies_before
					? "   <<< THE WAVE COUNTS IT - INJECTION WORKS"
					: "   (still not counted)");

			// Cleaned up either way. A successful injection is a real extra
			// enemy in a round the tester is playing, and a diagnostic has no
			// business changing the difficulty of somebody's run.
			if (instances_after > instances_before &&
				spawned.m_Kind != VALUE_UNDEFINED)
			{
				bridge::CallBuiltin("instance_destroy", { spawned });
			}

			if (bridge::ScriptExists("gml_Script_animal_delete"))
			{
				RValue ignored;
				bridge::CallScript("gml_Script_animal_delete", { *id }, ignored);
			}

			SelfTestLog("    cleaned up: o_enemy instances now %d, "
				"NUM_ENEMIES_ACTIVE now %d", CountEnemyInstances(),
				ReadGlobalCount("NUM_ENEMIES_ACTIVE"));
		}

		// ------------------------------------------------------------------
		// Phase 7: name the routines that build an ANIMALS entry
		// ------------------------------------------------------------------
		//
		// The mechanism is now known, measured end to end:
		//
		//     global.ANIMALS["cjr"] = { .type = "toddler", .id = "cjr",
		//                               .stats, .combatant_type, .on_* ... }
		//     global.ANIMALS_CONSCIOUS["cjr"] = true
		//     enemy_spawn("cjr", x, y) -> builds the instance from that entry
		//
		// The id is a key into a combatant registry. That is why replaying a
		// used key produced a half-built unit and a foreign key crashed the
		// game, and it is why nothing in the arguments or in self carried a
		// type: the type was one lookup away the whole time.
		//
		// What remains is producing an entry. Building one field by field is
		// possible but wrong-headed - the entry holds sprite refs, a stats
		// struct and a dozen callbacks, and inventing those is exactly the kind
		// of half-built object that hard-faulted the game.
		//
		// The game makes these itself, constantly. So the last thing to find is
		// the routine that does it, and for the first time it can be looked up
		// instead of guessed: the script table is enumerable by name.
		void RunScriptNameProbe()
		{
			SelfTestLog("=== script-name probe: what is the animal factory "
				"actually called? ===");

			// Deliberately broad. Every name this project has used so far came
			// from discovered_mappings.json, which is a list of plausible
			// guesses - 'enemy_wave_spawner' does not exist, and believing it
			// did cost several sessions.
			for (const char* fragment :
				{ "animal", "combatant", "spawn", "roster_add", "wave" })
			{
				const auto names = hooks::ScriptNamesContaining(fragment, 40);

				SelfTestLog("    --- '%s': %zu match(es) ---", fragment,
					names.size());

				for (const std::string& name : names)
					SelfTestLog("        %s", name.c_str());
			}
		}

		// Name the wave builder from the return address the observer captured.
		void RunSpawnCallerProbe()
		{
			SelfTestLog("=== caller probe: what calls enemy_spawn? ===");

			const void* address = g_SpawnCallerAddress.load();
			if (!address)
			{
				SelfTestWarn("    no wave has spawned this session, so nothing "
					"has called it yet. Let a wave in and press F5 again");
				return;
			}

			size_t offset = 0;
			const std::string owner = hooks::OwningScriptName(address, offset);

			if (owner.empty())
			{
				SelfTestWarn("    the return address %p belongs to no script in "
					"the table - the caller is native runner code rather than "
					"GML, and cannot be hooked by name", address);
				return;
			}

			SelfTestLog("    enemy_spawn was called from %s + 0x%zx",
				owner.c_str(), offset);

			// An offset this large is not a function; it is a miss. YYC appears
			// to compile every method defined in an object's Create event into
			// one blob, so the script table can hold many names that resolve at
			// or near the same native address, and "greatest entry at or below"
			// then picks an arbitrary one of them.
			//
			// The name is therefore a neighbourhood, not an identification. What
			// it does establish - and this is the useful part - is that the
			// caller is a method defined in an object's event rather than a
			// top-level script, which is exactly why 'enemy_wave_spawner' never
			// resolved as something hookable by name.
			if (offset > 0x400)
			{
				SelfTestWarn("    that offset is far too large to be a function "
					"body, so the name is the nearest table entry rather than "
					"the caller itself. Treat it as 'somewhere in that object's "
					"event code'");
			}
		}

		void RunAvailableEnemiesProbe()
		{
			SelfTestLog("=== available-enemies probe: what may legally spawn in "
				"this round? ===");

			if (!bridge::ScriptExists("gml_Script_enemies_get_available_for_round"))
			{
				SelfTestWarn("    that script does not exist on this build");
				return;
			}

			const int round = CurrentWaveRound();

			SelfTestLog("    calling enemies_get_available_for_round(%d, 1) - "
				"the shape the game uses", round);

			RValue available;
			if (!bridge::CallScriptAnnounced(
					"gml_Script_enemies_get_available_for_round",
					{ RValue(static_cast<double>(round)), RValue(1.0) },
					available))
			{
				SelfTestWarn("    it could not be called");
				return;
			}

			SelfTestLog("    SURVIVED. returned %s",
				DescribeValue(available).c_str());

			if (available.m_Kind != VALUE_ARRAY)
			{
				SelfTestWarn("    not an array - nothing further can be read "
					"from it here");
				return;
			}

			auto length = bridge::CallBuiltin("array_length", { available });
			if (!length || (length->m_Kind != VALUE_REAL &&
				length->m_Kind != VALUE_INT32 && length->m_Kind != VALUE_INT64))
			{
				SelfTestWarn("    array_length would not report a number");
				return;
			}

			const int count = static_cast<int>(length->ToDouble());
			SelfTestLog("    %d entr%s:", count, count == 1 ? "y" : "ies");

			// Bounded: this goes to a file the tester reads.
			constexpr int kMaxToShow = 40;

			// The entries came back as structs, so the interesting part is
			// inside them. struct_get_names and struct_get are the game's own
			// API for that - the same class of move as using json_parse instead
			// of writing a parser, and notably NOT the instance-member reads
			// that are impossible on this build. These are plain structs the
			// game just handed us, not instances.
			//
			// The first few are dumped in full. If one of them carries both a
			// readable enemy name and the short spawn code, that is the missing
			// link between the two namespaces, and the translation stops being
			// an approximation.
			constexpr int kStructsToOpen = 3;

			for (int i = 0; i < count && i < kMaxToShow; i++)
			{
				auto element = bridge::CallBuiltin("array_get",
					{ available, RValue(static_cast<double>(i)) });

				if (!element)
				{
					SelfTestLog("        [%d] unreadable", i);
					continue;
				}

				SelfTestLog("        [%d] %s", i, DescribeValue(*element).c_str());

				const bool struct_shaped = element->m_Kind == VALUE_OBJECT ||
					element->m_Kind == VALUE_REF;

				if (i >= kStructsToOpen || !struct_shaped || !element->m_Pointer)
					continue;

				auto names = bridge::CallBuiltin("struct_get_names", { *element });
				if (!names || names->m_Kind != VALUE_ARRAY)
				{
					SelfTestWarn("            struct_get_names did not return an "
						"array - the entry cannot be opened from here");
					continue;
				}

				auto field_count = bridge::CallBuiltin("array_length", { *names });
				if (!field_count || (field_count->m_Kind != VALUE_REAL &&
					field_count->m_Kind != VALUE_INT32 &&
					field_count->m_Kind != VALUE_INT64))
					continue;

				const int fields = static_cast<int>(field_count->ToDouble());

				for (int f = 0; f < fields && f < 32; f++)
				{
					auto name = bridge::CallBuiltin("array_get",
						{ *names, RValue(static_cast<double>(f)) });

					if (!name || name->m_Kind != VALUE_STRING)
						continue;

					const char* field_name = name->ToCString();
					if (!field_name)
						continue;

					auto value = bridge::CallBuiltin("struct_get",
						{ *element, *name });

					SelfTestLog("            .%s = %s", field_name,
						value ? DescribeValue(*value).c_str() : "unreadable");
				}
			}

			if (count > kMaxToShow)
				SelfTestLog("        ... and %d more", count - kMaxToShow);
		}

		std::string ReadClipboardText()
		{
			auto has_text = bridge::CallBuiltin("clipboard_has_text", {});
			if (!has_text || !has_text->ToBoolean())
				return {};

			auto text = bridge::CallBuiltin("clipboard_get_text", {});
			if (!text || text->m_Kind != VALUE_STRING)
				return {};

			const char* raw = text->ToCString();
			return raw ? std::string(raw) : std::string{};
		}

		// Returns true when the candidate was reached and returned.
		bool RunApplyCandidate(
			const ApplyCandidate& Candidate,
			const std::string& Payload,
			int Index,
			int Last
		)
		{
			SelfTestLog("--- apply %d of %d: %s ---", Index, Last,
				Candidate.script);
			SelfTestLog("    %s", Candidate.what);

			if (!bridge::ScriptExists(Candidate.script))
			{
				SelfTestWarn("    SKIPPED - this build has no such script");
				return false;
			}

			// The player's clipboard is theirs. It is saved, borrowed and put
			// back on every path out of here, and its contents are never logged.
			std::string borrowed;
			bool clipboard_borrowed = false;

			if (Candidate.wants_payload_on_clipboard)
			{
				if (Payload.empty())
				{
					SelfTestWarn("    SKIPPED - no duel payload to place on the "
						"clipboard");
					return false;
				}

				borrowed = ReadClipboardText();
				clipboard_borrowed = true;

				bridge::CallBuiltin("clipboard_set_text", { RValue(Payload) });
				SelfTestLog("    the duel payload is on the clipboard for this "
					"call");
			}

			const auto restore = [&]()
			{
				if (clipboard_borrowed && !borrowed.empty())
					bridge::CallBuiltin("clipboard_set_text", { RValue(borrowed) });
			};

			const int enemies_before = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int dudes_before = ReadGlobalCount("NUM_DUDES_ACTIVE");

			MarkAttempted(ApplyMarkerPath(), Index);

			SelfTestLog("    HANDED TO THE GAME - if the transcript ends here, "
				"this candidate is the one that kills it");

			// Argument counts are not knowable on this build, so each candidate
			// is called with none. A routine that needs one will say so in a
			// typed error, which is recoverable and names what it wanted.
			RValue returned;
			if (!bridge::CallScriptAnnounced(Candidate.script, {}, returned))
			{
				SelfTestWarn("    SKIPPED - the call could not be made");
				ClearMarker(ApplyMarkerPath());
				restore();
				return false;
			}

			ClearMarker(ApplyMarkerPath());
			restore();

			const int enemies_after = ReadGlobalCount("NUM_ENEMIES_ACTIVE");
			const int dudes_after = ReadGlobalCount("NUM_DUDES_ACTIVE");

			SelfTestLog("    SURVIVED. returned %s",
				DescribeValue(returned).c_str());

			// This is the entire point of the probe. Surviving is not the
			// result; moving the counts is.
			const bool moved =
				(enemies_before != enemies_after) || (dudes_before != dudes_after);

			SelfTestLog("    NUM_ENEMIES_ACTIVE %d -> %d, NUM_DUDES_ACTIVE %d -> %d%s",
				enemies_before, enemies_after, dudes_before, dudes_after,
				moved ? "   <<< THE ROUND CHANGED" : "   (no effect)");

			return true;
		}
	}

	// -----------------------------------------------------------------------
	// Spawn observers
	// -----------------------------------------------------------------------
	//
	// Every route to putting a unit in a live round is now closed by
	// measurement:
	//
	//   - the matchup routines parse and never touch a running round, and
	//     custom_matchup_fightable answers FALSE after eight successful parses;
	//   - enemy types are not objects - asset_get_index finds nothing for
	//     'toddler' or 'mallard_duck' under o_, obj_, o_enemy_, obj_enemy_,
	//     enemy_, o_e_ or the bare name;
	//   - per-instance member writes were established as impossible six
	//     attempts ago.
	//
	// What remains is the routine the game uses on itself, every wave, to put
	// an enemy on the field. Its name is in discovered_mappings.json; its
	// signature is not, and YYC left no way to ask.
	//
	// Guessing an argument list costs a launch per guess. Watching the game
	// call it costs nothing and is exact - the same move that produced every
	// real result in this project. These detours only observe.
	namespace
	{
		// One sighting proves the shape; several are needed to collect the
		// vocabulary, because a round only spawns the types it uses.
		constexpr int kSpawnCallsToLog = 12;

		struct SpawnObserver
		{
			const char* hook_id;
			const char* script;
			YYTK::PFUNC_YYGMLScript trampoline;
		};


		SpawnObserver g_SpawnObservers[] = {
			{ "hmd_obs_enemy_spawn",        "gml_Script_enemy_spawn",        nullptr },
			{ "hmd_obs_enemy_wave_spawner", "gml_Script_enemy_wave_spawner", nullptr },
			{ "hmd_obs_matchup_fit",        "gml_Script_matchup_get_enemies_to_fit", nullptr },
			{ "hmd_obs_enemies_available",  "gml_Script_enemies_get_available_for_round", nullptr },

			// The factory candidates, named by reading the script table rather
			// than by guessing. One of these produces the global.ANIMALS record
			// that enemy_spawn then builds an instance from, and watching the
			// game call it is how the mod learns to call it the same way.
			{ "hmd_obs_animal_generate",    "gml_Script_animal_generate",    nullptr },
			{ "hmd_obs_animal_ctor",        "gml_Script_Animal",             nullptr },
			{ "hmd_obs_animal_get",         "gml_Script_animal_get",         nullptr },
		};

		// One detour body per observer. They cannot share one function because a
		// detour has no way to know which hook invoked it, and the trampoline it
		// must call through differs per script.
		//
		// The signature line goes to the transcript as well as the log, because
		// the log is erased by the next launch and this is the whole point of
		// running them.
		template <size_t Index>
		RValue& SpawnObserverDetour(
			CInstance* Self,
			CInstance* Other,
			RValue& Result,
			int ArgumentCount,
			RValue** Arguments
		)
		{
			SpawnObserver& observer = g_SpawnObservers[Index];

			if (observer.trampoline)
				observer.trampoline(Self, Other, Result, ArgumentCount, Arguments);

			// enemy_spawn is the one that matters, and one sighting is not
			// enough: its first argument is a code from a namespace nothing else
			// uses, so the codes have to be collected the way the enemy type
			// names were.
			if (Index == 0)
			{
				RememberSpawnCall(ArgumentCount, Arguments);

				// Who called enemy_spawn?
				//
				// That is the wave builder, and it is the routine the mod
				// actually needs: enemy_spawn alone leaves a half-built unit
				// that hard-faults the game after the round, so whatever the
				// caller does around it is the missing part.
				//
				// Its name is not in discovered_mappings.json in any form that
				// resolved - enemy_wave_spawner does not exist under that name -
				// and YYC left no call graph. The return address is the fact
				// that replaces the guessing.
				//
				// Captured here and resolved later: naming it means walking the
				// entire script table, which is not something to do on the
				// game's thread in the middle of wave spawning.
				if (!g_SpawnCallerAddress.load())
					g_SpawnCallerAddress.store(_ReturnAddress());

				// What is 'self' when the game calls enemy_spawn?
				//
				// This is now the central question. The three arguments carry no
				// type: 54 harvested first-arguments are 54 distinct strings,
				// twenty-one of them in round 4 alone, and round 1 produced ten
				// different ones for ten identical toddlers. They are unique
				// per-spawn ids, not type names.
				//
				// So the type has to arrive some other way, and 'self' is where
				// a GML method keeps it. The available-enemies array returns
				// full type descriptors carrying .id ("toddler"), .hp,
				// .offensive_power and a set of on_* callbacks - exactly the
				// object a spawn method would be bound to.
				//
				// If self.id reads back as an enemy type name, then the reason
				// the mod's own spawn produced a half-built unit is simply that
				// it was called with the wrong self - and calling it with the
				// right one is the whole injection step.
				//
				// Read here rather than later because the object is certainly
				// alive here. GetMember checks kinds and returns nothing rather
				// than converting anything it does not recognise.
				static int selves_read = 0;
				if (selves_read < 3 && Self)
				{
					selves_read++;

					const RValue self_value(Self);

					for (const char* field : { "id", "type", "name" })
					{
						auto value = bridge::GetMember(self_value, field);
						if (!value)
							continue;

						char line[512]{};
						_snprintf_s(line, sizeof(line) - 1, _TRUNCATE,
							"    enemy_spawn's self.%s = %s", field,
							DescribeValue(*value).c_str());

						LogInfo("%s", line);
						AppendToTranscript(line);
					}
				}
			}

			// enemies_get_available_for_round(round, ?) - the game stating, in
			// its own numbering, which round it is about to build a wave for.
			// Codes are stamped and matched against this rather than against the
			// mod's inferred round.
			if (Index == 3)
			{
				const RValue* seen =
					hooks::Argument(ArgumentCount, Arguments, 0);

				if (seen && (seen->m_Kind == VALUE_REAL ||
					seen->m_Kind == VALUE_INT32 || seen->m_Kind == VALUE_INT64))
				{
					const double raw = seen->ToDouble();
					if (std::isfinite(raw) && raw >= 0.0)
						g_GameWaveRound.store(static_cast<int>(raw));
				}
			}

			static int logged = 0;
			if (logged < kSpawnCallsToLog)
			{
				logged++;

				std::string rendered;
				for (int i = 0; i < ArgumentCount; i++)
				{
					if (i > 0)
						rendered += ", ";
					rendered += hooks::Describe(
						hooks::Argument(ArgumentCount, Arguments, i));
				}

				AppendToTranscript("SPAWN OBSERVER - the game called a spawn "
					"routine and this is exactly how:");

				char line[1024]{};
				_snprintf_s(line, sizeof(line) - 1, _TRUNCATE,
					"    %s(%d args)%s%s -> %s",
					observer.script, ArgumentCount,
					rendered.empty() ? "" : " = ", rendered.c_str(),
					hooks::Describe(&Result).c_str());

				LogInfo("%s", line);
				AppendToTranscript(line);
			}

			return Result;
		}
	}

	bool InstallSpawnObservers(Aurie::AurieModule* Module)
	{
		g_SpawnObservers[0].trampoline = hooks::Install(Module,
			g_SpawnObservers[0].hook_id, g_SpawnObservers[0].script,
			&SpawnObserverDetour<0>);

		g_SpawnObservers[1].trampoline = hooks::Install(Module,
			g_SpawnObservers[1].hook_id, g_SpawnObservers[1].script,
			&SpawnObserverDetour<1>);

		g_SpawnObservers[2].trampoline = hooks::Install(Module,
			g_SpawnObservers[2].hook_id, g_SpawnObservers[2].script,
			&SpawnObserverDetour<2>);

		g_SpawnObservers[3].trampoline = hooks::Install(Module,
			g_SpawnObservers[3].hook_id, g_SpawnObservers[3].script,
			&SpawnObserverDetour<3>);

		g_SpawnObservers[4].trampoline = hooks::Install(Module,
			g_SpawnObservers[4].hook_id, g_SpawnObservers[4].script,
			&SpawnObserverDetour<4>);

		g_SpawnObservers[5].trampoline = hooks::Install(Module,
			g_SpawnObservers[5].hook_id, g_SpawnObservers[5].script,
			&SpawnObserverDetour<5>);

		g_SpawnObservers[6].trampoline = hooks::Install(Module,
			g_SpawnObservers[6].hook_id, g_SpawnObservers[6].script,
			&SpawnObserverDetour<6>);

		int installed = 0;
		for (const SpawnObserver& observer : g_SpawnObservers)
		{
			if (observer.trampoline)
				installed++;
			else
				LogWarn("spawn observer '%s' could not be installed - that "
					"routine's signature will stay unknown", observer.script);
		}

		LogInfo("spawn observers: %d of %d installed. They only watch; the first "
			"call to each is written to selftest_transcript.log",
			installed, static_cast<int>(std::size(g_SpawnObservers)));

		return installed > 0;
	}

	void RemoveSpawnObservers(Aurie::AurieModule* Module)
	{
		for (SpawnObserver& observer : g_SpawnObservers)
		{
			hooks::Remove(Module, observer.hook_id);
			observer.trampoline = nullptr;
		}
	}

	void SelfTestDuelPayload(bool RestartFromFirstStage)
	{
		SelfTestLog("=== duel payload self-test (staged) ===");

		// Read the marker before anything writes one.
		const int did_not_return = ReadStageThatDidNotReturn();
		int first = 0;

		if (RestartFromFirstStage)
		{
			ClearStageMarker();
			SelfTestLog("shift was held - starting again from stage 0");
		}
		else if (did_not_return >= 0)
		{
			first = did_not_return + 1;
			SelfTestLog("stage %d was attempted and never returned, so it is "
				"what killed the game. Resuming at stage %d. Hold shift with F5 "
				"to start over from 0.", did_not_return, first);
		}

		const std::string exported = CaptureGameNativeExport();
		if (exported.empty())
		{
			SelfTestWarn("self-test: nothing exported - cannot continue");
			return;
		}

		// Every remaining question about this format is a question about its
		// values rather than its shape, and "scalar" does not distinguish a
		// string from a number - which is how boss_fight_id hid for nine
		// sessions.
		SelfTestLog("self-test: the export, verbatim (%zu bytes)", exported.size());
		LogTextInSlices("export", exported);

		DescribeMatchupExport(exported);

		if (!bridge::ScriptExists("gml_Script_custom_matchup_parse"))
		{
			SelfTestWarn("self-test: custom_matchup_parse does not exist - the "
				"injection half of the duel has no route at all");
			return;
		}

		const std::vector<SelfTestStage> stages = BuildSelfTestStages(exported);
		if (stages.empty())
		{
			SelfTestWarn("self-test: no stages could be built from the export");
			return;
		}

		const int last = static_cast<int>(stages.size()) - 1;

		if (first > last)
		{
			SelfTestWarn("self-test: every stage has already been attempted. "
				"Hold shift with F5 to run them again from 0.");
			ClearStageMarker();
			return;
		}

		SelfTestLog("self-test: %d stages, running %d through %d. Each one loads "
			"a matchup, so the round is expected to change as this goes.",
			static_cast<int>(stages.size()), first, last);

		int reached = 0;
		int skipped = 0;

		for (int index = first; index <= last; index++)
		{
			if (RunSelfTestStage(stages[static_cast<size_t>(index)], index, last))
				reached++;
			else
				skipped++;
		}

		// Skipped is not survived. A stage the mod refused to hand over says
		// nothing about what the game would have done with it.
		SelfTestLog("=== payload stages %d-%d: %d reached the game and survived, "
			"%d never got that far ===", first, last, reached, skipped);

		// ------------------------------------------------------------------
		// Phase 2: find the routine that actually applies a parsed matchup.
		// ------------------------------------------------------------------
		//
		// Every payload stage survives and none of them move
		// NUM_ENEMIES_ACTIVE, so parsing is not applying and the format work is
		// finished. This is the step that remains.
		const std::string duel_payload = BuildDuelPayload(exported);

		const int apply_did_not_return =
			ReadIndexThatDidNotReturn(ApplyMarkerPath());

		int first_apply = 0;

		if (RestartFromFirstStage)
		{
			ClearMarker(ApplyMarkerPath());
		}
		else if (apply_did_not_return >= 0)
		{
			first_apply = apply_did_not_return + 1;
			SelfTestLog("apply candidate %d was attempted and never returned, so "
				"it is what killed the game. Resuming at %d.",
				apply_did_not_return, first_apply);
		}

		const int last_apply =
			static_cast<int>(std::size(kApplyCandidates)) - 1;

		if (first_apply > last_apply)
		{
			SelfTestWarn("=== every apply candidate has been attempted; skipping "
				"to the direct spawn probe. Hold shift with F5 to run them "
				"again from the first ===");
		}
		else
		{
			SelfTestLog("=== apply probe: which routine makes a parsed matchup "
				"take effect? Running %d through %d ===", first_apply, last_apply);

			for (int index = first_apply; index <= last_apply; index++)
			{
				RunApplyCandidate(kApplyCandidates[static_cast<size_t>(index)],
					duel_payload, index, last_apply);
			}
		}

		// Phase 3 runs regardless. It does not depend on anything the matchup
		// routines did, and it is the route that does not need them.
		RunDirectSpawnProbe();

		// Ordered after the spawn probe so that if the spawn kills the game,
		// this still runs on the next press once the marker skips that code.
		RunAvailableEnemiesProbe();
		RunSpawnCallerProbe();
		RunSpawnPlanProbe();
		RunScriptNameProbe();
		RunRealInjectionProbe();

		SelfTestLog("=== end of self-test ===");
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
