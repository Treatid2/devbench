#include "PapyrusDefaults.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace dvb::PapyrusDefaults
{
	namespace
	{
		struct Entry
		{
			std::string_view script;
			std::string_view function;
			std::string_view param;
			std::uint32_t    index;  // 0-based position in the .psc signature
			Value            value;
		};

		constexpr Value Bool(bool a_value) { return { Value::Kind::kBool, a_value ? 1.0 : 0.0 }; }
		constexpr Value Int(int a_value) { return { Value::Kind::kInt, static_cast<double>(a_value) }; }
		constexpr Value Float(double a_value) { return { Value::Kind::kFloat, a_value }; }

		// Retained unverified hints only; an unlisted slot receives nothing.
		constexpr Entry kEntries[] = {
			{ "ObjectReference", "PlaceAtMe", "aiCount", 1, Int(1) },
			{ "ObjectReference", "PlaceActorAtMe", "aiLevelMod", 1, Int(4) },
			{ "ObjectReference", "MoveTo", "abMatchRotation", 4, Bool(true) },
			{ "ObjectReference", "AddItem", "aiCount", 1, Int(1) },
			{ "ObjectReference", "RemoveItem", "aiCount", 1, Int(1) },
			{ "ObjectReference", "DropObject", "aiCount", 1, Int(1) },
			{ "ObjectReference", "Lock", "abLock", 0, Bool(true) },
			{ "ObjectReference", "SetOpen", "abOpen", 0, Bool(true) },
			{ "ObjectReference", "SetDestroyed", "abDestroyed", 0, Bool(true) },
			{ "ObjectReference", "BlockActivation", "abBlocked", 0, Bool(true) },
			{ "ObjectReference", "SetMotionType", "abAllowActivate", 1, Bool(true) },
			{ "ObjectReference", "IgnoreFriendlyHits", "abIgnore", 0, Bool(true) },
			{ "ObjectReference", "SetNoFavorAllowed", "abNoFavor", 0, Bool(true) },
			{ "ObjectReference", "PlayImpactEffect", "afPickDirZ", 4, Float(-1.0) },
			{ "ObjectReference", "PlayImpactEffect", "afPickLength", 5, Float(512.0) },
			{ "Actor", "AddSpell", "abVerbose", 1, Bool(true) },
			{ "Actor", "SetGhost", "abIsGhost", 0, Bool(true) },
			{ "Actor", "SetRestrained", "abRestrained", 0, Bool(true) },
			{ "Actor", "SetDontMove", "abDontMove", 0, Bool(true) },
			{ "Actor", "SetAlert", "abAlerted", 0, Bool(true) },
			{ "Actor", "SetUnconscious", "abUnconscious", 0, Bool(true) },
			{ "Actor", "EnableAI", "abEnable", 0, Bool(true) },
			{ "Actor", "SetHeadTracking", "abEnable", 0, Bool(true) },
			{ "Actor", "SetExpressionOverride", "aiStrength", 1, Int(100) },
			{ "Actor", "EquipItemEx", "equipSound", 3, Bool(true) },
			{ "Game", "EnablePlayerControls", "abMovement", 0, Bool(true) },
			{ "Game", "EnablePlayerControls", "abFighting", 1, Bool(true) },
			{ "Game", "EnablePlayerControls", "abCamSwitch", 2, Bool(true) },
			{ "Game", "EnablePlayerControls", "abLooking", 3, Bool(true) },
			{ "Game", "EnablePlayerControls", "abSneaking", 4, Bool(true) },
			{ "Game", "EnablePlayerControls", "abMenu", 5, Bool(true) },
			{ "Game", "EnablePlayerControls", "abActivate", 6, Bool(true) },
			{ "Game", "EnablePlayerControls", "abJournalTabs", 7, Bool(true) },
			{ "Game", "DisablePlayerControls", "abMovement", 0, Bool(true) },
			{ "Game", "DisablePlayerControls", "abFighting", 1, Bool(true) },
			{ "Game", "DisablePlayerControls", "abMenu", 5, Bool(true) },
			{ "Game", "DisablePlayerControls", "abActivate", 6, Bool(true) },
			{ "Game", "ShakeCamera", "afStrength", 1, Float(0.5) },
			{ "Game", "SetPlayerAIDriven", "abAIDriven", 0, Bool(true) },
			{ "VisualEffect", "Play", "afTime", 1, Float(-1.0) },
			{ "EffectShader", "Play", "afDuration", 1, Float(-1.0) },
			{ "ImageSpaceModifier", "Apply", "afStrength", 0, Float(1.0) },
			{ "ImageSpaceModifier", "ApplyCrossFade", "afFadeDuration", 0, Float(1.0) },
		};

		bool Same(std::string_view a_left, std::string_view a_right)
		{
			return std::ranges::equal(a_left, a_right, [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
		}

		bool UnnamedAt(std::string_view a_param, std::uint32_t a_index)
		{
			if (a_param.size() <= 5 || !Same(a_param.substr(0, 5), "param"))
				return false;
			const auto suffix = a_param.substr(5);
			std::uint64_t number = 0;
			const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), number);
			return error == std::errc{} && end == suffix.data() + suffix.size() &&
			       number == static_cast<std::uint64_t>(a_index) + 1;
		}
	}

	std::optional<Value> Find(std::string_view a_script, std::string_view a_function, std::string_view a_param,
		std::uint32_t a_index, Value::Kind a_kind)
	{
		const bool byIndex = UnnamedAt(a_param, a_index);
		for (const auto& e : kEntries)
			if (Same(e.script, a_script) && Same(e.function, a_function) &&
				e.index == a_index && e.value.kind == a_kind && (byIndex || Same(e.param, a_param)))
				return e.value;
		return std::nullopt;
	}
}
