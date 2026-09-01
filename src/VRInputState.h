#pragma once

#include "Json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvb
{
	inline constexpr std::size_t  kMaximumVRTrackedFrames = 60000;
	inline constexpr std::int64_t kMaximumVRTrackedDurationMs = 30 * 60 * 1000;

	struct VRTrackedPoseState
	{
		bool                  available = false;
		bool                  connected = false;
		bool                  valid = false;
		std::uint32_t         index = 0;
		std::int32_t          trackingResult = 0;
		std::array<float, 12> matrix{};
		std::array<float, 3>  velocity{};
		std::array<float, 3>  angularVelocity{};
	};

	struct VRControllerState
	{
		std::uint32_t         packetNumber = 0;
		std::uint64_t         pressed = 0;
		std::uint64_t         touched = 0;
		std::array<float, 10> axes{};  // five OpenVR x/y axis pairs
	};

	struct VRTrackedDeviceState
	{
		VRTrackedPoseState pose;
		VRControllerState  controller;
	};

	struct VRTrackedInputFrame
	{
		std::int64_t         tMs = 0;
		std::uint64_t        seq = 0;
		std::int32_t         originCode = 1;  // OpenVR standing
		VRTrackedPoseState   hmd;
		VRTrackedDeviceState left;
		VRTrackedDeviceState right;
	};

	// Parse and completely validate an atomic HMD + left-controller + right-controller sequence.
	// Throws std::invalid_argument before any game-facing mutation can occur.
	std::vector<VRTrackedInputFrame> ParseVRTrackedInputFrames(const json& a_frames);

	// Canonical JSON used by status/events and by host-independent round-trip tests.
	json VRTrackedInputFrameJson(const VRTrackedInputFrame& a_frame);
}
