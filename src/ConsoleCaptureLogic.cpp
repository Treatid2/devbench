#include "ConsoleCaptureLogic.h"

#include <algorithm>

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		int Traverse(VFormatter a_formatter, char* a_dst, std::size_t a_size,
			const char* a_format, std::va_list a_args)
		{
			std::va_list copy;
			va_copy(copy, a_args);
			try {
				const int result = a_formatter(a_dst, a_size, a_format, copy);
				va_end(copy);
				return result;
			} catch (...) {
				va_end(copy);
				throw;
			}
		}

		bool Contains(std::string_view a_text, const char* a_token)
		{
			return a_text.find(a_token) != std::string_view::npos;
		}

		void TrimToMostRecent(std::vector<std::string>& a_lines, std::size_t a_maxLines)
		{
			if (a_lines.size() > a_maxLines)
				a_lines.erase(a_lines.begin(), a_lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		}
	}

	FormattedPrint FormatPrint(const char* a_format, std::va_list a_args,
		std::size_t a_budget, VFormatter a_formatter, PrintAllocator a_allocator)
	{
		FormattedPrint out;
		if (!a_format || !a_formatter) {
			out.loss = PrintLoss::kFormat;
			return out;
		}
		const auto budget = std::min(a_budget, kMaxPrintBytes);
		char head[1024];
		int length;
		try {
			length = Traverse(a_formatter, head, sizeof(head), a_format, a_args);
		} catch (...) {
			out.loss = PrintLoss::kFormat;
			return out;
		}
		if (length < 0) {
			out.loss = PrintLoss::kFormat;
			return out;
		}
		const auto size = static_cast<std::size_t>(length);
		if (size > budget) {
			out.loss = PrintLoss::kOversize;
			return out;
		}
		try {
			if (size < sizeof(head)) {
				// Keep allocation injection on the short path as well.
				if (a_allocator) a_allocator(out.text, size);
				out.text.assign(head, size);
			} else {
				if (a_allocator) a_allocator(out.text, size + 1);
				else out.text.resize(size + 1);
				// The seam must obey the exact requested allocation contract.
				if (out.text.size() != size + 1) {
					out.text.clear();
					out.loss = PrintLoss::kAllocation;
					return out;
				}
				int written;
				try {
					written = Traverse(a_formatter, out.text.data(), out.text.size(), a_format, a_args);
				} catch (...) {
					written = -1;
				}
				if (written != length) {
					out.text.clear();
					out.loss = PrintLoss::kFormat;
					return out;
				}
				out.text.resize(size);
			}
		} catch (...) {
			out.text.clear();
			out.loss = PrintLoss::kAllocation;
		}
		return out;
	}

	FenceState FindFence(std::string_view a_text, std::size_t a_fromOffset)
	{
		FenceState        state;
		const std::size_t begin = a_text.rfind(kMarkerBegin);
		if (begin == std::string_view::npos || begin < a_fromOffset)
			return state;
		state.hasBegin = true;
		state.hasEnd = a_text.find(kMarkerEnd, begin) != std::string_view::npos;
		return state;
	}

	Slice SliceFencedText(std::string_view a_text, std::size_t a_maxLines, std::size_t a_fromOffset)
	{
		Slice             out;
		const std::size_t begin = a_text.rfind(kMarkerBegin);
		if (begin == std::string_view::npos || begin < a_fromOffset)
			return out;
		out.sawBegin = true;
		const std::size_t end = a_text.find(kMarkerEnd, begin);

		std::size_t start = a_text.find('\n', begin);
		start = (start == std::string_view::npos) ? a_text.size() : start + 1;
		std::size_t stop = a_text.size();
		if (end != std::string_view::npos) {
			out.sawEnd = true;
			const std::size_t lineStart = a_text.rfind('\n', end);
			stop = (lineStart == std::string_view::npos || lineStart < start) ? start : lineStart;
		}

		std::string line;
		const auto  flush = [&]() {
			if (!line.empty()) {
				out.lines.push_back(line);
				line.clear();
			}
		};
		for (const char c : a_text.substr(start, stop - start)) {
			if (c == '\n' || c == '\r')
				flush();
			else
				line += c;
		}
		flush();
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	Slice SliceFencedLines(const std::deque<std::string>& a_lines, std::size_t a_maxLines)
	{
		Slice       out;
		std::size_t begin = a_lines.size();
		for (std::size_t i = a_lines.size(); i-- > 0;) {
			if (Contains(a_lines[i], kMarkerBegin)) {
				begin = i;
				break;
			}
		}
		if (begin >= a_lines.size())
			return out;
		out.sawBegin = true;

		std::size_t end = a_lines.size();
		for (std::size_t i = begin + 1; i < a_lines.size(); ++i) {
			if (Contains(a_lines[i], kMarkerEnd)) {
				end = i;
				out.sawEnd = true;
				break;
			}
		}
		for (std::size_t i = begin + 1; i < end; ++i)
			if (!a_lines[i].empty())
				out.lines.push_back(a_lines[i]);
		TrimToMostRecent(out.lines, a_maxLines);
		return out;
	}

	void LineSampler::Reset(std::string a_seedLine)
	{
		m_lines.clear();
		m_lastSeen = std::move(a_seedLine);
		m_samples = 0;
		m_ticks = 0;
		m_sawBegin = false;
		m_sawEnd = false;
	}

	void LineSampler::Record(std::string_view a_line)
	{
		m_lastSeen.assign(a_line);
		++m_samples;
		m_lines.emplace_back(a_line);
		while (m_lines.size() > kRingMax)
			m_lines.pop_front();
	}

	LineSampler::Seen LineSampler::Observe(std::string_view a_line)
	{
		++m_ticks;
		if (a_line.empty())
			return Seen::kNothing;
		// A marker is recorded the first time it shows even if it matches the seed, so a stale
		// marker left by an aborted capture cannot swallow this capture's own.
		if (Contains(a_line, kMarkerBegin) && !m_sawBegin) {
			m_sawBegin = true;
			Record(a_line);
			return Seen::kBegin;
		}
		if (Contains(a_line, kMarkerEnd) && m_sawBegin && !m_sawEnd) {
			m_sawEnd = true;
			Record(a_line);
			return Seen::kEnd;
		}
		if (a_line == m_lastSeen)
			return Seen::kNothing;
		Record(a_line);
		return Seen::kLine;
	}

	SourceChooser::Choice SourceChooser::Look(bool a_bufferHasBegin, bool a_samplerSawBegin)
	{
		m_bufferStreak = a_bufferHasBegin ? m_bufferStreak + 1 : 0;
		if (a_samplerSawBegin)
			++m_samplerLooks;
		if (m_bufferStreak >= kStableLooks)
			return Choice::kBuffer;
		if (m_samplerLooks >= kStableLooks)
			return Choice::kSampler;
		return Choice::kUndecided;
	}

	bool QuietDetector::Look(bool a_newLine)
	{
		if (a_newLine) {
			m_sawLine = true;
			m_quiet = 0;
		} else if (m_sawLine) {
			++m_quiet;
		}
		return m_sawLine && m_quiet >= kQuietLooks;
	}

	void PrintCollector::Reset()
	{
		m_lines.clear();
		m_loss = {};
		m_payloadLines = 0;
		m_payloadBytes = 0;
		m_sawBegin = false;
		m_sawEnd = false;
	}

	void PrintCollector::Line(std::string_view a_line)
	{
		if (m_sawEnd || a_line.empty())
			return;
		if (!m_sawBegin) {
			if (!Contains(a_line, kMarkerBegin))
				return;
			m_lines.emplace_back(a_line);
			m_sawBegin = true;
			return;
		} else if (Contains(a_line, kMarkerEnd)) {
			m_lines.emplace_back(a_line);
			m_sawEnd = true;
			return;
		}
		// Fences do not consume the advertised payload capacity. Control records
		// are retained even when payload is full; two independently bounded slots.
		if (m_payloadLines >= kMaxLines) {
			++m_loss.lineLimit;
			return;
		}
		if (a_line.size() > kMaxPayloadBytes - m_payloadBytes) {
			++m_loss.byteLimit;
			return;
		}
		m_lines.emplace_back(a_line);
		++m_payloadLines;
		m_payloadBytes += a_line.size();
	}

	std::size_t PrintCollector::FormattingBudget() const
	{
		// Reserve a small temporary allowance for an end-control record when the
		// payload is full. Payload publication still checks the exact byte budget.
		return std::min(kMaxPrintBytes, std::max<std::size_t>(256, kMaxPayloadBytes - m_payloadBytes));
	}

	void PrintCollector::RecordLoss(PrintLoss a_loss)
	{
		switch (a_loss) {
		case PrintLoss::kFormat: ++m_loss.format; break;
		case PrintLoss::kAllocation: ++m_loss.allocation; break;
		case PrintLoss::kOversize: ++m_loss.oversize; break;
		case PrintLoss::kNone: break;
		}
	}

	void PrintCollector::Feed(std::string_view a_text)
	{
		std::size_t start = 0;
		for (std::size_t i = 0; i <= a_text.size(); ++i) {
			if (i == a_text.size() || a_text[i] == '\n' || a_text[i] == '\r') {
				Line(a_text.substr(start, i - start));
				start = i + 1;
			}
		}
	}
}
