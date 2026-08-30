#include "VRInputState.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>

namespace dvb
{
	namespace
	{
		constexpr std::size_t kMaximumFrames = 60000;
		constexpr auto        kMaximumDurationMs = 30 * 60 * 1000;

		template <std::size_t N>
		std::array<float, N> FloatArray(const json& a_parent, const char* a_name,
			bool a_required = true)
		{
			std::array<float, N> out{};
			if (!a_parent.contains(a_name)) {
				if (a_required)
					throw std::invalid_argument(std::format("missing '{}'", a_name));
				return out;
			}
			const auto& value = a_parent.at(a_name);
			if (!value.is_array() || value.size() != N)
				throw std::invalid_argument(std::format("'{}' must contain exactly {} numbers", a_name, N));
			for (std::size_t i = 0; i < N; ++i) {
				if (!value[i].is_number())
					throw std::invalid_argument(std::format("'{}[{}]' must be a number", a_name, i));
				const double number = value[i].get<double>();
				if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
					number > std::numeric_limits<float>::max())
					throw std::invalid_argument(std::format("'{}[{}]' must be finite", a_name, i));
				out[i] = static_cast<float>(number);
			}
			return out;
		}

		VRTrackedPoseState Pose(const json& a_value, const char* a_role)
		{
			if (!a_value.is_object())
				throw std::invalid_argument(std::format("'{}' must be an object", a_role));
			VRTrackedPoseState out;
			out.available = a_value.value("available", false);
			out.connected = a_value.value("connected", out.available);
			out.valid = a_value.value("valid", false);
			if (!out.available) {
				if (out.connected || out.valid)
					throw std::invalid_argument(std::format("'{}' cannot be connected/valid when unavailable", a_role));
				return out;
			}
			if (!a_value.contains("index") || !a_value["index"].is_number_integer())
				throw std::invalid_argument(std::format("'{}.index' must be an unsigned OpenVR device index", a_role));
			const auto index = a_value["index"].get<std::int64_t>();
			if (index < 0 || index >= 64)
				throw std::invalid_argument(std::format("'{}.index' must be below 64", a_role));
			out.index = static_cast<std::uint32_t>(index);
			out.trackingResult = a_value.value("trackingResult", 0);
			if (out.trackingResult < 0 || out.trackingResult > 300)
				throw std::invalid_argument(std::format("'{}.trackingResult' is outside the OpenVR enum range", a_role));
			out.velocity = FloatArray<3>(a_value, "velocity", false);
			out.angularVelocity = FloatArray<3>(a_value, "angularVelocity", false);
			if (out.valid)
				out.matrix = FloatArray<12>(a_value, "matrix");
			return out;
		}

		VRTrackedDeviceState Controller(const json& a_value, const char* a_role)
		{
			VRTrackedDeviceState out;
			out.pose = Pose(a_value, a_role);
			const json state = a_value.value("controller", json::object());
			if (!state.is_object())
				throw std::invalid_argument(std::format("'{}.controller' must be an object", a_role));
			out.controller.packetNumber = state.value("packetNumber", 0u);
			out.controller.pressed = state.value("pressed", std::uint64_t{ 0 });
			out.controller.touched = state.value("touched", std::uint64_t{ 0 });
			if (state.contains("axes")) {
				const auto& axes = state["axes"];
				if (!axes.is_array() || axes.size() != 5)
					throw std::invalid_argument(std::format("'{}.controller.axes' must contain five [x,y] pairs", a_role));
				for (std::size_t i = 0; i < 5; ++i) {
					if (!axes[i].is_array() || axes[i].size() != 2 ||
						!axes[i][0].is_number() || !axes[i][1].is_number())
						throw std::invalid_argument(std::format("'{}.controller.axes[{}]' must be [x,y]", a_role, i));
					const double x = axes[i][0].get<double>();
					const double y = axes[i][1].get<double>();
					if (!std::isfinite(x) || !std::isfinite(y) || x < -1.001 || x > 1.001 || y < -1.001 || y > 1.001)
						throw std::invalid_argument(std::format("'{}.controller.axes[{}]' must be finite values in [-1,1]", a_role, i));
					out.controller.axes[i * 2] = static_cast<float>(x);
					out.controller.axes[i * 2 + 1] = static_cast<float>(y);
				}
			}
			return out;
		}

