#include "test_framework.h"

#include "ConsoleCaptureLogic.h"

#include <new>
#include <future>
#include <chrono>
#include <thread>
#include <stdexcept>

using dvb::ConsoleLogCapture::kRingMax;

namespace
{
	const dvb::ConsoleLogCapture::Fence kFence{ "0123456789abcdef0123456789abcdef" };
	// Fixture-only adapters keep the existing coverage on the same production
	// implementation. Production supplies a new OS-random fence per window.
	struct LineSampler : dvb::ConsoleLogCapture::LineSampler {
		LineSampler() : dvb::ConsoleLogCapture::LineSampler(kFence) {}
	};
	struct PrintCollector : dvb::ConsoleLogCapture::PrintCollector {
		PrintCollector() : dvb::ConsoleLogCapture::PrintCollector(kFence) {}
	};
	auto SliceFencedLines(const std::deque<std::string>& a_lines, std::size_t a_maxLines) {
		return dvb::ConsoleLogCapture::SliceFencedLines(kFence, a_lines, a_maxLines);
	}
	auto SliceFencedText(std::string_view a_text, std::size_t a_maxLines, std::size_t a_offset = 0) {
		return dvb::ConsoleLogCapture::SliceFencedText(kFence, a_text, a_maxLines, a_offset);
	}
	auto FindFence(std::string_view a_text, std::size_t a_offset = 0) {
		return dvb::ConsoleLogCapture::FindFence(kFence, a_text, a_offset);
	}
	std::string BeginLine()
	{
		return kFence.beginLine;
	}

	std::string EndLine()
	{
		return kFence.endLine;
	}
}

TEST_CASE("text slicing returns the lines between the fence markers")
{
	const std::string text = "older output\n" + BeginLine() + "\nline one\nline two\n" + EndLine() + "\nlater\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "line one");
	CHECK(slice.lines[1] == "line two");
}

TEST_CASE("text slicing handles CRLF and drops blank lines")
{
	const std::string text = BeginLine() + "\r\nalpha\r\n\r\nbeta\r\n" + EndLine() + "\r\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "alpha");
	CHECK(slice.lines[1] == "beta");
}

TEST_CASE("duplicate begin does not restart an already completed exact window")
{
	const std::string text = BeginLine() + "\nold\n" + EndLine() + "\n" + BeginLine() + "\nnew\n" + EndLine() + "\n";
	const auto        slice = SliceFencedText(text, 200);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "old");
}

TEST_CASE("text slicing reports a missing end marker and a missing begin marker")
{
	const auto noEnd = SliceFencedText(BeginLine() + "\npartial\n", 200);
	CHECK(noEnd.sawBegin);
	CHECK(!noEnd.sawEnd);
	CHECK(noEnd.lines.size() == 1);

	const auto noBegin = SliceFencedText("just output\n" + EndLine() + "\n", 200);
	CHECK(!noBegin.sawBegin);
	CHECK(noBegin.lines.empty());

	CHECK(!SliceFencedText("", 200).sawBegin);
}

TEST_CASE("slicing keeps only the most recent lines when capped")
{
	std::string text = BeginLine() + "\n";
	for (int i = 0; i < 10; ++i)
		text += "row " + std::to_string(i) + "\n";
	text += EndLine() + "\n";
	const auto slice = SliceFencedText(text, 3);
	CHECK(slice.lines.size() == 3);
	CHECK(slice.lines.front() == "row 7");
	CHECK(slice.lines.back() == "row 9");
}

TEST_CASE("line slicing matches text slicing on the same fenced window")
{
	std::deque<std::string> lines{ "stale", BeginLine(), "one", "two", EndLine(), "after" };
	const auto              slice = SliceFencedLines(lines, 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 2);
	CHECK(slice.lines[0] == "one");

	std::deque<std::string> unfinished{ BeginLine(), "one" };
	const auto              partial = SliceFencedLines(unfinished, 200);
	CHECK(partial.sawBegin);
	CHECK(!partial.sawEnd);
	CHECK(SliceFencedLines({}, 200).lines.empty());
}

