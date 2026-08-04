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

	std::string ClampNotification(const std::string& Value)
	{
		// Anything the markup parser might treat as a delimiter. Replaced with a
		// space rather than deleted so that stripping a bracket cannot silently
		// weld two words together, and so a message made entirely of markup
		// collapses to whitespace and is then rejected by the trim below.
		auto is_markup = [](unsigned char c)
		{
			switch (c)
			{
			case '[': case ']':
			case '{': case '}':
			case '<': case '>':
			case '|': case '\\':
			case '^': case '~':
			case '$': case '#':
				return true;
			default:
				return false;
			}
		};

		std::string out;
		out.reserve(Value.size() < kMaxNotification ? Value.size() : kMaxNotification);

		for (unsigned char c : Value)
		{
			if (out.size() >= kMaxNotification)
				break;

			// Non-ASCII is dropped rather than passed through: the message is
			// assembled byte-wise from several sources, so a truncated multi-byte
			// sequence is possible, and a lone continuation byte is exactly the
			// kind of thing that produces an empty parsed name.
			if (c < 0x20 || c >= 0x7F)
				continue;

			const char replacement = is_markup(c) ? ' ' : static_cast<char>(c);

			// Collapse runs of whitespace, including runs created by stripping.
			if (replacement == ' ' && (out.empty() || out.back() == ' '))
				continue;

			out.push_back(replacement);
		}

		while (!out.empty() && out.back() == ' ')
			out.pop_back();

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
