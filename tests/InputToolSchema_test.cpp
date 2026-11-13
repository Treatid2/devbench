#include "test_framework.h"
#include "InputToolSchema.h"
#include "ToolRegistry.h"

#include <algorithm>
#include <string>
#include <vector>

using dvb::json;

TEST_CASE("input public registry listing retains read-only tracked observe")
{
	dvb::ToolRegistry registry;
	dvb::ToolDescriptor descriptor;
	descriptor.name = "input";
	descriptor.inputSchema = dvb::InputToolSchema(30000, 256, 10000);
	json notifiedSchema;
	registry.SetRegistrationListener([&](const dvb::ToolDescriptor& registered) {
		notifiedSchema = registered.inputSchema;
	});
	// No actual game handler or OpenVR call. Exercise the same public registry
	// descriptor/listener surfaces reflected by MCP and REST, not a fake runtime.
	registry.Register(std::move(descriptor), [](const json&, const dvb::ToolContext&) {
		return json::object();
	});
	const auto listed = registry.List();
	CHECK(listed.size() == 1);
	CHECK(listed.front().inputSchema == notifiedSchema);
	CHECK(registry.Describe("input")->inputSchema == notifiedSchema);
	const auto& properties = notifiedSchema.at("properties");
	CHECK(properties.at("action").at("type") == "string");
	const auto actions = properties.at("action").at("enum").get<std::vector<std::string>>();
	for (const char* action : { "capabilities", "status", "observe", "down", "up", "tap", "click", "move", "sequence", "stop", "releaseAll", "reach", "reachStatus", "reachStop" })
		CHECK(std::count(actions.begin(), actions.end(), action) == 1);
	CHECK(actions.size() == 14);
	CHECK(properties.at("device").at("enum") == json::array({ "keyboard", "mouse", "vrTrackedSet" }));
}

TEST_CASE("input schema extraction preserves producer bounds and ownership fields")
{
	const auto schema = dvb::InputToolSchema(29000, 42, 9000);
	CHECK(schema.at("type") == "object");
	const auto& properties = schema.at("properties");
	CHECK(properties.at("maxHoldMs").at("maximum") == 29000);
	CHECK(properties.at("events").at("maxItems") == 42);
	CHECK(properties.at("frames").at("maxItems") == 9000);
	CHECK(properties.at("key").at("oneOf").at(1).at("maximum") == 258);
	CHECK(properties.at("controlToken").at("type") == "string");
	CHECK(properties.at("surviveLifecycle").at("type") == "boolean");
	CHECK(properties.at("device").at("description").get<std::string>().find("observe requires vrTrackedSet") != std::string::npos);
	// No new action-specific required fields: historical default capabilities
	// and keyboard calls retain their existing registration contract.
	CHECK(!schema.contains("required"));
}