TEST_CASE("sampler records changes and ignores an unchanged line")
{
	LineSampler sampler;
	sampler.Reset("stale message");
	CHECK(sampler.Observe("stale message") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe("first") == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe("second") == LineSampler::Seen::kLine);
	CHECK(sampler.Samples() == 2);
	CHECK(sampler.Ticks() == 5);
}

TEST_CASE("sampler reports each marker once and slices a complete capture")
{
	LineSampler sampler;
	sampler.Reset("");
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("GetActorValue: Health >> 100.00") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());

	const auto slice = SliceFencedLines(sampler.Lines(), 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "GetActorValue: Health >> 100.00");
}

TEST_CASE("a stale begin marker left by an aborted capture cannot swallow the next one")
{
	LineSampler sampler;
	sampler.Reset(BeginLine());  // seeded with the marker an aborted capture left showing
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.SawBegin());
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(SliceFencedLines(sampler.Lines(), 200).lines.size() == 1);
}

TEST_CASE("the previous capture's end marker is not taken as this capture's end")
{
	LineSampler sampler;
	sampler.Reset(EndLine());  // the last capture's end marker is still showing
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kNothing);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.Observe("output") == LineSampler::Seen::kLine);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	CHECK(sampler.SawEnd());
	CHECK(SliceFencedLines(sampler.Lines(), 200).lines.size() == 1);
}

TEST_CASE("a command that prints nothing still ends on the end marker")
{
	LineSampler sampler;
	sampler.Reset("");
	sampler.Observe(BeginLine());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
	const auto slice = SliceFencedLines(sampler.Lines(), 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.empty());
}

TEST_CASE("reset clears the previous capture")
{
	LineSampler sampler;
	sampler.Reset("");
	sampler.Observe(BeginLine());
	sampler.Observe("x");
	sampler.Observe(EndLine());
	sampler.Reset("seed");
	CHECK(!sampler.SawBegin());
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Lines().empty());
	CHECK(sampler.Samples() == 0);
	CHECK(sampler.Ticks() == 0);
}

TEST_CASE("the sampler ring is bounded")
{
	LineSampler sampler;
	sampler.Reset("");
	for (std::size_t i = 0; i < kRingMax + 40; ++i)
		sampler.Observe("line " + std::to_string(i));
	CHECK(sampler.Lines().size() == kRingMax);
	CHECK(sampler.Lines().back() == "line " + std::to_string(kRingMax + 39));
}

using dvb::ConsoleLogCapture::kQuietLooks;
using dvb::ConsoleLogCapture::kStableLooks;
using dvb::ConsoleLogCapture::QuietDetector;
using dvb::ConsoleLogCapture::SourceChooser;

TEST_CASE("the buffer is chosen once it holds the begin marker for several looks in a row")
{
	SourceChooser chooser;
	for (int i = 1; i < kStableLooks; ++i)
		CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kBuffer);
}

TEST_CASE("a marker that vanishes from the buffer falls back to the sampler")
{
	// The Console menu drains the buffer each frame: the marker shows once, then is gone.
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	SourceChooser::Choice choice = SourceChooser::Choice::kUndecided;
	for (int i = 0; i < kStableLooks && choice == SourceChooser::Choice::kUndecided; ++i)
		choice = chooser.Look(false, true);
	CHECK(choice == SourceChooser::Choice::kSampler);
}

TEST_CASE("a buffer marker that flickers never reaches a stable streak")
{
	SourceChooser chooser;
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(false, true) == SourceChooser::Choice::kUndecided);
	CHECK(chooser.Look(true, true) == SourceChooser::Choice::kSampler);
}

TEST_CASE("nothing is chosen until a begin marker has been seen")
{
	SourceChooser chooser;
	for (int i = 0; i < 20; ++i)
		CHECK(chooser.Look(false, false) == SourceChooser::Choice::kUndecided);
}