		json PoseJson(const VRTrackedPoseState& a_pose)
		{
			json out{
				{ "available", a_pose.available },
				{ "connected", a_pose.connected },
				{ "valid", a_pose.valid },
			};
			if (!a_pose.available)
				return out;
			out["index"] = a_pose.index;
			out["trackingResult"] = a_pose.trackingResult;
			out["velocity"] = a_pose.velocity;
			out["angularVelocity"] = a_pose.angularVelocity;
			if (a_pose.valid)
				out["matrix"] = a_pose.matrix;
			return out;
		}
	}

	std::vector<VRTrackedInputFrame> ParseVRTrackedInputFrames(const json& a_frames)
	{
		if (!a_frames.is_array() || a_frames.empty() || a_frames.size() > kMaximumFrames)
			throw std::invalid_argument(std::format("'frames' must contain 1..{} atomic VR frames", kMaximumFrames));
		std::vector<VRTrackedInputFrame> out;
		out.reserve(a_frames.size());
		std::int64_t                 previous = -1;
		std::optional<std::uint32_t> leftIndex;
		std::optional<std::uint32_t> rightIndex;
		std::optional<std::int32_t>  originCode;
		for (std::size_t i = 0; i < a_frames.size(); ++i) {
			const auto& item = a_frames[i];
			if (!item.is_object())
				throw std::invalid_argument(std::format("frames[{}] must be an object", i));
			VRTrackedInputFrame frame;
			if (!item.contains("tMs") || !item["tMs"].is_number_integer())
				throw std::invalid_argument(std::format("frames[{}].tMs must be an integer", i));
			frame.tMs = item["tMs"].get<std::int64_t>();
			if (frame.tMs < 0 || frame.tMs > kMaximumDurationMs || frame.tMs < previous)
				throw std::invalid_argument(std::format("frames[{}].tMs must be monotonic in [0,{}]", i, kMaximumDurationMs));
			if (i == 0 && frame.tMs != 0)
				throw std::invalid_argument("frames[0].tMs must be 0 so the complete tracked set is defined immediately");
			previous = frame.tMs;
			frame.seq = item.value("seq", static_cast<std::uint64_t>(i + 1));
			frame.originCode = item.value("originCode", 1);
			if (frame.originCode < 0 || frame.originCode > 2)
				throw std::invalid_argument(std::format("frames[{}].originCode must be 0, 1, or 2", i));
			if (originCode && frame.originCode != *originCode)
				throw std::invalid_argument(std::format("frames[{}].originCode changed within one atomic sequence", i));
			originCode = frame.originCode;
			if (!item.contains("hmd") || !item.contains("left") || !item.contains("right"))
				throw std::invalid_argument(std::format("frames[{}] must contain hmd, left, and right", i));
			frame.hmd = Pose(item["hmd"], "hmd");
			frame.left = Controller(item["left"], "left");
			frame.right = Controller(item["right"], "right");
			if (frame.hmd.available && frame.hmd.index != 0)
				throw std::invalid_argument(std::format("frames[{}].hmd.index must be OpenVR HMD index 0", i));
			if (frame.left.pose.available && frame.right.pose.available &&
				frame.left.pose.index == frame.right.pose.index)
				throw std::invalid_argument(std::format("frames[{}] left/right indices must be distinct", i));
			const auto stableRoleIndex = [i](const char* a_role, const VRTrackedPoseState& a_pose,
											 std::optional<std::uint32_t>& a_index) {
				if (!a_pose.available)
					return;
				if (a_index && a_pose.index != *a_index)
					throw std::invalid_argument(std::format("frames[{}].{}.index changed within one atomic sequence", i, a_role));
				a_index = a_pose.index;
			};
			stableRoleIndex("left", frame.left.pose, leftIndex);
			stableRoleIndex("right", frame.right.pose, rightIndex);
			out.push_back(std::move(frame));
		}
		return out;
	}

	json VRTrackedInputFrameJson(const VRTrackedInputFrame& a_frame)
	{
		json       left = PoseJson(a_frame.left.pose);
		json       right = PoseJson(a_frame.right.pose);
		const auto controllerJson = [](const VRControllerState& a_controller) {
			json axes = json::array();
			for (std::size_t i = 0; i < 5; ++i)
				axes.push_back(json::array({ a_controller.axes[i * 2], a_controller.axes[i * 2 + 1] }));
			return json{ { "packetNumber", a_controller.packetNumber },
				{ "pressed", a_controller.pressed }, { "touched", a_controller.touched },
				{ "axes", std::move(axes) } };
		};
		left["controller"] = controllerJson(a_frame.left.controller);
		right["controller"] = controllerJson(a_frame.right.controller);
		return json{ { "tMs", a_frame.tMs }, { "seq", a_frame.seq },
			{ "originCode", a_frame.originCode }, { "hmd", PoseJson(a_frame.hmd) },
			{ "left", std::move(left) }, { "right", std::move(right) } };
	}
}
