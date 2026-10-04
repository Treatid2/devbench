#include "test_framework.h"

#include "CalendarAdmission.h"
#include "CalendarLease.h"
#include "CalendarProtocol.h"
#include "MainThreadTask.h"

#include <limits>

using namespace dvb::CalendarControl;

namespace
{
	struct FakeBackend final : Backend
	{
		Snapshot snapshot;
		int writes = 0, writeAttempts = 0;
		bool failWrite = false, hideAfterWrite = false, nextReadUnavailable = false;

		FakeBackend()
		{
			snapshot.source = { "process:created", 1, 42, { 1, 2, 3, 4, 5, 6 }, { 11, 12, 13, 14, 15, 16 }, 100 };
			snapshot.available = snapshot.worldLoaded = true;
			snapshot.date = { 201, 7, 17, 7.212f, 12.3f };
			snapshot.rate = 20;
			snapshot.engineMultiplier = 1;
			snapshot.frame = 55;
		}
		Snapshot Read() override
		{
			auto result = snapshot;
			if (nextReadUnavailable) {
				result.available = false;
				nextReadUnavailable = false;
			}
			return result;
		}
		bool WriteRate(const Snapshot& a_expected, float a_rate) override
		{
			++writeAttempts;
			if (failWrite || !snapshot.source.SameStorage(a_expected.source) || snapshot.rate != a_expected.rate)
				return false;
			++writes;
			snapshot.rate = a_rate;
			nextReadUnavailable = hideAfterWrite;
			return true;
		}
	};

	struct Fixture
	{
		FakeBackend backend;
		Controller controller{ backend };
		Outcome Hold(std::int64_t duration = 100)
		{
			return controller.Hold(backend.snapshot.source, "mapping", "mcp:session", "hold-command", 10, duration, 1000);
		}
		Outcome Release(const std::string& owner = "mapping", const std::string& connection = "mcp:session")
		{
			return controller.Release(backend.snapshot.source, owner, connection, "1");
		}
	};
}

TEST_CASE("calendar hold and exact release change rate only")
{
	Fixture f;
	const auto before = f.backend.Read();
	CHECK(f.Hold().ok);
	CHECK(f.backend.snapshot.rate == 0);
	CHECK(f.controller.Current()->baseline.source == before.source);
	CHECK(f.controller.Current()->deadline == 110);
	CHECK(f.Release().restored);
	CHECK(f.backend.snapshot.rate == 20);
	CHECK(f.backend.snapshot.date == before.date);
	CHECK(f.backend.snapshot.engineMultiplier == before.engineMultiplier);
	CHECK(f.Release().restored);
	CHECK(f.backend.writes == 2);
}

TEST_CASE("calendar expiry is monotonic even when frame does not change")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.controller.Tick(109);
	CHECK(f.backend.snapshot.rate == 0);
	CHECK(f.controller.NeedsPump());
	CHECK(f.controller.Tick(110).restored);
	CHECK(f.controller.Result().code == "expired");
	CHECK(f.backend.snapshot.frame == 55);
	CHECK(!f.controller.NeedsPump());
	f.controller.Tick(2000);
	CHECK(f.backend.writes == 2);
}

TEST_CASE("calendar owner connection lease and binding mismatches cannot release")
{
	Fixture f;
	CHECK(f.Hold().ok);
	CHECK(!f.Release("other").ok);
	CHECK(!f.Release("mapping", "mcp:other").ok);
	CHECK(!f.controller.Release(f.backend.snapshot.source, "mapping", "mcp:session", "2").ok);
	auto wrong = f.backend.snapshot.source;
	++wrong.generation;
	CHECK(!f.controller.Release(wrong, "mapping", "mcp:session", "1").ok);
	CHECK(!f.Hold().ok);
	CHECK(f.backend.snapshot.rate == 0);
	CHECK(f.backend.writes == 1);
}

TEST_CASE("calendar refuses unsupported state duration stale identity and expired apply")
{
	Fixture f;
	CHECK(!f.Hold(0).ok);
	CHECK(!f.Hold(kMaximumHoldMs + 1).ok);
	CHECK(!f.controller.Hold(f.backend.snapshot.source, "mapping", "rest", "command", 1000, 100, 1000).ok);
	CHECK(!f.controller.Hold(f.backend.snapshot.source, "mapping", "rest", "command",
		std::numeric_limits<std::int64_t>::max() - 1, 100, std::numeric_limits<std::int64_t>::max()).ok);
	for (const float rate : { 0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
		f.backend.snapshot.rate = rate;
		CHECK(!f.Hold().ok);
	}
	f.backend.snapshot.rate = 20;
	f.backend.snapshot.worldLoaded = false;
	CHECK(!f.Hold().ok);
	f.backend.snapshot.worldLoaded = true;
	auto old = f.backend.snapshot.source;
	++f.backend.snapshot.source.generation;
	CHECK(!f.controller.Hold(old, "mapping", "rest", "command", 10, 100, 1000).ok);
	CHECK(f.backend.writes == 0);
}

TEST_CASE("calendar external rate writer is never overwritten")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.snapshot.rate = 5;
	CHECK(!f.controller.Tick(110).restored);
	CHECK(f.controller.Result().code == "external_rate_write_without_restore");
	CHECK(f.backend.snapshot.rate == 5);
	CHECK(f.backend.writes == 1);
}

