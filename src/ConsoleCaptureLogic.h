#pragma once

#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dvb::ConsoleLogCapture
{
	// Nonce creation belongs to the engine-facing window. This pure framing seam
	// never accepts a substring or treats a second begin as a restart.
	struct Fence
	{
		explicit Fence(std::string a_nonce);
		std::string beginCommand, endCommand, beginLine, endLine;
	};

	// Shared by the actual print and main-thread paths. The owner must outlive
	// its leases (production closures/detours retain their shared window).
	class WindowAdmission
	{
	public:
		class Lease
		{
		public:
			Lease(Lease&& a_other) noexcept : m_owner(std::exchange(a_other.m_owner, nullptr)) {}
			~Lease();
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
		private:
			friend class WindowAdmission;
			explicit Lease(WindowAdmission* a_owner) : m_owner(a_owner) {}
			WindowAdmission* m_owner;
		};
		std::optional<Lease> Enter();
		bool StopAndDrain(std::chrono::steady_clock::time_point a_deadline);
		bool Accepting() const;
		bool Drained() const;
	private:
		mutable std::mutex m_mutex;
		std::condition_variable m_changed;
		bool m_accepting = true;
		std::size_t m_active = 0;
	};

	inline constexpr std::size_t kRingMax = 512;

	// Bound one formatted VPrint before allocating; the window payload has its
	// own independent byte limit. The two control records are separate capacity.
	inline constexpr std::size_t kMaxPrintBytes = 64 * 1024;
	inline constexpr std::size_t kMaxPayloadBytes = 1024 * 1024;

	enum class PrintLoss { kNone, kFormat, kAllocation, kOversize };
	struct FormattedPrint
	{
		std::string text;
		PrintLoss loss = PrintLoss::kNone;
	};
	// Production-used, bounded failure-injection seams. Every traversal gets its
	// own va_copy, even when the injected formatter throws. Defaults use libc and
	// std::string only; callers cannot ask for more than kMaxPrintBytes.
	using VFormatter = int (*)(char*, std::size_t, const char*, std::va_list);
	using PrintAllocator = void (*)(std::string&, std::size_t);
	FormattedPrint FormatPrint(const char* a_format, std::va_list a_args,
		std::size_t a_budget = kMaxPrintBytes,
		VFormatter a_formatter = &std::vsnprintf, PrintAllocator a_allocator = nullptr);

	struct PrintLossCounts
	{
		std::size_t lineLimit = 0;
		std::size_t byteLimit = 0;
		std::size_t format = 0;
		std::size_t allocation = 0;
		std::size_t oversize = 0;
		[[nodiscard]] std::size_t Total() const {
			return lineLimit + byteLimit + format + allocation + oversize;
		}
	};

	/// Looks the begin marker must survive in the buffer, or be seen in the sampler, before a
	/// source is chosen.
	inline constexpr int kStableLooks = 3;
	/// Looks without a new line after which a command's output counts as finished.
	inline constexpr int kQuietLooks = 3;

	struct Slice
	{
		bool                     sawBegin = false;
		bool                     sawEnd = false;
		bool                     lossPossible = false;
		std::vector<std::string> lines;
	};

	/// Whether complete lines hold this fence's first begin at/after a_fromOffset,
	/// and its first end after that begin. Duplicate begin lines never restart it.
	/// Markers before a_fromOffset belong to an earlier capture.
	struct FenceState
	{
		bool hasBegin = false;
		bool hasEnd = false;
	};
	FenceState FindFence(const Fence& a_fence, std::string_view a_text, std::size_t a_fromOffset = 0);

	/// The lines between the first exact begin (at or after a_fromOffset) and its first exact end,
	/// it, marker lines excluded, blank lines dropped, at most a_maxLines (the most recent).
	Slice SliceFencedText(const Fence& a_fence, std::string_view a_text, std::size_t a_maxLines, std::size_t a_fromOffset = 0);
	Slice SliceFencedLines(const Fence& a_fence, const std::deque<std::string>& a_lines, std::size_t a_maxLines);

	/// Builds a scrollback from repeated looks at one "most recent line" slot. A line replaced
	/// between two looks is never seen.
	class LineSampler
	{
	public:
		explicit LineSampler(Fence a_fence) : m_fence(std::move(a_fence)) {}
		enum class Seen
		{
			kNothing,
			kLine,
			kBegin,
			kEnd,
		};

		void Reset(std::string a_seedLine);

		Seen Observe(std::string_view a_line);

		[[nodiscard]] bool                           SawBegin() const { return m_sawBegin; }
		[[nodiscard]] bool                           SawEnd() const { return m_sawEnd; }
		[[nodiscard]] const std::deque<std::string>& Lines() const { return m_lines; }
		[[nodiscard]] std::size_t                    Samples() const { return m_samples; }
		[[nodiscard]] std::size_t                    Ticks() const { return m_ticks; }

	private:
		const Fence m_fence;
		void Record(std::string_view a_line);

		std::deque<std::string> m_lines;
		std::string             m_lastSeen;
		std::size_t             m_samples = 0;
		std::size_t             m_ticks = 0;
		bool                    m_sawBegin = false;
		bool                    m_sawEnd = false;
	};

	/// Picks the output source. The buffer is drained every frame once the Console menu exists, so it
	/// is chosen only if the begin marker is still in it kStableLooks looks in a row.
	class SourceChooser
	{
	public:
		enum class Choice
		{
			kUndecided,
			kBuffer,
			kSampler,
		};

		Choice Look(bool a_bufferHasBegin, bool a_samplerSawBegin);

	private:
		int m_bufferStreak = 0;
		int m_samplerLooks = -1;
	};

	/// Collects every line printed through ConsoleLog::VPrint during a capture: nothing before the
	/// begin marker, everything from it through the end marker, nothing after. A print may hold
	/// several lines. Not thread-safe; the caller locks.
	class PrintCollector
	{
	public:
		explicit PrintCollector(Fence a_fence) : m_fence(std::move(a_fence)) {}
		static constexpr std::size_t kMaxLines = 20000;

		void Reset();
		void Feed(std::string_view a_text);
		void RecordLoss(PrintLoss a_loss);

		[[nodiscard]] bool                           SawBegin() const { return m_sawBegin; }
		[[nodiscard]] bool                           SawEnd() const { return m_sawEnd; }
		[[nodiscard]] const std::deque<std::string>& Lines() const { return m_lines; }
		[[nodiscard]] std::size_t                    Dropped() const { return m_loss.Total(); }
		[[nodiscard]] const PrintLossCounts&         Loss() const { return m_loss; }
		[[nodiscard]] std::size_t                    PayloadLines() const { return m_payloadLines; }
		[[nodiscard]] std::size_t                    PayloadBytes() const { return m_payloadBytes; }
		[[nodiscard]] std::size_t                    FormattingBudget() const;

	private:
		const Fence m_fence;
		void Line(std::string_view a_line);

		std::deque<std::string> m_lines;
		PrintLossCounts         m_loss;
		std::size_t             m_payloadLines = 0;
		std::size_t             m_payloadBytes = 0;
		bool                    m_sawBegin = false;
		bool                    m_sawEnd = false;
	};

	/// Reports when a command has stopped printing: at least one new line seen, then
	/// kQuietLooks looks with none.
	class QuietDetector
	{
	public:
		bool Look(bool a_newLine);

	private:
		bool m_sawLine = false;
		int  m_quiet = 0;
	};
}