TEST_CASE("a command counts as finished only after output and then quiet")
{
	QuietDetector detector;
	for (int i = 0; i < 10; ++i)
		CHECK(!detector.Look(false));  // a command that has not printed yet is never finished
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}

TEST_CASE("new output restarts the quiet count")
{
	QuietDetector detector;
	detector.Look(true);
	detector.Look(false);
	detector.Look(false);
	CHECK(!detector.Look(true));
	for (int i = 1; i < kQuietLooks; ++i)
		CHECK(!detector.Look(false));
	CHECK(detector.Look(false));
}


TEST_CASE("a fence before the starting offset belongs to an earlier capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	const auto        state = FindFence(previous, previous.size());
	CHECK(!state.hasBegin);
	CHECK(!state.hasEnd);
	CHECK(!SliceFencedText(previous, 200, previous.size()).sawBegin);
}

TEST_CASE("a stale end marker cannot complete a new capture")
{
	const std::string previous = BeginLine() + "\nold output\n" + EndLine() + "\n";
	std::string       text = previous + BeginLine() + "\n";
	const auto        started = FindFence(text, previous.size());
	CHECK(started.hasBegin);
	CHECK(!started.hasEnd);

	text += "new output\n" + EndLine() + "\n";
	const auto finished = FindFence(text, previous.size());
	CHECK(finished.hasBegin);
	CHECK(finished.hasEnd);
	const auto slice = SliceFencedText(text, 200, previous.size());
	CHECK(slice.lines.size() == 1);
	CHECK(slice.lines[0] == "new output");
}

TEST_CASE("fence detection with no offset selects the first exact complete window")
{
	const std::string text = BeginLine() + "\na\n" + EndLine() + "\n" + BeginLine() + "\n";
	const auto        state = FindFence(text);
	CHECK(state.hasBegin);
	CHECK(state.hasEnd);
	CHECK(!FindFence("no markers here").hasBegin);
}

TEST_CASE("print collector keeps every line of a multi-line command between the markers")
{
	PrintCollector c;
	c.Feed("noise before the capture");
	c.Feed(BeginLine());
	c.Feed("00000014 (2 lights)");
	c.Feed("> skeleton_female.nif\n> NPC Root [Root]\r\n> MagicRight\n");
	c.Feed("> LP_Light[Let There Be Glow|MagicLightWhite01](1)#0 (radius: 246.9|fade: 0.97|visible)");
	c.Feed(EndLine());
	c.Feed("noise after the capture");
	CHECK(c.SawBegin());
	CHECK(c.SawEnd());
	const auto slice = SliceFencedLines(c.Lines(), 200);
	CHECK(slice.sawBegin);
	CHECK(slice.sawEnd);
	CHECK(slice.lines.size() == 5);
	CHECK(slice.lines[0] == "00000014 (2 lights)");
	CHECK(slice.lines[2] == "> NPC Root [Root]");
	CHECK(slice.lines[4].starts_with("> LP_Light["));
}

TEST_CASE("print collector ignores an end marker before the begin marker")
{
	PrintCollector c;
	c.Feed(EndLine());
	CHECK(!c.SawEnd());
	c.Feed(BeginLine());
	c.Feed("only line");
	CHECK(c.SawBegin());
	CHECK(!c.SawEnd());
	CHECK(SliceFencedLines(c.Lines(), 200).lines.size() == 1);
}

TEST_CASE("print collector caps its lines, counts the drop, and still sees the end marker")
{
	PrintCollector c;
	c.Feed(BeginLine());
	for (std::size_t i = 0; i < PrintCollector::kMaxLines + 10; ++i)
		c.Feed("x");
	c.Feed(EndLine());
	CHECK(c.SawEnd());
	CHECK(c.Dropped() == 10);
	c.Reset();
	CHECK(!c.SawBegin());
	CHECK(c.Lines().empty());
	CHECK(c.Dropped() == 0);
}

namespace
{
	using namespace dvb::ConsoleLogCapture;

