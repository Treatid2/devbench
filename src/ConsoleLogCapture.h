#pragma once

#include "ConsoleCaptureLogic.h"

#include <string>
#include <cstdint>
#include <vector>

// Output is read from a hook on ConsoleLog::VPrint that sees every line printed during the capture.
// Without the hook it falls back to ConsoleLog::buffer, which the game stops filling once the Console
// menu exists, else to a sampler of lastMessage that keeps only the last line printed per frame.
namespace dvb::ConsoleLogCapture
{
	struct Result
	{
		std::uint64_t windowId = 0;
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		std::vector<std::string> lines;

		/// "print", "buffer", "sampler", or "none" if no capture has seen its begin marker.
		std::string source = "none";
		/// The sampler can lose lines; print loss includes limits/format/allocation failures.
		bool lossPossible = false;
		/// The end marker never arrived, so `lines` may be incomplete.
		bool timedOut = false;

		bool        consoleLogNull = false;
		bool        bufferEmpty = false;
		std::size_t bufferLen = 0;
		bool        bufferHasBegin = false;
		std::string lastMessage;
		bool        lastMessageHasBegin = false;
		bool        consoleMenuExists = false;
		bool        consoleMenuOpen = false;
		bool        consoleMode = false;

		bool        printHooked = false;
		std::size_t printLines = 0;
		std::size_t printDropped = 0;
		PrintLossCounts printLoss;
		std::size_t printPayloadLines = 0;
		std::size_t printPayloadBytes = 0;

		std::size_t ringLines = 0;
		std::size_t samples = 0;
		std::size_t ticks = 0;
		std::size_t engineFrames = 0;
	};

	/// Hooks ConsoleLog::VPrint so captures see every printed line. Leaves the older sources in use
	/// when the function's first bytes are not a form it can safely relocate. Main thread, once.
	void InstallPrintHook();

	/// Runs a_command fenced; completed=false if no end arrived. Throws 409 if
	/// active/draining, 504 for a missing begin (payload not run) or uncertain
	/// task/drain failure (no replay). Listener thread only.
	struct CaptureOutcome { bool completed; std::uint64_t windowId; };
	CaptureOutcome RunFencedCapture(const std::string& a_command);

	/// Copies the last closed, drained window's immutable snapshot. Any thread;
	/// no live engine reads. Throws 409 while a window is active or draining.
	Result ReadFenced(std::size_t a_maxLines = 200, std::uint64_t a_windowId = 0);

	/// Exact active fence commands only; used by ConsoleHook to omit internal
	/// commands from recordings, never a prefix/substring suppression rule.
	bool IsCaptureControlCommand(std::string_view a_command);
}