TEST_CASE("calendar external date change cancels hold without rewinding")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.snapshot.date[3] = 19.647f;
	CHECK(f.controller.Tick(11).restored);
	CHECK(f.controller.Result().code == "external_calendar_change");
	CHECK(f.backend.snapshot.date[3] == 19.647f);
	CHECK(f.backend.snapshot.rate == 20);
}

TEST_CASE("calendar engine multiplier change ends hold but never changes that multiplier")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.snapshot.engineMultiplier = 0.5;
	CHECK(f.controller.Tick(11).restored);
	CHECK(f.controller.Result().code == "engine_multiplier_changed");
	CHECK(f.backend.snapshot.engineMultiplier == 0.5);
}

TEST_CASE("calendar storage process and load changes invalidate without stale restore")
{
	for (int changed = 0; changed < 4; ++changed) {
		Fixture f;
		CHECK(f.Hold().ok);
		if (changed == 0) ++f.backend.snapshot.source.generation;
		if (changed == 1) ++f.backend.snapshot.source.storage[5];
		if (changed == 2) ++f.backend.snapshot.source.forms[5];
		if (changed == 3) f.backend.snapshot.source.processSession = "new-process";
		f.backend.snapshot.rate = 30;
		CHECK(!f.controller.Tick(110).restored);
		CHECK(!f.controller.Current());
		CHECK(f.backend.snapshot.rate == 30);
		CHECK(f.backend.writes == 1);
	}
}

TEST_CASE("calendar scene loss restores only still-identified same-generation globals")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.snapshot.worldLoaded = false;
	CHECK(f.controller.Tick(11).restored);
	CHECK(f.controller.Result().code == "scene_lost");
	CHECK(f.backend.snapshot.rate == 20);
	Fixture other;
	CHECK(other.Hold().ok);
	other.backend.snapshot.available = false;
	CHECK(!other.controller.Tick(11).restored);
	CHECK(other.backend.writes == 1);
}

TEST_CASE("calendar lifecycle cleanup does not restore into a later generation")
{
	Fixture f;
	CHECK(f.Hold().ok);
	CHECK(f.controller.End("pre_load", true).restored);
	CHECK(f.backend.snapshot.rate == 20);
	CHECK(f.Hold().ok);
	++f.backend.snapshot.source.generation;
	f.backend.snapshot.rate = 7;
	CHECK(!f.controller.End("new_load_generation", false).restored);
	CHECK(f.backend.snapshot.rate == 7);
	CHECK(!f.controller.NeedsPump());
}

TEST_CASE("calendar unverified apply stays tracked only after a committed zero write")
{
	Fixture f;
	f.backend.failWrite = true;
	CHECK(!f.Hold().ok);
	CHECK(!f.controller.Current());
	CHECK(f.backend.snapshot.rate == 20);
	f.backend.failWrite = false;
	f.backend.hideAfterWrite = true;
	CHECK(!f.Hold().ok);
	CHECK(f.controller.Current().has_value());
	CHECK(f.backend.snapshot.rate == 0);
	f.backend.hideAfterWrite = false;
	CHECK(f.controller.Tick(110).restored);
	CHECK(f.backend.snapshot.rate == 20);
}

TEST_CASE("calendar failed restore requires explicit release not an automatic retry loop")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.failWrite = true;
	CHECK(!f.controller.Tick(110).restored);
	CHECK(f.controller.Current()->cleanupAttempted);
	CHECK(!f.controller.NeedsPump());
	f.backend.failWrite = false;
	f.controller.Tick(1000);
	CHECK(f.backend.snapshot.rate == 0);
	CHECK(f.Release().restored);
	CHECK(f.backend.snapshot.rate == 20);
}

