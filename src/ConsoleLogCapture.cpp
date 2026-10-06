#include "ConsoleLogCapture.h"

#include "GameState.h"
#include "MainThread.h"
#include "PrologueScan.h"
#include "ToolRegistry.h"

#include <SKSE/ContextHook.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace dvb::ConsoleLogCapture
{
	namespace
	{
		using namespace std::chrono;
		using Clock = steady_clock;
		constexpr auto kLookInterval = milliseconds(8);
		constexpr auto kLookTimeout = milliseconds(2000);
		constexpr auto kCaptureDeadline = seconds(30);
		constexpr auto kDrainTimeout = milliseconds(2000);
		constexpr int kBeginLooks = 60, kCommandLooks = 25, kEndLooks = 60;

		enum class Source { kNone, kPrint, kBuffer, kSampler };

		struct Window
		{
			Window(std::uint64_t a_id, Fence a_fence, bool a_hooked) :
				id(a_id), fence(std::move(a_fence)), sampler(fence), printed(fence)
			{
				diagnostics.windowId = id;
				diagnostics.printHooked = a_hooked;
			}
			const std::uint64_t id;
			const Fence fence;
			WindowAdmission admission;
			std::mutex dataMutex;
			// Mutable only under dataMutex; queued closures own this window.
			LineSampler sampler;
			PrintCollector printed;
			std::size_t bufferBaseline = 0;
			int lastFrame = -1;
			std::size_t engineFrames = 0;
			std::atomic<Source> source{ Source::kNone };
			std::atomic<bool> timedOut{ true };
			std::atomic<bool> commandFault{ false };
			Result diagnostics;
			Slice bufferSlice;  // bounded fallback snapshot, never a live view
		};
		using WindowPtr = std::shared_ptr<Window>;
		std::mutex g_captureMutex;
		std::mutex g_windowsMutex;
		WindowPtr g_active;
		std::shared_ptr<const Result> g_frozen;
		std::uint64_t g_generation = 0;  // serialized by g_captureMutex
		std::atomic<bool> g_printHooked{ false };

		WindowPtr ActiveWindow()
		{
			std::lock_guard lock(g_windowsMutex);
			return g_active;
		}

		Fence NewFence()
		{
			// Resolve only from System32, without altering the build's link closure.
			// Failure has no PRNG/clock/counter fallback. The module is owned only
			// during this call; no borrowed function pointer escapes its lifetime.
			const auto module = ::LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!module) throw ToolError(500, "console capture system RNG is unavailable");
			struct ModuleHold {
				HMODULE module;
				~ModuleHold() { ::FreeLibrary(module); }
			} hold{module};
			const auto fill = reinterpret_cast<decltype(&BCryptGenRandom)>(::GetProcAddress(module, "BCryptGenRandom"));
			std::array<unsigned char, 16> bytes{};
			if (!fill || fill(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
				throw ToolError(500, "console capture system RNG failed");
			std::string nonce;
			for (const auto byte : bytes) nonce += std::format("{:02x}", byte);
			return Fence(std::move(nonce));
		}

		// ConsoleLog::VPrint(this, fmt, va_list) at entry: rdx = format, r8 = args.
		void PrintDetour(CONTEXT& a_ctx)
		{
			try {
				const auto window = ActiveWindow();
				if (!window || !window->diagnostics.printHooked) return;
				auto admitted = window->admission.Enter();
				if (!admitted) return;
				// Identity is retained before formatting. Close stops new
				// admissions and drains this lease before freezing its result.
				const auto* fmt = reinterpret_cast<const char*>(a_ctx.Rdx);
				const auto args = reinterpret_cast<std::va_list>(a_ctx.R8);
				std::lock_guard lock(window->dataMutex);
				const auto formatted = FormatPrint(fmt, args, window->printed.FormattingBudget());
				window->printed.RecordLoss(formatted.loss);
				if (formatted.loss == PrintLoss::kNone) {
					try { window->printed.Feed(formatted.text); }
					catch (...) { window->printed.RecordLoss(PrintLoss::kAllocation); }
				}
			} catch (...) {
				// Never let an observer exception escape into the engine.
			}
		}

		std::string CurrentLine()
		{
			auto* cl = RE::ConsoleLog::GetSingleton();
			return cl ? std::string(cl->lastMessage, ::strnlen(cl->lastMessage, sizeof(cl->lastMessage))) : std::string{};
		}
		std::string_view BufferText()
		{
			auto* cl = RE::ConsoleLog::GetSingleton();
			if (!cl) return {};
			const char* raw = cl->buffer.c_str();
			return raw ? std::string_view(raw) : std::string_view{};
		}
		std::size_t BufferFromOffset(const Window& a_window, std::size_t a_size)
		{
			return a_size < a_window.bufferBaseline ? 0 : a_window.bufferBaseline;
		}

		struct LookView
		{
			LineSampler::Seen seen = LineSampler::Seen::kNothing;
			bool printSawBegin = false, printSawEnd = false;
			bool samplerSawBegin = false, samplerSawEnd = false;
			bool bufferHasBegin = false, bufferHasEnd = false;
		};

		// Main thread, with an admitted lease and dataMutex held. Every fallback
		// field comes from this exact window's last completed observation.
		LookView LookAtSources(Window& a_window)
		{
			const int frame = game::CurrentFrame();
			if (frame != a_window.lastFrame) {
				++a_window.engineFrames;
				a_window.lastFrame = frame;
			}
			LookView view;
			view.printSawBegin = a_window.printed.SawBegin();
			view.printSawEnd = a_window.printed.SawEnd();
			const auto line = CurrentLine();
			view.seen = a_window.sampler.Observe(line);
			view.samplerSawBegin = a_window.sampler.SawBegin();
			view.samplerSawEnd = a_window.sampler.SawEnd();
			const auto buffer = BufferText();
			const auto from = BufferFromOffset(a_window, buffer.size());
			const auto fence = FindFence(a_window.fence, buffer, from);
			view.bufferHasBegin = fence.hasBegin;
			view.bufferHasEnd = fence.hasEnd;
			a_window.bufferSlice = SliceFencedText(a_window.fence, buffer, PrintCollector::kMaxLines, from);
			auto& d = a_window.diagnostics;
			d.consoleLogNull = RE::ConsoleLog::GetSingleton() == nullptr;
			d.lastMessage = line;
			d.lastMessageHasBegin = line == a_window.fence.beginLine;
			d.bufferEmpty = buffer.empty();
			d.bufferLen = buffer.size();
			d.bufferHasBegin = fence.hasBegin;
			if (auto* ui = RE::UI::GetSingleton()) {
				d.consoleMenuExists = ui->GetMenu(RE::Console::MENU_NAME).get() != nullptr;
				d.consoleMenuOpen = ui->IsMenuOpen(RE::Console::MENU_NAME);
			}
			d.consoleMode = RE::ConsoleLog::IsConsoleMode();
			return view;
		}

		void Queue(const WindowPtr& a_window, std::string a_command, Clock::time_point a_deadline,
			bool a_thenEnd = false)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) throw ToolError(500, "SKSE TaskInterface unavailable");
			task->AddTask([window = a_window, c = std::move(a_command), a_deadline, a_thenEnd] {
				try {
					if (Clock::now() >= a_deadline) return;
					auto admitted = window->admission.Enter();
					if (!admitted) return;  // late queued task belongs to a closed window
					RE::Console::ExecuteCommand(c.c_str());
					// A running command retains the old window and prevents successor
					// admission; do not dispatch an additional command after close.
					if (a_thenEnd && window->admission.Accepting())
						RE::Console::ExecuteCommand(window->fence.endCommand.c_str());
				} catch (...) {
					window->commandFault.store(true);
				}
			});
		}

		std::optional<LookView> Look(const WindowPtr& a_window)
		{
			std::this_thread::sleep_for(kLookInterval);
			try {
				// Return by value through RunAndWait's owned task. In particular
				// there is no reference to a listener's stack after a timeout.
				const auto result = MainThread::RunAndWait([window = a_window]() -> json {
					auto admitted = window->admission.Enter();
					if (!admitted) return nullptr;
					std::lock_guard lock(window->dataMutex);
					const auto v = LookAtSources(*window);
					return json{ {"seen", static_cast<int>(v.seen)}, {"pb", v.printSawBegin}, {"pe", v.printSawEnd},
						{"sb", v.samplerSawBegin}, {"se", v.samplerSawEnd}, {"bb", v.bufferHasBegin}, {"be", v.bufferHasEnd} };
				}, kLookTimeout);
				if (result.is_null()) return std::nullopt;
				return LookView{static_cast<LineSampler::Seen>(result.at("seen").get<int>()),
					result.at("pb").get<bool>(), result.at("pe").get<bool>(),
					result.at("sb").get<bool>(), result.at("se").get<bool>(),
					result.at("bb").get<bool>(), result.at("be").get<bool>()};
			} catch (const ToolError& e) {
				logs::warn("devbench: console capture stopped looking: {}", e.what());
				return std::nullopt;
			}
		}

		template <class Done>
		bool WaitFor(const WindowPtr& a_window, Clock::time_point a_deadline, int a_looks, Done a_done)
		{
			for (int i = 0; i < a_looks && Clock::now() < a_deadline; ++i) {
				if (a_window->commandFault.load()) return false;
				const auto view = Look(a_window);
				if (!view || Clock::now() >= a_deadline) return false;
				if (a_done(*view)) return true;
			}
			return false;
		}

		Source ChooseSource(const WindowPtr& a_window, Clock::time_point a_deadline)
		{
			Queue(a_window, a_window->fence.beginCommand, a_deadline);
			SourceChooser chooser;
			auto choice = SourceChooser::Choice::kUndecided;
			bool printed = false;
			WaitFor(a_window, a_deadline, kBeginLooks, [&](const LookView& v) {
				if (v.printSawBegin) { printed = true; return true; }
				choice = chooser.Look(v.bufferHasBegin, v.samplerSawBegin);
				return choice != SourceChooser::Choice::kUndecided;
			});
			if (printed) return Source::kPrint;
			if (choice == SourceChooser::Choice::kBuffer) return Source::kBuffer;
			if (choice == SourceChooser::Choice::kSampler) return Source::kSampler;
			return Source::kNone;
		}

		// Only after the admission gate is closed and drained. No new engine
		// observation occurs here; frozen data cannot be changed by a later task.
		bool Freeze(const WindowPtr& a_window)
		{
			if (!a_window->admission.Drained()) return false;
			Result out;
			{
				std::lock_guard lock(a_window->dataMutex);
				out = a_window->diagnostics;
				out.timedOut = a_window->timedOut.load();
				out.ringLines = a_window->sampler.Lines().size();
				out.samples = a_window->sampler.Samples();
				out.ticks = a_window->sampler.Ticks();
				out.engineFrames = a_window->engineFrames;
				out.printLines = a_window->printed.Lines().size();
				out.printDropped = a_window->printed.Dropped();
				out.printLoss = a_window->printed.Loss();
				out.printPayloadLines = a_window->printed.PayloadLines();
				out.printPayloadBytes = a_window->printed.PayloadBytes();
				Slice slice;
				switch (a_window->source.load()) {
				case Source::kPrint:
					slice = SliceFencedLines(a_window->fence, a_window->printed.Lines(), PrintCollector::kMaxLines);
					out.source = "print";
					out.lossPossible = out.printDropped > 0;
					break;
				case Source::kBuffer:
					slice = a_window->bufferSlice;
					out.source = "buffer";
					break;
				case Source::kSampler:
					slice = SliceFencedLines(a_window->fence, a_window->sampler.Lines(), PrintCollector::kMaxLines);
					out.source = "sampler";
					out.lossPossible = true;
					break;
				default: break;
				}
				out.sawBegin = slice.sawBegin;
				out.sawEnd = slice.sawEnd;
				out.lossPossible = out.lossPossible || slice.lossPossible;
				out.lines = std::move(slice.lines);
			}
			auto frozen = std::make_shared<const Result>(std::move(out));
			std::lock_guard lock(g_windowsMutex);
			if (g_active != a_window)
				return g_frozen && g_frozen->windowId == a_window->id;  // already frozen by a reader
			g_frozen = std::move(frozen);
			g_active.reset();
			return true;
		}

		bool Close(const WindowPtr& a_window)
		{
			return a_window->admission.StopAndDrain(Clock::now() + kDrainTimeout) && Freeze(a_window);
		}
	}

	void InstallPrintHook()
	{
		if (g_printHooked.load())
			return;
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(50180, 51110) };
		const auto*                     code = reinterpret_cast<const std::uint8_t*>(target.address());
		const std::size_t               length = SafePrologueLength(std::span<const std::uint8_t>(code, 32));
		if (length == 0 || length > 16) {
			std::string bytes;
			for (std::size_t i = 0; i < 16; ++i)
				bytes += std::format("{:02X} ", code[i]);
			logs::warn("devbench: console print hook not installed (ConsoleLog::VPrint starts {}); captures use the buffer or sampler", bytes);
			return;
		}
		// Negative include: the detour sees the registers as the caller left them, then the
		// copied prologue runs and execution resumes after it.
		if (!SKSE::stl::install_context_hook(target.address(), static_cast<int>(length), &PrintDetour, -static_cast<int>(length))) {
			logs::error("devbench: failed to install console print hook");
			return;
		}
		g_printHooked.store(true);
		logs::info("devbench: console print hook installed ({} byte prologue)", length);
	}

	bool IsCaptureControlCommand(std::string_view a_command)
	{
		const auto window = ActiveWindow();
		return window && (a_command == window->fence.beginCommand || a_command == window->fence.endCommand);
	}

	CaptureOutcome RunFencedCapture(const std::string& a_command)
	{
		std::unique_lock owned(g_captureMutex, std::try_to_lock);
		if (!owned.owns_lock())
			throw ToolError(409, "a console capture is already running");
		// After a drain timeout preserve the closed window until its admitted
		// tasks finish. A new capture is refused; never reset an active collector.
		if (const auto previous = ActiveWindow()) {
			if (!Freeze(previous))
				throw ToolError(409, "previous console capture is still active or draining; do not replay its command");
		}
		if (g_generation == std::numeric_limits<std::uint64_t>::max())
			throw ToolError(500, "console capture window generation exhausted");
		auto window = std::make_shared<Window>(++g_generation, NewFence(), g_printHooked.load());
		{
			std::lock_guard lock(g_windowsMutex);
			g_active = window;
		}
		try {
			MainThread::RunAndWait([window]() -> json {
				auto admitted = window->admission.Enter();
				if (!admitted) return nullptr;
				std::lock_guard lock(window->dataMutex);
				window->sampler.Reset(CurrentLine());
				window->bufferBaseline = BufferText().size();
				window->diagnostics.consoleLogNull = RE::ConsoleLog::GetSingleton() == nullptr;
				return true;
			}, kLookTimeout);
			const auto deadline = Clock::now() + kCaptureDeadline;
			const auto source = ChooseSource(window, deadline);
			window->source.store(source);
			if (source == Source::kNone)
				throw ToolError(504, "console capture never saw its exact begin frame; the payload command was not run");
			bool finished;
			if (source == Source::kSampler) {
				Queue(window, a_command, deadline);
				QuietDetector quiet;
				WaitFor(window, deadline, kCommandLooks, [&](const LookView& v) {
					return quiet.Look(v.seen == LineSampler::Seen::kLine);
				});
				Queue(window, window->fence.endCommand, deadline);
				finished = WaitFor(window, deadline, kEndLooks, [](const LookView& v) { return v.samplerSawEnd; });
			} else {
				Queue(window, a_command, deadline, true);
				finished = WaitFor(window, deadline, kEndLooks, [source](const LookView& v) {
					return source == Source::kPrint ? v.printSawEnd : v.bufferHasEnd;
				});
			}
			window->timedOut.store(!finished);
			if (window->commandFault.load())
				throw ToolError(504, "console capture task failed; command completion is uncertain, do not replay");
			if (!Close(window))
				throw ToolError(504, "console capture closed but admitted work is still draining; command completion is uncertain, do not replay");
			return { finished, window->id };
		} catch (...) {
			window->timedOut.store(true);
			// A bounded close failure leaves g_active retained and rejects a
			// successor. The original exception remains the primary result.
			try {
				if (window->admission.Accepting()) Close(window);
				else if (window->admission.Drained()) Freeze(window);
			} catch (...) {}  // preserve the primary exception, including allocation failure
			throw;
		}
	}

	Result ReadFenced(std::size_t a_maxLines, std::uint64_t a_windowId)
	{
		if (const auto window = ActiveWindow()) {
			if (!Freeze(window))
				throw ToolError(409, "console capture is active or draining; no immutable result is available");
		}
		std::shared_ptr<const Result> frozen;
		{
			std::lock_guard lock(g_windowsMutex);
			frozen = g_frozen;
		}
		Result out = frozen ? *frozen : Result{};
		if (a_windowId != 0 && out.windowId != a_windowId)
			throw ToolError(409, "console read windowId is not the retained closed window; no historical lookup is available");
		if (out.lines.size() > a_maxLines)
			out.lines.erase(out.lines.begin(), out.lines.end() - static_cast<std::ptrdiff_t>(a_maxLines));
		return out;
	}
}
