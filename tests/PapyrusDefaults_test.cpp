#include "test_framework.h"
#include "PapyrusDefaults.h"

using dvb::PapyrusDefaults::Find;
using Kind = dvb::PapyrusDefaults::Value::Kind;

TEST_CASE("historical Papyrus hints require coherent name index and kind")
{
	const auto v = Find("ObjectReference", "PlaceAtMe", "aiCount", 1, Kind::kInt);
	CHECK(v.has_value());
	CHECK(v->number == 1.0);
	CHECK(!Find("ObjectReference", "PlaceAtMe", "aiCount", 0, Kind::kInt));
	CHECK(!Find("ObjectReference", "PlaceAtMe", "aiCount", 1, Kind::kBool));
	CHECK(Find("objectreference", "moveto", "ABMATCHROTATION", 4, Kind::kBool));
	CHECK(!Find("MyMod", "DoThing", "aiCount", 1, Kind::kInt));
}

TEST_CASE("historical Papyrus hints validate the whole one based placeholder")
{
	CHECK(Find("ObjectReference", "PlaceAtMe", "param2", 1, Kind::kInt));
	for (const auto name : { "param999", "param0", "param2x", "param-2", "param18446744073709551616" })
		CHECK(!Find("ObjectReference", "PlaceAtMe", name, 1, Kind::kInt));
	CHECK(!Find("ObjectReference", "PlaceAtMe", "param2", 0, Kind::kInt));
	CHECK(!Find("ObjectReference", "PlaceAtMe", "abForcePersist", 1, Kind::kInt));
}
