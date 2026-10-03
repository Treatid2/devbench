#pragma once

#include "Json.h"

#include <array>
#include <optional>
#include <string_view>

namespace dvb::HandObservation
{
	struct Hand
	{
		bool spellEquipped = false;
		bool casterPresent = false;
		bool artPresent = false;
		bool attached = false;
		bool loading = false;
		bool currentSpellMatchesEquipped = false;  // observed pointer comparison, not art identity
	};

	struct Hands
	{
		bool actorAnd3DPresent = false;
		bool settledDrawn = false;
		Hand left;
		Hand right;
	};

	// A deliberately conservative collection of observations, NOT a render/capture-ready
	// postcondition. Even equal current/equipped pointers do not identify the attached art.
	inline bool SpellArtObserved(const Hand& a_hand)
	{
		return a_hand.spellEquipped && a_hand.casterPresent && a_hand.artPresent &&
		       a_hand.attached && !a_hand.loading && a_hand.currentSpellMatchesEquipped;
	}

	inline bool AllSpellArtObserved(const Hands& a_hands)
	{
		return (!a_hands.left.spellEquipped || SpellArtObserved(a_hands.left)) &&
		       (!a_hands.right.spellEquipped || SpellArtObserved(a_hands.right));
	}

	inline constexpr std::array<std::string_view, 8> kConditions{
		"weaponStateSettledDrawn", "handsArtObserved", "castingArtAttachedLeft", "castingArtAttachedRight",
		"weaponDrawn", "handsReady", "castingArtLeft", "castingArtRight"
	};

	inline bool IsCondition(std::string_view a_name)
	{
		for (const auto name : kConditions)
			if (name == a_name)
				return true;
		return false;
	}

	inline std::optional<bool> Condition(std::string_view a_name, const Hands& a_hands)
	{
		if (!IsCondition(a_name))
			return std::nullopt;
		if (!a_hands.actorAnd3DPresent)
			return false;
		if (a_name == "weaponStateSettledDrawn" || a_name == "weaponDrawn")
			return a_hands.settledDrawn;
		if (a_name == "handsArtObserved" || a_name == "handsReady")
			return a_hands.settledDrawn && AllSpellArtObserved(a_hands);
		const auto& hand = (a_name == "castingArtAttachedLeft" || a_name == "castingArtLeft") ? a_hands.left : a_hands.right;
		return hand.casterPresent && hand.attached;  // raw flag only; no identity/loading promise
	}

	inline json HandFields(const Hand& a_hand)
	{
		return json{
			{ "spellArtObserved", SpellArtObserved(a_hand) },
			{ "currentSpellMatchesEquipped", a_hand.spellEquipped && a_hand.casterPresent ? json(a_hand.currentSpellMatchesEquipped) : json(nullptr) },
		};
	}

	inline json HandsFields(const Hands& a_hands)
	{
		return json{
			{ "artObservationVersion", 2 },
			{ "has3D", a_hands.actorAnd3DPresent },
			{ "weaponStateSettledDrawn", a_hands.actorAnd3DPresent && a_hands.settledDrawn },
			{ "allSpellArtObserved", AllSpellArtObserved(a_hands) },
			{ "handsArtObserved", a_hands.actorAnd3DPresent && a_hands.settledDrawn && AllSpellArtObserved(a_hands) },
			{ "castingArtReady", AllSpellArtObserved(a_hands) },  // deprecated non-readiness alias
			{ "visibleSpellArtProven", false },
		};
	}
}
