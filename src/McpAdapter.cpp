#include "McpAdapter.h"

#include "EventBus.h"
#include "McpContent.h"
#include "ToolRegistry.h"

#include <mcp_server.h>
#include <mcp_tool.h>

namespace dvb
{
	McpAdapter::McpAdapter(ToolRegistry& a_registry, EventBus& a_events, mcp::server& a_server) :
		m_registry(a_registry), m_events(a_events), m_server(a_server),
		m_callbacks(std::make_shared<CallbackLease<McpAdapter>>(*this))
	{}

	McpAdapter::~McpAdapter()
	{
		m_callbacks->Retire();
		m_registry.SetRegistrationListener({});
		if (m_sub)
			m_events.Unsubscribe(m_sub);
	}

	void McpAdapter::Wire()
	{
		// Generic registration — forwards to the registry by name, so no tool-specific
		// code lives in the adapter. Used for both existing tools and (via the registry's
		// registration listener) tools added later by cross-plugin consumers at kPostLoad.
		auto registerOne = [lease = m_callbacks](const ToolDescriptor& a_desc) {
			lease->Invoke([&](McpAdapter& self) {
				mcp::tool t;
				t.name = a_desc.name;
				t.description = a_desc.description;
				t.parameters_schema = a_desc.inputSchema;

				// Handlers need the registry, not the adapter address. Its Server
				// owner must outlive all transport handlers and escaped consumers.
				self.m_server.register_tool(t, [registry = &self.m_registry, name = a_desc.name](const json& a_args, const std::string& a_session) -> json {
					const ToolResult r = registry->Invoke(name, a_args, ToolContext{ a_session });
					if (r.ok)
						return ToContentBlocks(r.value);  // content ARRAY — see McpContent.h
					// cpp-mcp constructs isError:true from what(); preserve legacy
					// status text or the deliberately supplied structured receipt.
					throw std::runtime_error(ToolErrorText(r));
				});

				// A tool was added or replaced; tell connected clients to re-list.
				if (self.m_announceChanges)
					self.m_server.broadcast_notification(mcp::request::create_notification(
						"notifications/tools/list_changed", json::object()));
			});
		};

		for (const auto& desc : m_registry.List())
			registerOne(desc);

		// Expose tools registered after wiring (cross-plugin consumers). Note: this can
		// call register_tool from the main/message thread post-start; safe in practice
		// because consumers register at kPostLoad, before any MCP client connects.
		m_registry.SetRegistrationListener(registerOne);
		m_callbacks->Invoke([](McpAdapter& self) { self.m_announceChanges = true; });

		// Forward bus events to connected MCP clients as notifications.
		m_sub = m_events.Subscribe([lease = m_callbacks](const EventBus::Event& a_ev) {
			lease->Invoke([&](McpAdapter& self) {
				self.m_server.broadcast_notification(mcp::request::create_notification(
					"notifications/message",
					json{ { "topic", a_ev.topic }, { "seq", a_ev.seq }, { "data", a_ev.payload } }));
			});
		});
	}
}
