// SPDX-FileCopyrightText: 2026 Braden Atzert
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// The probe journal: what has already been asked, and what asking it cost.
//
// A probe that can take the game down answers one question per launch, and the
// only way that is not ruinous is if the next launch knows where the last one
// got to. Two hand-rolled markers used to do this - selftest_stage.txt for the
// payload bisect and selftest_apply.txt for the apply candidates - each holding
// a single integer meaning "this index was attempted and never returned".
//
// That worked, and it has two faults that this replaces:
//
//   1. An integer index is positional. Insert a stage at the front and every
//      recorded index silently means a different thing, so a bisect resumes
//      past the wrong entry and reports a result for a probe that never ran.
//      Ids here are stable strings, chosen by the probe and never reused.
//
//   2. "Resume at N+1" only knows the high-water mark. It cannot say which
//      entries survived and which killed the game, so the record of the whole
//      bisect lived in the transcript and in the reader's memory. This is a
//      journal: every attempt and every survival is a line, so the set of
//      lethal probes is a query rather than a recollection.
//
// The format is two whitespace-separated tokens per line, `<id> <status>`, and
// this header's implementation is the only thing that writes one or reads one.
// The last time a writer and a reader disagreed about a field count, a probe
// spent two rounds of play reporting "nothing to work with" while sitting on a
// full file - so the format has exactly one owner, and that owner is tested
// offline rather than in the game.
//
#include <filesystem>
#include <string>
#include <vector>

namespace hmd::journal
{
	enum class Status
	{
		// Written immediately BEFORE a probe calls into the game. A line that
		// is never followed by Survived is the signature of a probe that took
		// the process with it - which is the measurement, not a failure of it.
		Attempted,

		// Written immediately after the probe returns.
		Survived,

		// The probe declined to run: a precondition was not met, the script
		// does not exist on this build, there was nothing to feed it. This is
		// deliberately NOT a consumption - the question is still open and the
		// next launch will ask it again. Recorded only so the transcript can
		// show that the entry was considered.
		Skipped,
	};

	struct Entry
	{
		std::string id;
		Status status = Status::Skipped;
	};

	// Render one line, newline included. The only place a line is formatted.
	std::string FormatLine(const std::string& Id, Status State);

	// Read lines back. Malformed lines - wrong token count, unknown status,
	// empty id - are ignored rather than aborting the parse, so a journal that
	// was being written when the game died still yields everything before the
	// torn line. The only place a line is read.
	std::vector<Entry> Parse(const std::string& Text);

	// A journal backed by a file, appended to and never truncated.
	//
	// The membership queries answer against the journal as it stands, including
	// what this session has recorded, so an entry answered a moment ago counts
	// as answered. That is what makes "how many questions still need a launch
	// of their own" answerable at the end of a press rather than only at the
	// start of the next one.
	class Journal
	{
	public:
		explicit Journal(std::filesystem::path Path);

		// Append one line and flush it. The file is opened and closed per line
		// on purpose: the next thing that happens may be the process dying, and
		// a buffered line would not survive that.
		void Record(const std::string& Id, Status State);

		// This id was attempted at some point before this session, so it has
		// had its launch. True whether it survived or not.
		bool WasEverAttempted(const std::string& Id) const;

		// This id was attempted and no survival was ever recorded for it, so it
		// is what ended some previous launch.
		bool IsProvenLethal(const std::string& Id) const;

		// Every id in each of those two categories, in first-seen order.
		std::vector<std::string> Attempted() const;
		std::vector<std::string> Lethal() const;

		// Forget everything. Used by the "start over" path.
		void Clear();

		const std::filesystem::path& Path() const { return m_Path; }

	private:
		void Load();

		std::filesystem::path m_Path;
		std::vector<Entry> m_Entries;
	};
}