TEST_CASE("calendar failed lifecycle cleanup retains exact explicit retry custody")
{
	for (const auto* reason : { "unsupported_save_event", "loading_menu", "service_stop", "pre_load" }) {
		Fixture f;
		CHECK(f.Hold().ok);
		const auto owned = *f.controller.Current();
		f.backend.failWrite = true;
		CHECK(!f.controller.End(reason, true).restored);
		CHECK(f.controller.Result().code == "restore_failed_requires_explicit_release");
		CHECK(f.controller.Current()->id == owned.id);
		CHECK(f.controller.Current()->owner == owned.owner);
		CHECK(f.controller.Current()->connection == owned.connection);
		CHECK(f.controller.Current()->baseline.source == owned.baseline.source);
		CHECK(!f.controller.NeedsPump());
		CHECK(f.backend.snapshot.rate == 0);
		CHECK(!f.Release("other").ok);
		CHECK(!f.Release("mapping", "mcp:other").ok);
		f.backend.failWrite = false;
		const auto attempts = f.backend.writeAttempts;
		f.controller.Tick(1000);
		f.controller.End(reason, true);
		CHECK(f.backend.writeAttempts == attempts);
		CHECK(f.controller.Current().has_value());
		CHECK(f.Release().restored);
		CHECK(f.backend.snapshot.rate == 20);
		CHECK(!f.controller.Current());
	}
}

TEST_CASE("calendar unavailable cleanup readback retains uncertainty without writing")
{
	for (const bool lifecycle : { false, true }) {
		Fixture f;
		CHECK(f.Hold().ok);
		f.backend.snapshot.available = false;
		const auto attempts = f.backend.writeAttempts;
		const auto result = lifecycle ? f.controller.End("loading_menu", true) : f.controller.Tick(110);
		CHECK(result.code == "restore_failed_requires_explicit_release");
		CHECK(f.controller.Current().has_value());
		CHECK(!f.controller.NeedsPump());
		CHECK(f.backend.writeAttempts == attempts);
		f.backend.snapshot.available = true;
		CHECK(f.Release().restored);
		CHECK(f.backend.snapshot.rate == 20);
	}
}

TEST_CASE("calendar unverified lifecycle restore does not claim success or replay writes")
{
	Fixture f;
	CHECK(f.Hold().ok);
	f.backend.hideAfterWrite = true;
	CHECK(!f.controller.End("unsupported_save_event", true).restored);
	CHECK(f.controller.Current().has_value());
	CHECK(f.controller.Result().code == "restore_failed_requires_explicit_release");
	CHECK(f.backend.snapshot.rate == 20);
	const auto attempts = f.backend.writeAttempts;
	f.controller.End("unsupported_save_event", true);
	CHECK(f.backend.writeAttempts == attempts);
	// Observing a nonzero rate later cannot attribute it to our earlier write.
	CHECK(!f.Release().restored);
	CHECK(f.controller.Result().code == "external_rate_write_without_restore");
	CHECK(!f.controller.Current());
	CHECK(f.backend.writeAttempts == attempts);
}

TEST_CASE("calendar allowed lifecycle cleanup retires only proven invalidation")
{
	for (const bool storageChanged : { false, true }) {
		Fixture f;
		CHECK(f.Hold().ok);
		if (storageChanged)
			++f.backend.snapshot.source.generation;
		else
			f.backend.snapshot.rate = 7;
		const auto attempts = f.backend.writeAttempts;
		CHECK(!f.controller.End("pre_load", true).restored);
		CHECK(!f.controller.Current());
		CHECK(f.backend.writeAttempts == attempts);
	}
}

TEST_CASE("calendar wait and sleep admission refuses active and failed cleanup custody")
{
	for (const bool sleep : { false, true }) {
		Fixture f;
		CHECK(f.Hold().ok);
		int queued = 0, started = 0, ticks = 0;
		auto invoke = [&]() {
			RequireIdle(f.controller.Current().has_value());
			++queued;
			RequireIdle(f.controller.Current().has_value());
			++started;
			++ticks;
			return sleep;
		};
		try { invoke(); CHECK(false); }
		catch (const dvb::ToolError& error) { CHECK(error.code == 409); }
		CHECK(queued == 0 && started == 0 && ticks == 0);
		f.backend.failWrite = true;
		f.controller.End("loading_menu", true);
		CHECK_THROWS_AS(invoke(), dvb::ToolError);
		CHECK(queued == 0 && started == 0 && ticks == 0);
		f.backend.failWrite = false;
		CHECK(f.Release().restored);
		CHECK(invoke() == sleep);
		CHECK(queued == 1 && started == 1 && ticks == 1);
	}
}

