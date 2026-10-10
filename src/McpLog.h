#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <ostream>
#include <streambuf>
#include <string_view>
#include <utility>

namespace dvb
{
	enum class McpLogLevel { debug, info, warn, error, off };
	inline bool ParseMcpLogLevel(std::string_view text, McpLogLevel& level) noexcept
	{
		if (text == "debug") level = McpLogLevel::debug;
		else if (text == "info") level = McpLogLevel::info;
		else if (text == "warn") level = McpLogLevel::warn;
		else if (text == "error") level = McpLogLevel::error;
		else if (text == "off") level = McpLogLevel::off;
		else { level = McpLogLevel::warn; return false; }
		return true;
	}

	// Install one process-lifetime sink before transport startup. The callback
	// must not recursively use this logger. No lock is held while calling it.
	// Sink ownership remains external; atomics do not make owner destruction safe.
	class McpLog
	{
	public:
		using Sink = void (*)(McpLogLevel, std::string_view);
		static constexpr std::size_t MaxMessageBytes = 2048;
		static void SetSink(Sink sink) noexcept { sink_.store(sink, std::memory_order_release); }
		static void SetLevel(McpLogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
		static void NoteFailure() noexcept { ++failures_; }
		static std::uint64_t Failures() noexcept { return failures_.load(std::memory_order_relaxed); }
		template<class... Args>
		static void Write(McpLogLevel level, Args&&... args) noexcept
		{
			if (static_cast<unsigned>(level) >= static_cast<unsigned>(McpLogLevel::off) ||
				level < level_.load(std::memory_order_relaxed)) return;
			const auto sink = sink_.load(std::memory_order_acquire);
			if (!sink) { NoteFailure(); return; }
			try {
				Buffer buffer;
				std::ostream stream(&buffer);
				stream.exceptions(std::ios::badbit | std::ios::failbit);
				(stream << ... << std::forward<Args>(args));
				buffer.Sanitize();
				sink(level, buffer.View());
			} catch (...) { NoteFailure(); }  // no recursive log or request failure
		}
	private:
		class Buffer final : public std::streambuf
		{
		public:
			std::string_view View() const noexcept { return { bytes_.data(), used_ }; }
			void Sanitize() noexcept
			{
				// Printable single-line ASCII: peers cannot inject ANSI/control codes
				// or invalid UTF-8. Unicode peer metadata is deliberately lossy here.
				for (std::size_t at = 0; at < used_; ++at) {
					const auto c = static_cast<unsigned char>(bytes_[at]);
					if (c < 32 || c >= 127) bytes_[at] = '?';
					if (at + 36 <= used_ && IsUuid(at)) {
						for (std::size_t j = 0; j < 36; ++j) bytes_[at+j] = '*';
						at += 35;
					}
				}
				if (truncated_) {
					// Redact a handle prefix cut by the byte cap, not just full UUIDs.
					for (auto at = used_ - 36; at < used_; ++at) bytes_[at] = '*';
					constexpr std::string_view suffix = " [truncated]";
					for (std::size_t i = 0; i < suffix.size(); ++i) bytes_[used_-suffix.size()+i] = suffix[i];
				}
			}
		protected:
			std::streamsize xsputn(const char* data, std::streamsize count) override
			{
				if (count <= 0) return 0;
				const auto available = bytes_.size() - used_;
				const auto take = static_cast<std::uintmax_t>(count) > available ? available : static_cast<std::size_t>(count);
				for (std::size_t i = 0; i < take; ++i) bytes_[used_++] = data[i];
				if (static_cast<std::uintmax_t>(count) > available) truncated_ = true;
				return count;  // overflow is discarded, not a stream error
			}
			int_type overflow(int_type c) override
			{
				if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
				if (used_ < bytes_.size()) bytes_[used_++] = traits_type::to_char_type(c);
				else truncated_ = true;
				return c;
			}
		private:
			bool IsUuid(std::size_t at) const noexcept
			{
				for (std::size_t i = 0; i < 36; ++i) {
					const char c = bytes_[at+i];
					if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
					else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
				}
				return true;
			}
			std::array<char, MaxMessageBytes> bytes_{};
			std::size_t used_{ 0 };
			bool truncated_{ false };
		};
		inline static std::atomic<Sink> sink_{ nullptr };
		inline static std::atomic<McpLogLevel> level_{ McpLogLevel::warn };
		inline static std::atomic<std::uint64_t> failures_{ 0 };
	};
}
