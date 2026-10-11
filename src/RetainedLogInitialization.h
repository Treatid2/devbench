#pragma once

#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

namespace dvb
{
	enum class RetainedLogStage { path, sink, logger, publish, ready };
	enum class RetainedLogError { none, pathUnavailable, system, exception, unknown };
	struct RetainedLogResult
	{
		RetainedLogStage stage{ RetainedLogStage::path };
		RetainedLogError error{ RetainedLogError::none };
		int code{ 0 };  // meaningful only for system_error; no raw exception/path text
		bool Ready() const noexcept { return stage == RetainedLogStage::ready; }
	};

	inline std::string_view RetainedLogStageName(RetainedLogStage stage) noexcept
	{
		switch (stage) {
		case RetainedLogStage::path: return "path";
		case RetainedLogStage::sink: return "sink";
		case RetainedLogStage::logger: return "logger";
		case RetainedLogStage::publish: return "publish";
		case RetainedLogStage::ready: return "ready";
		}
		return "unknown";
	}
	inline std::string_view RetainedLogErrorName(RetainedLogError error) noexcept
	{
		switch (error) {
		case RetainedLogError::none: return "none";
		case RetainedLogError::pathUnavailable: return "path_unavailable";
		case RetainedLogError::system: return "system_error";
		case RetainedLogError::exception: return "exception";
		case RetainedLogError::unknown: return "unknown_exception";
		}
		return "unknown";
	}

	// Production and host tests use the same ordering/error boundary. Configure
	// the candidate completely before publishing it as the ordinary logger.
	// Callers own concrete path/sink/logger types and all filesystem side effects.
	template<class Resolve, class CreateSink, class Configure, class Publish>
	RetainedLogResult InitializeRetainedLog(Resolve&& resolve, CreateSink&& createSink,
		Configure&& configure, Publish&& publish) noexcept
	{
		RetainedLogResult result;
		try {
			auto path = resolve();
			if (!path) { result.error = RetainedLogError::pathUnavailable; return result; }
			result.stage = RetainedLogStage::sink;
			auto sink = createSink(*path);
			result.stage = RetainedLogStage::logger;
			auto logger = configure(std::move(sink));
			result.stage = RetainedLogStage::publish;
			publish(std::move(logger));
			result.stage = RetainedLogStage::ready;
		} catch (const std::system_error& error) {
			result.error = RetainedLogError::system;
			result.code = error.code().value();
		} catch (const std::exception&) {
			result.error = RetainedLogError::exception;
		} catch (...) {
			result.error = RetainedLogError::unknown;
		}
		return result;
	}
}
