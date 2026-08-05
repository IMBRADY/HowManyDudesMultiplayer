// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProbeJournal.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace hmd::journal
{
	namespace
	{
		const char* StatusToken(Status State)
		{
			switch (State)
			{
			case Status::Attempted: return "attempted";
			case Status::Survived:  return "survived";
			case Status::Skipped:   return "skipped";
			}

			return "skipped";
		}

		bool TokenToStatus(const std::string& Token, Status& Out)
		{
			if (Token == "attempted")
			{
				Out = Status::Attempted;
				return true;
			}

			if (Token == "survived")
			{
				Out = Status::Survived;
				return true;
			}

			if (Token == "skipped")
			{
				Out = Status::Skipped;
				return true;
			}

			return false;
		}

		// An id with whitespace in it would write a line the parser reads as a
		// different id plus a junk status, so the two-token contract is enforced
		// rather than trusted. Probe ids are compile-time constants, so this can
		// only fire during development.
		//
		// Everything that stores or writes an id goes through this, so what is
		// held in memory is always what a later launch will parse back. A writer
		// and a reader disagreeing about a line's shape has already cost this
		// project two rounds of play.
		std::string SanitizeId(const std::string& Id)
		{
			std::string safe;
			safe.reserve(Id.size());

			for (const char character : Id)
				safe.push_back(std::isspace(static_cast<unsigned char>(character))
					? '_' : character);

			return safe.empty() ? std::string("unnamed") : safe;
		}
	}

	std::string FormatLine(const std::string& Id, Status State)
	{
		return SanitizeId(Id) + " " + StatusToken(State) + "\n";
	}

	std::vector<Entry> Parse(const std::string& Text)
	{
		std::vector<Entry> entries;

		std::istringstream lines(Text);
		std::string line;

		while (std::getline(lines, line))
		{
			// Line at a time, not a streamed field chain over the whole file.
			// A chain stops at the first line it cannot match and takes every
			// later line with it, which turns one torn line - the ordinary
			// result of the process dying mid-write - into an empty journal.
			std::istringstream fields(line);

			std::string id;
			std::string token;

			if (!(fields >> id >> token))
				continue;

			Entry entry;
			entry.id = id;

			if (!TokenToStatus(token, entry.status))
				continue;

			entries.push_back(entry);
		}

		return entries;
	}

	Journal::Journal(std::filesystem::path Path)
		: m_Path(std::move(Path))
	{
		Load();
	}

	void Journal::Load()
	{
		m_Entries.clear();

		if (m_Path.empty())
			return;

		std::ifstream file(m_Path, std::ios::binary);
		if (!file)
			return;

		std::ostringstream contents;
		contents << file.rdbuf();

		m_Entries = Parse(contents.str());
	}

	void Journal::Record(const std::string& Id, Status State)
	{
		// The in-memory view is updated whether or not the file can be written,
		// so a read-only game folder degrades to "this session remembers" rather
		// than to "every probe looks unasked".
		Entry entry;
		entry.id = SanitizeId(Id);
		entry.status = State;
		m_Entries.push_back(entry);

		if (m_Path.empty())
			return;

		std::ofstream file(m_Path, std::ios::app | std::ios::binary);
		if (!file)
			return;

		file << FormatLine(Id, State);
	}

	bool Journal::WasEverAttempted(const std::string& Id) const
	{
		const std::string wanted = SanitizeId(Id);

		return std::any_of(m_Entries.begin(), m_Entries.end(),
			[&](const Entry& entry)
			{
				return entry.id == wanted && entry.status == Status::Attempted;
			});
	}

	bool Journal::IsProvenLethal(const std::string& Id) const
	{
		if (!WasEverAttempted(Id))
			return false;

		const std::string wanted = SanitizeId(Id);

		const bool survived = std::any_of(m_Entries.begin(), m_Entries.end(),
			[&](const Entry& entry)
			{
				return entry.id == wanted && entry.status == Status::Survived;
			});

		return !survived;
	}

	std::vector<std::string> Journal::Attempted() const
	{
		std::vector<std::string> ids;

		for (const Entry& entry : m_Entries)
		{
			if (entry.status != Status::Attempted)
				continue;

			if (std::find(ids.begin(), ids.end(), entry.id) == ids.end())
				ids.push_back(entry.id);
		}

		return ids;
	}

	std::vector<std::string> Journal::Lethal() const
	{
		std::vector<std::string> ids;

		for (const std::string& id : Attempted())
		{
			if (IsProvenLethal(id))
				ids.push_back(id);
		}

		return ids;
	}

	void Journal::Clear()
	{
		m_Entries.clear();

		if (m_Path.empty())
			return;

		std::error_code error;
		std::filesystem::remove(m_Path, error);
	}
}
