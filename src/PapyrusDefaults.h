#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dvb::PapyrusDefaults
{
	/// Historical, unverified source hints, NOT defaults from a loaded declaration.
	/// No source/version/signature provenance binds this corpus to a running VM.
	/// Papyrus call never applies it: every runtime argument must be explicit.
	struct Value
	{
		enum class Kind
		{
			kBool,
			kInt,
			kFloat,
		};
		Kind   kind;
		double number;
	};

	/// Lookup requires script/function, zero-based position and kind, plus either
	/// the case-insensitive stored name or a coherent one-based paramN placeholder.
	/// A match is still only an unverified hint, never optionality/default authority.
	std::optional<Value> Find(std::string_view a_script, std::string_view a_function, std::string_view a_param,
		std::uint32_t a_index, Value::Kind a_kind);
}
