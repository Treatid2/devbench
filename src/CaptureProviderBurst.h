#pragma once

#include "ToolExtensions.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace dvb::Capture
{
	inline bool SameBurstProvider(std::string_view a_left, std::string_view a_right)
	{
		return std::ranges::equal(a_left, a_right, [](unsigned char left, unsigned char right) {
			return std::tolower(left) == std::tolower(right);
		});
	}

	inline bool IsCsxBurstFilename(std::string_view a_name, std::size_t a_limit)
	{
		if (a_name.empty() || a_name.size() > a_limit || a_name.back() == ' ' || a_name.back() == '.')
			return false;
		for (const unsigned char ch : a_name) {
			if (ch < 0x20 || ch > 0x7e || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
				return false;
		}
		const auto stem = a_name.substr(0, a_name.find('.'));
		for (const auto device : { "CON", "PRN", "AUX", "NUL" }) {
			if (SameBurstProvider(stem, device))
				return false;
		}
		return stem.size() != 4 || stem[3] < '1' || stem[3] > '9' ||
			   (!SameBurstProvider(stem.substr(0, 3), "COM") && !SameBurstProvider(stem.substr(0, 3), "LPT"));
	}

	// The CSX reference provider validates these same input-only gates before
	// DispatchRequest. Check them here too, before reserving an uncertain start.
	// Provider-side filesystem, runtime and admission checks still apply.
	inline void ValidateCsxBurstStart(const json& a_args)
	{
		const auto frames = [&](const char* a_field) {
			const auto it = a_args.find(a_field);
			const auto n = it != a_args.end() && it->is_number_integer() ? it->get<std::uint64_t>() : 0;
			if (n < 1 || n > 600)
				throw ToolError(400, std::string("capture burst '") + a_field + "' must be an integer in 1..600");
			return n;
		};
		const auto count = frames("count");
		const auto cadence = a_args.contains("everyNFrames") ? frames("everyNFrames") : 1;
		if ((count - 1) * cadence > 216000)
			throw ToolError(400, "capture burst cadence exceeds the CSX 216000-frame span limit");

		const auto prefix = a_args.find("prefix");
		if (prefix == a_args.end() || !prefix->is_string() ||
			!IsCsxBurstFilename(prefix->get_ref<const std::string&>(), 128))
			throw ToolError(400, "capture burst requires a safe ASCII filename prefix of 1..128 bytes");

		const auto absolute = [&](const char* a_field) {
			const auto it = a_args.find(a_field);
			if (it != a_args.end() && it->is_string()) {
				const auto& value = it->get_ref<const std::string&>();
				if (value.find('\0') == std::string::npos) {
					try {
						auto path = std::filesystem::path(std::u8string(value.begin(), value.end())).lexically_normal();
						if (path.is_absolute())
							return path;
					} catch (const std::filesystem::filesystem_error&) {}
				}
			}
			throw ToolError(400, std::string("capture burst '") + a_field + "' must be an absolute UTF-8 path");
		};
		(void)absolute("outputDir");
		if (a_args.contains("manifestPath")) {
			const auto manifest = absolute("manifestPath");
			const auto name = manifest.filename().u8string();
			if (!IsCsxBurstFilename({ reinterpret_cast<const char*>(name.data()), name.size() }, 200) ||
				!SameBurstProvider(manifest.extension().string(), ".json"))
				throw ToolError(400, "capture burst manifestPath must name a safe JSON file");
		}
		if (a_args.contains("scale") && (!a_args["scale"].is_number() || a_args["scale"] != 1))
			throw ToolError(400, "capture burst supports only whole native frames with scale=1");
		if (a_args.contains("excludeUi") && !a_args["excludeUi"].is_boolean())
			throw ToolError(400, "capture burst excludeUi must be a boolean");
	}

	// Use the descriptor and handler from the same registration snapshot. A legacy
	// provider might ignore action and start a single capture if called anyway.
	inline void ValidateProviderBurst(const ToolExtensions::Entry& a_provider, const json& a_args)
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
	}

	inline json InvokeProviderBurst(const ToolExtensions::Entry& a_provider, const json& a_args, const ToolContext& a_ctx)
	{
		ValidateProviderBurst(a_provider, a_args);
		return a_provider.handler(a_args, a_ctx);
	}

	// One outstanding provider-owned start at a time. Never call a provider
	// under this mutex: providers may marshal to the game thread and emit events.
	class ProviderBurstRun
	{
	public:
		json Start(const std::string& a_key, const ToolExtensions::Entry& a_provider, const json& a_args,
			const ToolContext& a_ctx, std::uint64_t a_eventHead)
		{
			// These are definite refusals because the handler has not been called.
			ValidateProviderBurst(a_provider, a_args);
			const auto request = a_args.find("requestId");
			if (request == a_args.end() || !request->is_string() ||
				request->get_ref<const std::string&>().empty() || request->get_ref<const std::string&>().size() > 128)
				throw ToolError(400, "capture provider burst requires a requestId of 1..128 bytes");
			if (SameBurstProvider(a_key, "communityshaders"))
				ValidateCsxBurstStart(a_args);
			{
				const std::lock_guard lock(m_mutex);
				if (m_run)
					throw ToolError(409, "a provider burst may still be active; reconcile its original request via burstStatus or capture.burst.ready");
				m_run = Run{ a_key, request->get<std::string>(), {}, {}, a_eventHead };
				++m_generation;
				m_starting = true;
			}
			try {
				auto reply = a_provider.handler(a_args, a_ctx);
				const std::lock_guard lock(m_mutex);
				ApplyReceipt(a_key, reply);
				m_starting = false;
				return reply;
			} catch (...) {
				const std::lock_guard lock(m_mutex);
				// Dispatch may already have admitted the sequence. Keep the original
				// request and exclusion until matching proof arrives.
				m_starting = false;
				throw;
			}
		}

		std::optional<std::uint64_t> EventCursor() const
		{
			const std::lock_guard lock(m_mutex);
			return m_run && !m_starting ? std::optional(m_run->sinceSeq) : std::nullopt;
		}

		json Status(const std::string& a_key, const ToolExtensions::Entry& a_provider, const json& a_args,
			const ToolContext& a_ctx)
		{
			std::optional<std::uint64_t> generation;
			{
				const std::lock_guard lock(m_mutex);
				if (m_run && !m_starting)
					generation = m_generation;
			}
			auto reply = InvokeProviderBurst(a_provider, a_args, a_ctx);
			const auto asked = a_args.find("runId");
			const auto answered = reply.find("runId");
			const std::lock_guard lock(m_mutex);
			if (generation == m_generation && asked != a_args.end() && answered != reply.end() && *asked == *answered)
				ApplyReceipt(a_key, reply);
			return reply;
		}

		void ObserveTerminal(std::string_view a_key, const json& a_reply, std::uint64_t a_seq)
		{
			const std::lock_guard lock(m_mutex);
			if (m_run && !m_starting && a_seq > m_run->sinceSeq &&
				a_reply.contains("done") && a_reply["done"] == true)
				ApplyReceipt(a_key, a_reply);
		}

		bool InFlight() const
		{
			const std::lock_guard lock(m_mutex);
			return m_run.has_value();
		}

	private:
		// Caller holds m_mutex. Unknown replies leave both the identity and the
		// exclusion intact. Bound IDs cannot be replaced by a later receipt.
		void ApplyReceipt(std::string_view a_key, const json& a_reply)
		{
			if (!m_run || !SameBurstProvider(a_key, m_run->provider))
				return;
			const auto done = a_reply.find("done");
			const auto request = a_reply.find("requestId");
			const auto id = a_reply.find("runId");
			const auto apiId = a_reply.find("apiRequestId");
			if (done == a_reply.end() || !done->is_boolean() || request == a_reply.end() || *request != m_run->request ||
				id == a_reply.end() || !id->is_number_integer() ||
				apiId == a_reply.end() || !apiId->is_string() || apiId->get_ref<const std::string&>().empty())
				return;
			const auto runId = id->get<std::uint64_t>();
			if (runId == 0 || runId > 9007199254740991ULL)
				return;
			if ((!m_run->id.is_null() && m_run->id != *id) || (!m_run->apiId.is_null() && m_run->apiId != *apiId))
				return;
			if (*done == true) {
				m_run.reset();
				return;
			}
			m_run->id = *id;
			m_run->apiId = *apiId;
		}

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
		std::uint64_t m_generation = 0;
		bool m_starting = false;
	};
}
