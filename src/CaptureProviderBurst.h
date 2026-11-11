#pragma once

#include "ToolExtensions.h"

#include <algorithm>

namespace dvb::Capture
{
	// Use the descriptor and handler from the same registration snapshot. A legacy
	// provider might ignore action and start a single capture if called anyway.
	inline json InvokeProviderBurst(const ToolExtensions::Entry& a_provider, const json& a_args, const ToolContext& a_ctx)
	{
		const auto& action = a_args.at("action");
		const json* actions = &a_provider.descriptor;
		for (const char* field : { "inputSchema", "properties", "action", "enum" }) {
			const auto it = actions->find(field);
			if (it == actions->end())
				throw ToolError(400, "capture provider does not advertise the requested burst action");
			actions = &*it;
		}
		if (!actions->is_array() || std::find(actions->begin(), actions->end(), action) == actions->end())
			throw ToolError(400, "capture provider does not advertise the requested burst action");

		// The provider owns request identity, errors, and terminal publication.
		return a_provider.handler(a_args, a_ctx);
	}
}
