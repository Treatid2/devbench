#include "test_framework.h"

#include "VRInputState.h"

using dvb::json;
using dvb::ParseVRTrackedInputFrames;
using dvb::VRTrackedInputFrameJson;

namespace
{
	json Pose(int a_index)
	{
		return json{ { "available", true }, { "connected", true }, { "valid", true },
			{ "index", a_index }, { "trackingResult", 200 },
			{ "matrix", json::array({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 }) },
			{ "velocity", json::array({ 0, 0, 0 }) },
			{ "angularVelocity", json::array({ 0, 0, 0 }) } };
	}

	json Controller(int a_index)
	{
		json out = Pose(a_index);
		out["controller"] = json{ { "packetNumber", 4 }, { "pressed", 8 }, { "touched", 8 },
			{ "axes", json::array({ json::array({ 0.25, -0.5 }), json::array({ 0, 0 }),
						  json::array({ 0, 0 }), json::array({ 0, 0 }), json::array({ 0, 0 }) }) } };
		return out;
	}

	json Frame(std::int64_t a_tMs)
	{
		return json{ { "tMs", a_tMs }, { "originCode", 1 }, { "hmd", Pose(0) },
			{ "left", Controller(1) }, { "right", Controller(2) } };
	}
}

TEST_CASE("atomic VR tracked-set frames validate and round trip")
{
	const auto frames = ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20) }));
	CHECK(frames.size() == 2);
	CHECK(frames[1].tMs == 20);
	CHECK(frames[0].right.controller.pressed == 8);
	const json encoded = VRTrackedInputFrameJson(frames[0]);
	CHECK(encoded["hmd"]["index"] == 0);
	CHECK(encoded["left"]["controller"]["axes"][0][0] == 0.25f);
}

TEST_CASE("atomic VR tracked-set validation rejects incomplete or incoherent sequences")
{
	json missing = Frame(0);
	missing.erase("right");
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ missing })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(10) })));
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), Frame(20), Frame(10) })));

	json duplicate = Frame(0);
	duplicate["right"]["index"] = 1;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ duplicate })));

	json badAxis = Frame(0);
	badAxis["left"]["controller"]["axes"][0][0] = 2.0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ badAxis })));

	json changedIdentity = Frame(20);
	changedIdentity["right"]["index"] = 3;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedIdentity })));

	json changedOrigin = Frame(20);
	changedOrigin["originCode"] = 0;
	CHECK_THROWS(ParseVRTrackedInputFrames(json::array({ Frame(0), changedOrigin })));
}