	FormattedPrint FormatWith(std::size_t a_budget, VFormatter a_formatter,
		PrintAllocator a_allocator, const char* a_format, ...)
	{
		std::va_list args;
		va_start(args, a_format);
		try {
			auto result = FormatPrint(a_format, args, a_budget, a_formatter, a_allocator);
			va_end(args);
			return result;
		} catch (...) {
			va_end(args);
			throw;
		}
	}

	int g_formatCalls = 0;
	int g_failOnCall = 0;
	bool g_wrongLength = false;
	int InjectFormat(char* a_dst, std::size_t a_size, const char* a_format, std::va_list a_args)
	{
		++g_formatCalls;
		if (g_formatCalls == g_failOnCall) return -1;
		const int n = std::vsnprintf(a_dst, a_size, a_format, a_args);
		return g_wrongLength && g_formatCalls == 2 ? n + 1 : n;
	}
	void RefuseAllocation(std::string&, std::size_t) { throw std::bad_alloc(); }
}

TEST_CASE("print formatting independently traverses long mixed varargs twice")
{
	const std::string longText(2048, 'x');
	g_formatCalls = 0; g_failOnCall = 0; g_wrongLength = false;
	const auto result = FormatWith(kMaxPrintBytes, InjectFormat, nullptr,
		"%d:%s:%d", 17, longText.c_str(), 29);
	CHECK(result.loss == PrintLoss::kNone);
	CHECK(result.text == "17:" + longText + ":29");
	CHECK(g_formatCalls == 2);
}

TEST_CASE("first and second print format failures and inconsistent lengths are explicit loss")
{
	const std::string longText(2048, 'x');
	for (int failed : {1, 2}) {
		g_formatCalls = 0; g_failOnCall = failed; g_wrongLength = false;
		const auto result = FormatWith(kMaxPrintBytes, InjectFormat, nullptr, "%s", longText.c_str());
		CHECK(result.loss == PrintLoss::kFormat);
		CHECK(result.text.empty());
	}
	g_formatCalls = 0; g_failOnCall = 0; g_wrongLength = true;
	const auto wrong = FormatWith(kMaxPrintBytes, InjectFormat, nullptr, "%s", longText.c_str());
	CHECK(wrong.loss == PrintLoss::kFormat);
	CHECK(wrong.text.empty());
	g_wrongLength = false;
}

TEST_CASE("print allocation refusal is bounded observable loss on both format paths")
{
	for (std::size_t size : {10u, 2048u}) {
		const std::string text(size, 'x');
		const auto result = FormatWith(kMaxPrintBytes, &std::vsnprintf, RefuseAllocation, "%s", text.c_str());
		CHECK(result.loss == PrintLoss::kAllocation);
		CHECK(result.text.empty());
	}
}

TEST_CASE("print preallocation budget rejects oversize and clamps caller limits")
{
	const std::string exact(kMaxPrintBytes, 'x');
	const auto accepted = FormatWith(kMaxPrintBytes, &std::vsnprintf, nullptr, "%s", exact.c_str());
	CHECK(accepted.loss == PrintLoss::kNone);
	CHECK(accepted.text.size() == kMaxPrintBytes);
	const auto over = FormatWith(kMaxPrintBytes * 2, &std::vsnprintf, nullptr, "%s!", exact.c_str());
	CHECK(over.loss == PrintLoss::kOversize);
	CHECK(over.text.empty());
	const auto remaining = FormatWith(8, &std::vsnprintf, nullptr, "%s", "ninebytes");
	CHECK(remaining.loss == PrintLoss::kOversize);
	CHECK(remaining.text.empty());
}

TEST_CASE("fences do not consume 19999 20000 or 20001 payload line capacity")
{
	for (std::size_t count : {19999u, 20000u, 20001u}) {
		PrintCollector c;
		c.Feed(BeginLine());
		for (std::size_t i = 0; i < count; ++i) c.Feed("x");
		c.Feed(EndLine());
		CHECK(c.SawEnd());
		CHECK(c.PayloadLines() == (count > PrintCollector::kMaxLines ? PrintCollector::kMaxLines : count));
		CHECK(c.Lines().size() == c.PayloadLines() + 2);
		CHECK(c.Loss().lineLimit == (count == 20001 ? 1u : 0u));
		CHECK(c.Dropped() == c.Loss().lineLimit);
	}
}

