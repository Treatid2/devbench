#include "test_framework.h"
#include "CallbackLease.h"
#include "PathUtf8.h"
#include "ToolRegistry.h"

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
	struct Target { int calls = 0; };
}

TEST_CASE("copied callback cannot reach an owner after retirement")
{
	auto target = std::make_unique<Target>();
	auto lease = std::make_shared<dvb::CallbackLease<Target>>(*target);
	auto copied = [lease] { return lease->Invoke([](Target& t) { ++t.calls; }); };
	CHECK(copied());
	CHECK(target->calls == 1);
	lease->Retire();
	target.reset();
	CHECK(!copied());
	CHECK_NOTHROW(lease->Retire());
}

TEST_CASE("a throwing serialized callback releases its admission")
{
	Target target;
	dvb::CallbackLease<Target> lease(target);
	CHECK_THROWS_AS(lease.Invoke([](Target&) { throw std::runtime_error("injected callback failure"); }), std::runtime_error);
	CHECK_NOTHROW(lease.Retire());
	CHECK(!lease.Invoke([](Target& t) { ++t.calls; }));
	CHECK(target.calls == 0);
}

TEST_CASE("retirement waits for an admitted callback before owner destruction")
{
	Target target;
	dvb::CallbackLease<Target, dvb::CallbackMode::Concurrent> lease(target);
	std::promise<void> entered;
	std::promise<void> release;
	auto permission = release.get_future().share();
	std::thread callback([&] {
		lease.Invoke([&](Target& t) { entered.set_value(); permission.wait(); ++t.calls; });
	});
	entered.get_future().wait();
	auto retirement = std::async(std::launch::async, [&] { lease.Retire(); });
	CHECK(retirement.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
	release.set_value();
	callback.join();
	retirement.get();
	CHECK(target.calls == 1);
	CHECK(!lease.Invoke([](Target& t) { ++t.calls; }));
}

TEST_CASE("concurrent route admission does not block health behind a waiting tool")
{
	Target target;
	dvb::CallbackLease<Target, dvb::CallbackMode::Concurrent> lease(target);
	std::promise<void> entered;
	std::promise<void> release;
	auto permission = release.get_future().share();
	std::thread tool([&] {
		lease.Invoke([&](Target&) { entered.set_value(); permission.wait(); });
	});
	entered.get_future().wait();
	auto health = std::async(std::launch::async, [&] {
		return lease.Invoke([](Target&) {});
	});
	const auto healthWhileToolPending = health.wait_for(std::chrono::seconds(2));
	// Always release/join before assertions: a failing serialization regression
	// must not leave this test's own worker waiting on the future destructor.
	release.set_value();
	tool.join();
	CHECK(healthWhileToolPending == std::future_status::ready);
	CHECK(health.get());
	lease.Retire();
	CHECK(!lease.Invoke([](Target&) {}));
}

TEST_CASE("a throwing concurrent callback releases its admission")
{
	Target target;
	dvb::CallbackLease<Target, dvb::CallbackMode::Concurrent> lease(target);
	CHECK_THROWS_AS(lease.Invoke([](Target&) { throw std::runtime_error("injected route failure"); }), std::runtime_error);
	CHECK_NOTHROW(lease.Retire());
	CHECK(!lease.Invoke([](Target&) {}));
}

TEST_CASE("a copied concurrent route cannot reach a destroyed owner")
{
	auto target = std::make_unique<Target>();
	auto lease = std::make_shared<dvb::CallbackLease<Target, dvb::CallbackMode::Concurrent>>(*target);
	auto route = [lease] { return lease->Invoke([](Target& t) { ++t.calls; }); };
	CHECK(route());
	CHECK(target->calls == 1);
	lease->Retire();
	target.reset();
	CHECK(!route());
	CHECK_NOTHROW(lease->Retire());
}

TEST_CASE("actual registry callback retains only a revocable owner lease")
{
	dvb::ToolRegistry registry;
	auto target = std::make_unique<Target>();
	auto lease = std::make_shared<dvb::CallbackLease<Target>>(*target);
	registry.SetRegistrationListener([lease](const dvb::ToolDescriptor&) {
		lease->Invoke([](Target& t) { ++t.calls; });
	});
	dvb::ToolDescriptor descriptor;
	descriptor.name = "before";
	registry.Register(descriptor, [](const dvb::json&, const dvb::ToolContext&) { return dvb::json::object(); });
	CHECK(target->calls == 1);
	lease->Retire();
	target.reset();
	// Intentionally leave the registration in place to model an already-copied
	// listener; clearing registry storage alone is not a revocation guarantee.
	descriptor.name = "after";
	CHECK_NOTHROW(registry.Register(descriptor, [](const dvb::json&, const dvb::ToolContext&) { return dvb::json::object(); }));
	registry.SetRegistrationListener({});
}

TEST_CASE("production discovery path conversion preserves non-ASCII UTF8 bytes")
{
	const std::u8string expected = u8"profiles/élève/玩家/😀/devbench-bridge.exe";
	const auto actual = dvb::PathUtf8(std::filesystem::path(expected));
	CHECK(actual == std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
	const dvb::json roundTrip = dvb::json::parse(dvb::json{{ "exePath", actual }}.dump());
	CHECK(roundTrip.at("exePath").get<std::string>() == actual);
}

TEST_CASE("production discovery path conversion preserves empty paths")
{
	CHECK(dvb::PathUtf8(std::filesystem::path{}).empty());
}
