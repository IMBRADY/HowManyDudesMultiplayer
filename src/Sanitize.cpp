// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Sanitize.h"

#include <cmath>

namespace hmd::sanitize
{
	namespace
	{
		// Top-level keys written by the game's own custom-matchup exporter.
		// Confirmed present as exact strings in data.win's STRG chunk, next to
		// "DevTools/custom_matchup.json" and the importer's own
		// "Incorrect format for parsing Custom Matchup data." message.
		constexpr const char* kMatchupKeys[] = {
			"dudes", "enemies", "relics", "trinkets",
			"consumables", "arenamods", "cash", "food", "boss"
		};

		int CountMatchupKeys(const json::Value& Object)
		{
			int found = 0;
			for (const char* key : kMatchupKeys)
			{
				if (Object.Has(key))
					found++;
			}
			return found;
		}
	}

	double ClampNumber(double Value, double Minimum, double Maximum)
	{
		// NaN fails every comparison, so test for finiteness explicitly rather
		// than relying on the clamp below to catch it.
		if (!std::isfinite(Value))
			return Minimum;

		if (Value < Minimum)
			return Minimum;

		if (Value > Maximum)
			return Maximum;

		return Value;
	}

	std::string ClampText(const std::string& Value)
	{
		std::string out;
		out.reserve(Value.size() < kMaxTextField ? Value.size() : kMaxTextField);

		for (unsigned char c : Value)
		{
			if (out.size() >= kMaxTextField)
				break;

			// Control characters would let a peer inject line breaks into the
			// console log; DEL is dropped for the same reason.
			if (c < 0x20 || c == 0x7F)
				continue;

			out.push_back(static_cast<char>(c));
		}

		return out;
	}

	bool IsMatchupPayload(const std::string& Candidate)
	{
		if (Candidate.empty() || Candidate.size() > kMaxMatchupBytes)
			return false;

		json::Value root;
		if (!json::Parse(Candidate, root) || !root.IsObject())
			return false;

		if (CountMatchupKeys(root) >= kMinMatchupKeys)
			return true;

		// Tolerate one level of wrapping (e.g. a version header around a data
		// object). Bounded to a single level on purpose - this is a shape check,
		// not a search.
		for (const auto& [name, member] : root.Members())
		{
			(void)name;
			if (member.IsObject() && CountMatchupKeys(member) >= kMinMatchupKeys)
				return true;
		}

		return false;
	}
}