TEST_CASE("payload byte budget refuses extra data but preserves end control and loss counters")
{
	PrintCollector c;
	c.Feed(BeginLine());
	const std::string block(kMaxPrintBytes, 'x');
	for (std::size_t i = 0; i < kMaxPayloadBytes / kMaxPrintBytes; ++i) c.Feed(block);
	CHECK(c.PayloadBytes() == kMaxPayloadBytes);
	CHECK(c.FormattingBudget() == 256);
	c.Feed("x");
	CHECK(c.Loss().byteLimit == 1);
	c.RecordLoss(PrintLoss::kFormat);
	c.RecordLoss(PrintLoss::kAllocation);
	c.RecordLoss(PrintLoss::kOversize);
	c.Feed(EndLine());
	CHECK(c.SawEnd());
	CHECK(c.Loss().format == 1);
	CHECK(c.Loss().allocation == 1);
	CHECK(c.Loss().oversize == 1);
	CHECK(c.Dropped() == 4);
	c.Reset();
	CHECK(c.PayloadLines() == 0);
	CHECK(c.PayloadBytes() == 0);
	CHECK(c.Dropped() == 0);
}

TEST_CASE("exact nonce fencing rejects token-like payload old windows and combined controls")
{
	const dvb::ConsoleLogCapture::Fence old{ "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
	const std::string combined = BeginLine() + " " + EndLine();
	const std::string prefixed = "payload: " + BeginLine();
	const std::string suffix = EndLine() + " payload";
	const std::string text = old.beginLine + "\r\n" + old.endLine + "\r\n" +
		combined + "\r\n" + prefixed + "\r\n" + BeginLine() + "\r\n" +
		prefixed + "\r\n" + BeginLine() + "\r\n" + combined + "\r\n" + suffix +
		"\r\n" + EndLine() + "\r\n" + EndLine() + "\r\n";
	const auto state = FindFence(text);
	CHECK(state.hasBegin && state.hasEnd);
	const auto sliced = SliceFencedText(text, 200);
	CHECK(sliced.lines == std::vector<std::string>({prefixed, combined, suffix}));
	PrintCollector collector;
	collector.Feed(old.beginLine);
	collector.Feed(combined);
	collector.Feed(prefixed);
	CHECK(!collector.SawBegin());
	collector.Feed(BeginLine());
	collector.Feed(BeginLine());
	collector.Feed(combined);
	collector.Feed(suffix);
	CHECK(!collector.SawEnd());
	collector.Feed(EndLine());
	collector.Feed(EndLine());
	CHECK(collector.PayloadLines() == 2);
	CHECK(SliceFencedLines(collector.Lines(), 200).lines == std::vector<std::string>({combined, suffix}));
	LineSampler sampler;
	sampler.Reset(old.beginLine);
	CHECK(sampler.Observe(old.beginLine) == LineSampler::Seen::kNothing);
	CHECK(sampler.Observe(combined) == LineSampler::Seen::kLine);
	CHECK(!sampler.SawBegin());
	CHECK(sampler.Observe(BeginLine()) == LineSampler::Seen::kBegin);
	CHECK(sampler.Observe(prefixed) == LineSampler::Seen::kLine);
	CHECK(!sampler.SawEnd());
	CHECK(sampler.Observe(EndLine()) == LineSampler::Seen::kEnd);
}

TEST_CASE("framing validates nonce and offsets never turn a line suffix into a control")
{
	CHECK_THROWS(dvb::ConsoleLogCapture::Fence("short"));
	CHECK_THROWS(dvb::ConsoleLogCapture::Fence("0123456789abcdef0123456789abcdeG"));
	CHECK_THROWS(dvb::ConsoleLogCapture::Fence("0123456789abcdef0123456789abcdef0"));
	const std::string text = "payload:" + BeginLine() + "\n" + EndLine();
	CHECK(!FindFence(text, 8).hasBegin);
	CHECK(!SliceFencedText(text, 200, 8).sawBegin);
}

TEST_CASE("production admission gate drains an admitted writer before immutable read and reopen")
{
	using dvb::ConsoleLogCapture::WindowAdmission;
	using namespace std::chrono;
	WindowAdmission gate;
	PrintCollector first;
	first.Feed(BeginLine());
	std::promise<void> entered, resume;
	auto enteredFuture = entered.get_future();
	auto resumeFuture = resume.get_future();
	auto writer = std::async(std::launch::async, [&] {
		auto admitted = gate.Enter();
		if (!admitted) { entered.set_value(); return; }
		entered.set_value();
		if (resumeFuture.wait_for(seconds(1)) != std::future_status::ready) return;
		// Publication retains the same gate/collector, as PrintDetour does.
		first.Feed("admitted output");
		first.Feed(EndLine());
	});
	const auto ready = enteredFuture.wait_for(seconds(1));
	CHECK(ready == std::future_status::ready);
	CHECK(!gate.StopAndDrain(steady_clock::now()));
	CHECK(!gate.Accepting());
	CHECK(!gate.Drained());  // production Freeze refuses read/publication here
	CHECK(!gate.Enter().has_value());
	resume.set_value();
	CHECK(writer.wait_for(seconds(1)) == std::future_status::ready);
	writer.get();
	CHECK(gate.StopAndDrain(steady_clock::now() + seconds(1)));
	CHECK(gate.Drained());
	const auto frozen = SliceFencedLines(first.Lines(), 200);
	CHECK(frozen.sawEnd);
	CHECK(frozen.lines == std::vector<std::string>({"admitted output"}));
	WindowAdmission successorGate;
	const dvb::ConsoleLogCapture::Fence next{ "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" };
	dvb::ConsoleLogCapture::PrintCollector second{next};
	auto admitted = successorGate.Enter();
	CHECK(admitted.has_value());
	CHECK(!gate.Enter().has_value());  // stale admitted task cannot re-enter
	second.Feed(BeginLine());
	second.Feed(EndLine());
	CHECK(!second.SawBegin());
	second.Feed(next.beginLine);
	second.Feed("successor output");
	second.Feed(next.endLine);
	CHECK(dvb::ConsoleLogCapture::SliceFencedLines(next, second.Lines(), 200).lines ==
		std::vector<std::string>({"successor output"}));
	CHECK(frozen.lines == std::vector<std::string>({"admitted output"}));
}

TEST_CASE("production admission lease is exception safe and queued late work cannot enter")
{
	using dvb::ConsoleLogCapture::WindowAdmission;
	using namespace std::chrono;
	WindowAdmission gate;
	try {
		auto entered = gate.Enter();
		CHECK(entered.has_value());
		throw std::runtime_error("injected observer failure");
	} catch (const std::runtime_error&) {}
	CHECK(gate.StopAndDrain(steady_clock::now() + seconds(1)));
	CHECK(!gate.Enter().has_value());
	CHECK(gate.Drained());
}

TEST_CASE("fallback slicing bounds payload bytes and reports omitted data without losing the end")
{
	using namespace dvb::ConsoleLogCapture;
	const std::string block(kMaxPrintBytes, 'x');
	std::string text = BeginLine() + "\n";
	for (std::size_t i = 0; i < kMaxPayloadBytes / kMaxPrintBytes + 1; ++i) text += block + "\n";
	text += std::string(kMaxPayloadBytes + 1, 'y') + "\n" + EndLine() + "\n";
	const auto out = ::SliceFencedText(text, 20000);
	CHECK(out.sawBegin && out.sawEnd && out.lossPossible);
	std::size_t bytes = 0;
	for (const auto& line : out.lines) bytes += line.size();
	CHECK(bytes <= kMaxPayloadBytes);
}
