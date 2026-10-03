#include "test_framework.h"
#include "HandObservation.h"

namespace hand = dvb::HandObservation;

namespace
{
	hand::Hand ObservedSpell() { return { true, true, true, true, false, true }; }
	hand::Hands Settled() { return { true, true, {}, {} }; }
}

TEST_CASE("hand observation non-spell hands are vacuous not visibility proof")
{
	const auto snapshot = Settled();
	CHECK(hand::AllSpellArtObserved(snapshot));
	CHECK(*hand::Condition("handsArtObserved", snapshot));
	const auto fields = hand::HandsFields(snapshot);
	CHECK(fields.at("artObservationVersion") == 2);
	CHECK(fields.at("visibleSpellArtProven") == false);
	CHECK(hand::HandFields(snapshot.left).at("currentSpellMatchesEquipped").is_null());
}

TEST_CASE("hand observation one spell and one weapon uses only spell requirements")
{
	auto snapshot = Settled();
	snapshot.left = ObservedSpell();
	CHECK(hand::AllSpellArtObserved(snapshot));
	CHECK(*hand::Condition("handsArtObserved", snapshot));
	CHECK(hand::HandFields(snapshot.left).at("spellArtObserved") == true);
}

TEST_CASE("hand observation both spell hands must satisfy observation policy")
{
	auto snapshot = Settled();
	snapshot.left = ObservedSpell();
	snapshot.right = ObservedSpell();
	CHECK(*hand::Condition("handsArtObserved", snapshot));
	snapshot.right.attached = false;
	CHECK(!*hand::Condition("handsArtObserved", snapshot));
}

TEST_CASE("hand observation rejects absent caster or art and detached art")
{
	auto spell = ObservedSpell();
	spell.casterPresent = false;
	CHECK(!hand::SpellArtObserved(spell));
	spell = ObservedSpell();
	spell.artPresent = false;
	CHECK(!hand::SpellArtObserved(spell));
	spell = ObservedSpell();
	spell.attached = false;
	CHECK(!hand::SpellArtObserved(spell));
}

TEST_CASE("hand observation rejects loading despite attached flag")
{
	auto snapshot = Settled();
	snapshot.left = ObservedSpell();
	snapshot.left.loading = true;
	CHECK(!*hand::Condition("handsArtObserved", snapshot));
	CHECK(*hand::Condition("castingArtAttachedLeft", snapshot));  // explicitly weaker raw flag
}

TEST_CASE("hand observation A to B mismatch cannot satisfy composite observation")
{
	auto snapshot = Settled();
	snapshot.left = ObservedSpell();
	CHECK(*hand::Condition("handsArtObserved", snapshot));
	snapshot.left.currentSpellMatchesEquipped = false;
	CHECK(!*hand::Condition("handsArtObserved", snapshot));
	CHECK(hand::HandFields(snapshot.left).at("currentSpellMatchesEquipped") == false);
	snapshot.left.currentSpellMatchesEquipped = true;
	CHECK(*hand::Condition("handsArtObserved", snapshot));
	CHECK(hand::HandsFields(snapshot).at("visibleSpellArtProven") == false);  // equality is not proof
}

TEST_CASE("hand observation unsettled draw and unavailable 3D remain false")
{
	auto snapshot = Settled();
	snapshot.settledDrawn = false;
	CHECK(!*hand::Condition("weaponStateSettledDrawn", snapshot));
	CHECK(!*hand::Condition("handsArtObserved", snapshot));
	snapshot.settledDrawn = true;
	snapshot.actorAnd3DPresent = false;
	for (const auto name : hand::kConditions)
		CHECK(!*hand::Condition(name, snapshot));
	CHECK(hand::HandsFields(snapshot).at("handsArtObserved") == false);
	CHECK(hand::HandsFields(snapshot).at("weaponStateSettledDrawn") == false);
}

TEST_CASE("hand observation deprecated scenario names have explicit shared aliases")
{
	auto snapshot = Settled();
	snapshot.left = ObservedSpell();
	CHECK(hand::Condition("weaponDrawn", snapshot) == hand::Condition("weaponStateSettledDrawn", snapshot));
	CHECK(hand::Condition("handsReady", snapshot) == hand::Condition("handsArtObserved", snapshot));
	CHECK(hand::Condition("castingArtLeft", snapshot) == hand::Condition("castingArtAttachedLeft", snapshot));
	CHECK(hand::Condition("castingArtRight", snapshot) == hand::Condition("castingArtAttachedRight", snapshot));
	CHECK(!hand::IsCondition("handReady"));
	CHECK(!hand::Condition("handReady", snapshot).has_value());
}

TEST_CASE("hand observation emitted shared fields match scenario policy")
{
	for (const bool drawn : { false, true })
		for (const bool loading : { false, true }) {
			auto snapshot = Settled();
			snapshot.settledDrawn = drawn;
			snapshot.left = ObservedSpell();
			snapshot.left.loading = loading;
			const auto fields = hand::HandsFields(snapshot);
			CHECK(fields.at("handsArtObserved") == *hand::Condition("handsArtObserved", snapshot));
			CHECK(fields.at("weaponStateSettledDrawn") == *hand::Condition("weaponStateSettledDrawn", snapshot));
			CHECK(fields.at("castingArtReady") == fields.at("allSpellArtObserved"));
		}
}
