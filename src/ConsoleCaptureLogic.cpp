#include "ConsoleCaptureLogic.h"

#include <algorithm>
#include <stdexcept>
#include <iterator>

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

		// Only complete CR/LF-delimited lines match; an offset within a line may
		// not turn its suffix into a control record.
		template <class Visit>
		void VisitLines(std::string_view a_text, Visit a_visit)
		{
			std::size_t start = 0;
			for (std::size_t i = 0; i <= a_text.size(); ++i) {
				if (i == a_text.size() || a_text[i] == '\n' || a_text[i] == '\r') {
					a_visit(start, a_text.substr(start, i - start));
					start = i + 1;
				}
			}
		}

		void AppendBounded(std::deque<std::string>& a_lines, std::size_t& a_bytes,
			std::string_view a_line, std::size_t a_maxLines, bool& a_loss)
		{
			if (a_maxLines == 0 || a_line.size() > kMaxPayloadBytes) { a_loss = true; return; }
			while (!a_lines.empty() && (a_lines.size() >= a_maxLines || a_line.size() > kMaxPayloadBytes - a_bytes)) {
				a_bytes -= a_lines.front().size();
				a_lines.pop_front();
				a_loss = true;
			}
			a_lines.emplace_back(a_line);
			a_bytes += a_line.size();
		}
	}

	Fence::Fence(std::string a_nonce)
	{
		if (a_nonce.size() != 32 || !std::all_of(a_nonce.begin(), a_nonce.end(), [](char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		})) throw std::invalid_argument("console fence requires 128-bit lowercase hex nonce");
		beginCommand = "DVBCAPBEGIN_" + a_nonce;
		endCommand = "DVBCAPEND_" + a_nonce;
		// Existing observed unknown-command framing, not a new engine command.
		// An unrecognised/localised framing fails closed before the payload runs.
		beginLine = "Script command \"" + beginCommand + "\" not found.";
		endLine = "Script command \"" + endCommand + "\" not found.";
	}

	WindowAdmission::Lease::~Lease()
	{
		if (!m_owner) return;
		std::lock_guard lock(m_owner->m_mutex);
		--m_owner->m_active;
		m_owner->m_changed.notify_all();
	}

	std::optional<WindowAdmission::Lease> WindowAdmission::Enter()
	{
		std::lock_guard lock(m_mutex);
		if (!m_accepting) return std::nullopt;
		++m_active;
		return Lease(this);
	}

	bool WindowAdmission::StopAndDrain(std::chrono::steady_clock::time_point a_deadline)
	{
		std::unique_lock lock(m_mutex);
		m_accepting = false;
		return m_changed.wait_until(lock, a_deadline, [&] { return m_active == 0; });
	}

	bool WindowAdmission::Accepting() const
	{
		std::lock_guard lock(m_mutex);
		return m_accepting;
	}

	bool WindowAdmission::Drained() const
	{
		std::lock_guard lock(m_mutex);
		return !m_accepting && m_active == 0;
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

	FenceState FindFence(const Fence& a_fence, std::string_view a_text, std::size_t a_fromOffset)
	{
		FenceState state;
		VisitLines(a_text, [&](std::size_t offset, std::string_view line) {
			if (offset < a_fromOffset || state.hasEnd) return;
			if (!state.hasBegin && line == a_fence.beginLine) state.hasBegin = true;
			else if (state.hasBegin && line == a_fence.endLine) state.hasEnd = true;
		});
		return state;
	}

	Slice SliceFencedText(const Fence& a_fence, std::string_view a_text, std::size_t a_maxLines, std::size_t a_fromOffset)
	{
		Slice out;
		std::deque<std::string> lines;
		std::size_t bytes = 0;
		VisitLines(a_text, [&](std::size_t offset, std::string_view line) {
			if (offset < a_fromOffset || out.sawEnd || line.empty()) return;
			if (line == a_fence.beginLine) out.sawBegin = true;
			else if (out.sawBegin && line == a_fence.endLine) out.sawEnd = true;
			else if (out.sawBegin) AppendBounded(lines, bytes, line, a_maxLines, out.lossPossible);
		});
		out.lines.assign(std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
		return out;
	}

	Slice SliceFencedLines(const Fence& a_fence, const std::deque<std::string>& a_lines, std::size_t a_maxLines)
	{
		Slice out;
		std::deque<std::string> lines;
		std::size_t bytes = 0;
		for (const auto& line : a_lines) {
			if (out.sawEnd) break;
			if (line == a_fence.beginLine) out.sawBegin = true;
			else if (out.sawBegin && line == a_fence.endLine) out.sawEnd = true;
			else if (out.sawBegin && !line.empty()) AppendBounded(lines, bytes, line, a_maxLines, out.lossPossible);
		}
		out.lines.assign(std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
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
		if (a_line == m_fence.beginLine && !m_sawBegin) {
			m_sawBegin = true;
			Record(a_line);
			return Seen::kBegin;
		}
		if (a_line == m_fence.endLine && m_sawBegin && !m_sawEnd) {
			m_sawEnd = true;
			Record(a_line);
			return Seen::kEnd;
		}
		if (a_line == m_lastSeen)
			return Seen::kNothing;
		if (a_line == m_fence.beginLine || a_line == m_fence.endLine)
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
			if (a_line != m_fence.beginLine)
				return;
			m_lines.emplace_back(a_line);
			m_sawBegin = true;
			return;
		} else if (a_line == m_fence.endLine) {
			m_lines.emplace_back(a_line);
			m_sawEnd = true;
			return;
		}
		if (a_line == m_fence.beginLine) return;  // duplicate control, not payload/restart
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
