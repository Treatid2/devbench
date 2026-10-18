#include "test_framework.h"
#include "EventBus.h"
#include "PostLoadStartup.h"
#include <future>

TEST_CASE("deferred bus preparation starts no delivery until its safe owner phase")
{
    dvb::EventBus bus(false);
    std::promise<void> delivered;
    auto result = delivered.get_future();
    const auto sub = bus.Subscribe([&](const dvb::EventBus::Event& event) {
        CHECK(event.topic == "after-start");
        delivered.set_value();
    });
    bus.Publish("before-start", dvb::json::object());
    CHECK(bus.HeadSeq() == 1);
    CHECK(bus.Since(0).size() == 1);
    CHECK(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    bus.StartDelivery();
    bus.StartDelivery(); // Idempotent owner call, not a second worker.
    bus.Publish("after-start", dvb::json::object());
    CHECK(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    bus.Unsubscribe(sub);
}