TEST_CASE("calendar queued wait and sleep recheck a hold admitted after listener check")
{
	for (const bool sleep : { false, true }) {
		Fixture f;
		int started = 0, ticks = 0;
		RequireIdle(f.controller.Current().has_value());
		const auto start = dvb::MainThread::QueuedTask::Clock::now();
		dvb::MainThread::QueuedTask task([&]() -> dvb::json {
			RequireIdle(f.controller.Current().has_value());
			++started;
			++ticks;
			return sleep;
		}, start + std::chrono::seconds(1));
		auto future = task.GetFuture();
		CHECK(f.Hold().ok);
		task.Run(start);
		CHECK_THROWS_AS(future.get(), dvb::ToolError);
		CHECK(started == 0 && ticks == 0);
		CHECK(f.backend.snapshot.rate == 0);
	}
}

TEST_CASE("calendar wait and sleep admission resumes after verified expiry")
{
	for (const bool sleep : { false, true }) {
		Fixture f;
		CHECK(f.Hold().ok);
		CHECK(f.controller.Tick(110).restored);
		int ticks = 0;
		RequireIdle(f.controller.Current().has_value());
		RequireIdle(f.controller.Current().has_value());
		++ticks;
		CHECK(ticks == 1);
		CHECK(f.backend.snapshot.rate == 20);
		(void)sleep;
	}
}

TEST_CASE("calendar abandoned queued invocation never applies a hold")
{
	Fixture f;
	const auto deadline = dvb::MainThread::QueuedTask::Clock::now();
	dvb::MainThread::QueuedTask task([&]() -> dvb::json { return f.Hold().ok; }, deadline);
	task.Run(deadline);
	CHECK(f.backend.writes == 0);
	CHECK(!f.controller.Current());
}

TEST_CASE("calendar already-started late completion leaves a bounded identifiable lease")
{
	Fixture f;
	dvb::MainThread::QueuedTask* running = nullptr;
	const auto start = dvb::MainThread::QueuedTask::Clock::now();
	dvb::MainThread::QueuedTask task([&]() -> dvb::json {
		CHECK(!running->Abandon());
		CHECK(f.Hold().ok);
		CHECK(f.controller.Current()->command == "hold-command");
		CHECK(f.controller.Current()->deadline == 110);
		return true;
	}, start + std::chrono::seconds(1));
	running = &task;
	task.Run(start);
	CHECK(f.controller.Tick(110).restored);
	CHECK(f.backend.snapshot.engineMultiplier == 1);
	CHECK(f.backend.snapshot.rate == 20);
}

TEST_CASE("calendar runtime protocol refuses unknown controls and mistyped bindings")
{
	using dvb::json;
	CHECK(ParseRequest(json{ { "action", "status" } }).action == "status");
	CHECK_THROWS_AS(ParseRequest(json{ { "action", "status" }, { "holdMs", 100 } }), dvb::ToolError);
	json hold{ { "action", "hold" }, { "owner", "mapping" }, { "commandId", "command" }, { "holdMs", 100 },
		{ "binding", { { "processSession", "process:created" }, { "pid", 1 }, { "loadGeneration", 1 },
			{ "cellFormId", 42 }, { "globalFormIds", json::array({ 1, 2, 3, 4, 5, 6 }) } } } };
	CHECK(ParseRequest(hold).duration == 100);
	for (const auto* unsupported : { "hour", "date", "freeze", "scale", "leaseId" }) {
		auto invalid = hold;
		invalid[unsupported] = 0;
		CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	}
	auto invalid = hold;
	invalid["binding"]["pid"] = "1";
	CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	invalid = hold;
	invalid["binding"]["globalFormIds"][0] = 1.5;
	CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	invalid = hold;
	invalid["binding"]["loadGeneration"] = -1;
	CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	invalid = hold;
	invalid["holdMs"] = true;
	CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	invalid = hold;
	invalid["owner"] = "";
	CHECK_THROWS_AS(ParseRequest(invalid), dvb::ToolError);
	invalid = hold;
	invalid["action"] = "release";
	invalid.erase("holdMs");
	invalid["leaseId"] = "1";
	CHECK(ParseRequest(invalid).lease == "1");
}

TEST_CASE("calendar descriptor publishes the same typed protocol used by the native tool")
{
	const auto descriptor = Descriptor();
	CHECK(descriptor.name == "calendar");
	CHECK(!descriptor.readOnly);
	CHECK(descriptor.inputSchema["properties"]["holdMs"]["maximum"] == kMaximumHoldMs);
	CHECK(descriptor.inputSchema["properties"]["binding"]["properties"]["globalFormIds"]["maxItems"] == 6);
	CHECK(descriptor.inputSchema["oneOf"][0]["maxProperties"] == 1);
	std::printf("CALENDAR_DESCRIPTOR_JSON=%s\n", dvb::json{ { "name", descriptor.name },
		{ "description", descriptor.description }, { "inputSchema", descriptor.inputSchema }, { "readOnly", descriptor.readOnly } }.dump().c_str());
}
