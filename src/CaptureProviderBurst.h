#pragma once

#include "ToolExtensions.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>

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
		if (action == "burst" && a_args.contains("subrect") && !a_args["subrect"].is_null())
			throw ToolError(400, "capture burst requires whole frames; subrect is available for single capture");

		// The provider owns request identity, errors, and terminal publication.
		return a_provider.handler(a_args, a_ctx);
	}

	// One provider-owned start or accepted run at a time. Never call a provider
	// under this mutex: providers may marshal to the game thread and emit events.
	class ProviderBurstRun
	{
	public:
		json Start(const std::string& a_key, const ToolExtensions::Entry& a_provider, const json& a_args,
			const ToolContext& a_ctx, std::uint64_t a_eventHead)
		{
			{
				const std::lock_guard lock(m_mutex);
				if (m_starting || m_run)
					throw ToolError(409, "a provider burst is starting or active; reconcile it with burstStatus");
				m_starting = true;
			}
			try {
				auto reply = InvokeProviderBurst(a_provider, a_args, a_ctx);
				const std::lock_guard lock(m_mutex);
				if (reply.is_object() && reply.contains("queued") && reply["queued"] == true) {
					const auto request = a_args.at("requestId").get<std::string>();
					m_run = Run{ a_key, request, reply.at("runId"), reply.at("apiRequestId"), a_eventHead };
				}
				m_starting = false;
				return reply;
			} catch (...) {
				const std::lock_guard lock(m_mutex);
				m_starting = false;
				throw;
			}
		}

		std::optional<std::uint64_t> EventCursor() const
		{
			const std::lock_guard lock(m_mutex);
			return m_run ? std::optional(m_run->sinceSeq) : std::nullopt;
		}

		json Status(const std::string& a_key, const ToolExtensions::Entry& a_provider, const json& a_args,
			const ToolContext& a_ctx)
		{
			auto reply = InvokeProviderBurst(a_provider, a_args, a_ctx);
			ObserveTerminal(a_key, reply);
			return reply;
		}

		void ObserveTerminal(std::string_view a_key, const json& a_reply, std::optional<std::uint64_t> a_seq = std::nullopt)
		{
			const std::lock_guard lock(m_mutex);
			if (!m_run || (a_seq && *a_seq <= m_run->sinceSeq))
				return;
			const auto& run = *m_run;
			if (!std::ranges::equal(a_key, run.provider, [](unsigned char left, unsigned char right) {
					return std::tolower(left) == std::tolower(right);
				}))
				return;
			if (a_reply.contains("done") && a_reply["done"] == true &&
				a_reply.contains("requestId") && a_reply["requestId"] == run.request &&
				a_reply.contains("runId") && a_reply["runId"] == run.id &&
				a_reply.contains("apiRequestId") && a_reply["apiRequestId"] == run.apiId)
				m_run.reset();
		}

		bool InFlight() const
		{
			const std::lock_guard lock(m_mutex);
			return m_starting || m_run.has_value();
		}

	private:
		struct Run
		{
			std::string   provider;
			std::string   request;
			json          id;
			json          apiId;
			std::uint64_t sinceSeq;
		};

		mutable std::mutex m_mutex;
		std::optional<Run> m_run;
		bool m_starting = false;
	};
}
