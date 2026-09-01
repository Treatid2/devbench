#include "VRInput.h"

#include "EventBus.h"
#include "MainThread.h"
#include "ToolRegistry.h"
#include "VRInputState.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace dvb
{
	namespace
	{
		using namespace std::chrono;

		constexpr int         kVRContractVersion = 1;
		constexpr const char* kDevice = "vrTrackedSet";
		constexpr int         kDefaultTailMs = 50;
		constexpr int         kMaximumTailMs = 1000;

		// Vtable slots from the pinned OpenVR IVRCompositor_022 and IVRSystem_019 ABIs.
		constexpr std::size_t kIVRCompositorWaitGetPosesSlot = 2;
		constexpr std::size_t kIVRCompositorGetLastPosesSlot = 3;
		constexpr std::size_t kIVRSystemGetDeviceToAbsoluteTrackingPoseSlot = 11;
		constexpr std::size_t kIVRSystemGetTrackedDeviceIndexForControllerRoleSlot = 18;
		constexpr std::size_t kIVRSystemGetControllerRoleForTrackedDeviceIndexSlot = 19;
		constexpr std::size_t kIVRSystemGetTrackedDeviceClassSlot = 20;
		constexpr std::size_t kIVRSystemIsTrackedDeviceConnectedSlot = 21;
		constexpr std::size_t kIVRSystemGetControllerStateSlot = 34;
		constexpr std::size_t kIVRSystemGetControllerStateWithPoseSlot = 35;

		std::atomic<bool> g_vrReady{ false };

		bool BooleanArgument(const json& a_args, const char* a_name, bool a_default)
		{
			if (!a_args.contains(a_name))
				return a_default;
			if (!a_args.at(a_name).is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return a_args.at(a_name).get<bool>();
		}

		int IntegerArgument(const json& a_args, const char* a_name, int a_default)
		{
			if (!a_args.contains(a_name))
				return a_default;
			if (!a_args.at(a_name).is_number_integer())
				throw ToolError(400, std::format("'{}' must be an integer", a_name));
			return a_args.at(a_name).get<int>();
		}

		std::string ResolveOwner(const json& a_args, const ToolContext& a_ctx)
		{
			std::string owner;
			if (a_args.contains("owner")) {
				if (!a_args["owner"].is_string())
					throw ToolError(400, "'owner' must be a string");
				owner = a_args["owner"].get<std::string>();
			} else if (!a_ctx.clientId.empty()) {
				owner = "mcp:" + a_ctx.clientId;
			} else {
				owner = "rest:anonymous";
			}
			if (owner.empty() || owner.size() > 128)
				throw ToolError(400, "'owner' must contain 1..128 characters");
			return owner;
		}

		json ContractJson()
		{
			return json{ { "name", "devbench.input.vrTrackedSet" },
				{ "version", json{ { "major", kVRContractVersion }, { "minor", 0 } } } };
		}

		vr::TrackedDevicePose_t OpenVRPose(const VRTrackedPoseState& a_pose)
		{
			vr::TrackedDevicePose_t out{};
			out.bDeviceIsConnected = a_pose.connected;
			out.bPoseIsValid = a_pose.valid;
			out.eTrackingResult = static_cast<vr::ETrackingResult>(a_pose.trackingResult);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					out.mDeviceToAbsoluteTracking.m[row][col] = a_pose.matrix[row * 4 + col];
			for (int i = 0; i < 3; ++i) {
				out.vVelocity.v[i] = a_pose.velocity[i];
				out.vAngularVelocity.v[i] = a_pose.angularVelocity[i];
			}
			return out;
		}

		vr::VRControllerState_t OpenVRController(const VRControllerState& a_state)
		{
			vr::VRControllerState_t out{};
			out.unPacketNum = a_state.packetNumber;
			out.ulButtonPressed = a_state.pressed;
			out.ulButtonTouched = a_state.touched;
			for (std::size_t i = 0; i < vr::k_unControllerStateAxisCount; ++i) {
				out.rAxis[i].x = a_state.axes[i * 2];
				out.rAxis[i].y = a_state.axes[i * 2 + 1];
			}
			return out;
		}

		class VRTrackedSetManager
		{
		public:
			static VRTrackedSetManager& Get()
			{
				// Deliberately process-lifetime: hook thunks and detached bounded playback workers may
				// reference it while Skyrim is shutting down.
				static auto* instance = new VRTrackedSetManager();
				return *instance;
			}

			void SetEvents(EventBus& a_events)
			{
				std::lock_guard lock(m_mutex);
				m_events = &a_events;
			}

			json Status()
			{
				std::lock_guard lock(m_mutex);
				json            out{
					{ "contract", ContractJson() },
					{ "device", kDevice },
					{ "ready", g_vrReady.load(std::memory_order_relaxed) },
					{ "active", m_active },
					{ "starting", m_starting },
					{ "restoring", m_restoring },
					{ "generation", m_generation },
				};
				if (m_active) {
					out["owner"] = m_owner;
					out["surviveLifecycle"] = m_surviveLifecycle;
					out["frameIndex"] = m_frameIndex;
					out["frameCount"] = m_frameCount;
					out["elapsedMs"] = duration_cast<milliseconds>(steady_clock::now() - m_started).count();
					out["durationMs"] = m_durationMs;
					if (m_current)
						out["current"] = json{ { "tMs", m_current->tMs }, { "seq", m_current->seq },
							{ "originCode", m_current->originCode } };
				} else if (!m_lastCompletion.empty()) {
					out["lastCompletion"] = m_lastCompletion;
				}
				return out;
			}

			json Start(const json& a_args, const std::string& a_owner)
			{
				if (!g_vrReady.load(std::memory_order_relaxed))
					throw ToolError(503, "OpenVR tracked-set injection is not ready; Skyrim VR and its OpenVR interfaces must be initialized");
				if (!a_args.contains("frames"))
					throw ToolError(400, "VR tracked-set sequence requires 'frames'");
				std::vector<VRTrackedInputFrame> frames;
				try {
					frames = ParseVRTrackedInputFrames(a_args["frames"]);
				} catch (const std::exception& e) {
					throw ToolError(400, e.what());
				}
				const int tailMs = IntegerArgument(a_args, "tailMs", kDefaultTailMs);
				if (tailMs < 10 || tailMs > kMaximumTailMs)
					throw ToolError(400, std::format("'tailMs' must be between 10 and {}", kMaximumTailMs));
				const bool surviveLifecycle = BooleanArgument(a_args, "surviveLifecycle", false);
				auto*      compositor = RE::BSOpenVR::GetIVRCompositor();
				if (!compositor)
					throw ToolError(503, "OpenVR compositor is unavailable");
				const auto requestedOrigin = static_cast<vr::ETrackingUniverseOrigin>(frames.front().originCode);
				if (compositor->GetTrackingSpace() != requestedOrigin)
					throw ToolError(409, "recorded OpenVR origin does not match the compositor tracking space; select a compatible capture instead of mutating global tracking state");

				std::uint64_t generation;
				const auto    frameCount = frames.size();
				const auto    durationMs = frames.back().tMs + tailMs;
				const auto    started = steady_clock::now();
				{
					std::lock_guard lock(m_mutex);
					if (m_active || m_starting || m_restoring)
						throw ToolError(409, std::format("VR tracked-set sequence is owned by '{}'", m_owner));
					m_starting = true;
					m_owner = a_owner;
					generation = ++m_generation;
				}

				try {
					ApplyControllerIndices(generation, frames.front().left.pose.index,
						frames.front().right.pose.index);
					{
						std::lock_guard lock(m_mutex);
						if (!m_starting || m_generation != generation)
							throw ToolError(409, "VR tracked-set activation was superseded");
						m_starting = false;
						m_active = true;
						m_frameIndex = 0;
						m_frameCount = frameCount;
						m_durationMs = durationMs;
						m_surviveLifecycle = surviveLifecycle;
						m_started = started;
						m_current = frames.front();
						m_lastCompletion = json::object();
					}
					std::thread worker([this, generation, owner = a_owner,
										   frames = std::move(frames), tailMs, started]() mutable {
						Playback(generation, owner, std::move(frames), tailMs, started);
					});
					worker.detach();
				} catch (...) {
					AbortActivation(generation);
					throw;
				}
				Publish("started", json{ { "owner", a_owner }, { "generation", generation },
									   { "frameCount", frameCount }, { "durationMs", durationMs },
									   { "surviveLifecycle", surviveLifecycle } });
				return json{ { "contract", ContractJson() }, { "device", kDevice },
					{ "action", "sequence" }, { "queued", true }, { "owner", a_owner },
					{ "generation", generation }, { "frameCount", frameCount },
					{ "durationMs", durationMs }, { "surviveLifecycle", surviveLifecycle } };
			}

			void StopForLifecycle(std::string_view a_reason)
			{
				std::string   owner;
				std::uint64_t generation = 0;
				json          completion;
				{
					std::lock_guard lock(m_mutex);
					if (!m_active)
						return;
					if (m_surviveLifecycle) {
						logs::info("devbench: preserving owned VR tracked-set sequence '{}' across {}",
							m_owner, a_reason);
						return;
					}
					owner = m_owner;
					generation = m_generation;
					completion = json{ { "owner", owner }, { "generation", generation },
						{ "reason", a_reason }, { "completed", false } };
				}
				Finish(generation, "stopped", std::move(completion));
			}

			json Stop(const std::string& a_owner, bool a_force, std::string_view a_reason)
			{
				std::string   owner;
				std::uint64_t generation = 0;
				json          completion;
				{
					std::lock_guard lock(m_mutex);
					if (!m_active)
						return json{ { "contract", ContractJson() }, { "device", kDevice },
							{ "action", "stop" }, { "stopped", false }, { "notActive", true } };
					if (!a_force && m_owner != a_owner)
						throw ToolError(409, std::format("VR tracked-set sequence is owned by '{}' (not '{}')", m_owner, a_owner));
					owner = m_owner;
					generation = m_generation;
					completion = json{ { "owner", owner }, { "generation", generation },
						{ "reason", a_reason }, { "completed", false } };
				}
				const bool restored = Finish(generation, "stopped", std::move(completion));
				return json{ { "contract", ContractJson() }, { "device", kDevice },
					{ "action", "stop" }, { "stopRequested", true }, { "stopped", restored }, { "owner", owner },
					{ "generation", generation }, { "reason", a_reason },
					{ "restored", restored }, { "restorationPending", !restored } };
			}

			std::optional<VRTrackedInputFrame> Current() const
			{
				std::lock_guard lock(m_mutex);
				return m_active ? m_current : std::nullopt;
			}

		private:
			void Playback(std::uint64_t a_generation, const std::string& a_owner,
				std::vector<VRTrackedInputFrame> a_frames, int a_tailMs,
				steady_clock::time_point a_started)
			{
				for (std::size_t i = 1; i < a_frames.size(); ++i) {
					std::unique_lock lock(m_mutex);
					const auto       due = a_started + milliseconds(a_frames[i].tMs);
					if (m_cv.wait_until(lock, due, [&] { return !m_active || m_generation != a_generation; }))
						return;
					m_current = a_frames[i];
					m_frameIndex = i;
					lock.unlock();
				}
				json completion;
				{
					std::unique_lock lock(m_mutex);
					const auto       due = a_started + milliseconds(a_frames.back().tMs + a_tailMs);
					if (m_cv.wait_until(lock, due, [&] { return !m_active || m_generation != a_generation; }))
						return;
					completion = json{ { "owner", a_owner }, { "generation", a_generation },
						{ "reason", "complete" }, { "completed", true }, { "frameCount", a_frames.size() } };
				}
				Finish(a_generation, "finished", std::move(completion));
			}

			void ApplyControllerIndices(std::uint64_t a_generation, std::uint32_t a_left,
				std::uint32_t a_right)
			{
				MainThread::RunAndWait([this, a_generation, a_left, a_right]() -> json {
					std::lock_guard lock(m_mutex);
					if (!m_starting || m_generation != a_generation)
						return json{ { "applied", false }, { "superseded", true } };
					auto* manager = RE::BSInputDeviceManager::GetSingleton();
					auto* left = manager ? manager->GetVRControllerLeft() : nullptr;
					auto* right = manager ? manager->GetVRControllerRight() : nullptr;
					if (!left || !right)
						throw ToolError(503, "Skyrim VR controller devices are unavailable");
					m_previousLeftIndex = left->GetRuntimeData().trackedDeviceIndex;
					m_previousRightIndex = right->GetRuntimeData().trackedDeviceIndex;
					left->GetRuntimeData().trackedDeviceIndex = a_left;
					right->GetRuntimeData().trackedDeviceIndex = a_right;
					m_indicesApplied = true;
					return json{ { "applied", true } };
				});
			}

			bool RestoreControllerIndices(std::uint64_t a_generation, std::string a_event,
				json a_completion)
			{
				try {
					const json result = MainThread::RunAndWait([this, a_generation,
																   a_event = std::move(a_event), a_completion = std::move(a_completion)]() mutable -> json {
						{
							std::lock_guard lock(m_mutex);
							if (!m_restoring || m_generation != a_generation)
								return json{ { "restored", false }, { "superseded", true } };
							if (m_indicesApplied) {
								auto* manager = RE::BSInputDeviceManager::GetSingleton();
								auto* left = manager ? manager->GetVRControllerLeft() : nullptr;
								auto* right = manager ? manager->GetVRControllerRight() : nullptr;
								if (!left || !right)
									throw ToolError(503, "Skyrim VR controller devices are unavailable during restoration");
								left->GetRuntimeData().trackedDeviceIndex = m_previousLeftIndex;
								right->GetRuntimeData().trackedDeviceIndex = m_previousRightIndex;
								m_indicesApplied = false;
							}
							m_restoring = false;
							a_completion["controllerIndicesRestored"] = true;
							m_lastCompletion = a_completion;
						}
						if (!a_event.empty())
							Publish(a_event.c_str(), std::move(a_completion));
						return json{ { "restored", true } };
					});
					return result.value("restored", false);
				} catch (const std::exception& e) {
					logs::warn("devbench: VR tracked-set controller restoration pending: {}", e.what());
					return false;
				}
			}

			void AbortActivation(std::uint64_t a_generation)
			{
				bool restore = false;
				{
					std::lock_guard lock(m_mutex);
					if (m_generation != a_generation)
						return;
					m_starting = false;
					m_active = false;
					m_current.reset();
					m_restoring = m_indicesApplied;
					restore = m_indicesApplied;
				}
				m_cv.notify_all();
				if (restore)
					RestoreControllerIndices(a_generation, {}, json::object());
			}

			bool Finish(std::uint64_t a_generation, const char* a_event, json a_completion)
			{
				bool restore = false;
				{
					std::lock_guard lock(m_mutex);
					if (m_generation != a_generation)
						return false;
					m_active = false;
					m_current.reset();
					m_restoring = m_indicesApplied;
					restore = m_indicesApplied;
					a_completion["controllerIndicesRestored"] = !restore;
					m_lastCompletion = a_completion;
				}
				m_cv.notify_all();
				if (restore)
					return RestoreControllerIndices(a_generation, a_event, std::move(a_completion));
				a_completion["controllerIndicesRestored"] = true;
				{
					std::lock_guard lock(m_mutex);
					m_lastCompletion = a_completion;
				}
				Publish(a_event, std::move(a_completion));
				return true;
			}

			void Publish(const char* a_state, json a_payload) noexcept
			{
				EventBus* events = nullptr;
				{
					std::lock_guard lock(m_mutex);
					events = m_events;
				}
				if (events)
					try {
						a_payload["state"] = a_state;
						events->Publish("input.vrTrackedSet", std::move(a_payload));
					} catch (const std::exception& e) {
						logs::warn("devbench: VR tracked-set event publish failed: {}", e.what());
					}
			}

			mutable std::mutex                 m_mutex;
			std::condition_variable            m_cv;
			EventBus*                          m_events = nullptr;
			bool                               m_active = false;
			bool                               m_starting = false;
			bool                               m_restoring = false;
			bool                               m_indicesApplied = false;
			std::uint32_t                      m_previousLeftIndex = vr::k_unTrackedDeviceIndexInvalid;
			std::uint32_t                      m_previousRightIndex = vr::k_unTrackedDeviceIndexInvalid;
			bool                               m_surviveLifecycle = false;
			std::string                        m_owner;
			std::uint64_t                      m_generation = 0;
			std::size_t                        m_frameIndex = 0;
			std::size_t                        m_frameCount = 0;
			std::int64_t                       m_durationMs = 0;
			steady_clock::time_point           m_started;
			std::optional<VRTrackedInputFrame> m_current;
			json                               m_lastCompletion = json::object();
		};

		bool CanOverridePoseArray(const VRTrackedInputFrame& a_frame,
			vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			if (a_count == 0)
				return true;
			if (!a_poses)
				return false;
			return a_frame.hmd.index < a_count && a_frame.left.pose.index < a_count &&
			       a_frame.right.pose.index < a_count;
		}

		void OverridePoseArray(const VRTrackedInputFrame& a_frame,
			vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			if (a_count == 0)
				return;
			const auto apply = [&](const VRTrackedPoseState& a_pose) {
				if (a_pose.index < a_count)
					a_poses[a_pose.index] = OpenVRPose(a_pose);
			};
			apply(a_frame.hmd);
			apply(a_frame.left.pose);
			apply(a_frame.right.pose);
		}

		using WaitGetPosesFn = vr::EVRCompositorError (*)(vr::IVRCompositor*,
			vr::TrackedDevicePose_t*, std::uint32_t, vr::TrackedDevicePose_t*, std::uint32_t);
		using GetTrackingPoseFn = void (*)(vr::IVRSystem*, vr::ETrackingUniverseOrigin, float,
			vr::TrackedDevicePose_t*, std::uint32_t);
		using RoleIndexFn = vr::TrackedDeviceIndex_t (*)(vr::IVRSystem*, vr::ETrackedControllerRole);
		using IndexRoleFn = vr::ETrackedControllerRole (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using DeviceClassFn = vr::ETrackedDeviceClass (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using ConnectedFn = bool (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t);
		using ControllerStateFn = bool (*)(vr::IVRSystem*, vr::TrackedDeviceIndex_t,
			vr::VRControllerState_t*, std::uint32_t);
		using ControllerStatePoseFn = bool (*)(vr::IVRSystem*, vr::ETrackingUniverseOrigin,
			vr::TrackedDeviceIndex_t, vr::VRControllerState_t*, std::uint32_t,
			vr::TrackedDevicePose_t*);

		REL::Relocation<WaitGetPosesFn>        g_waitGetPoses;
		REL::Relocation<WaitGetPosesFn>        g_getLastPoses;
		REL::Relocation<GetTrackingPoseFn>     g_getTrackingPose;
		REL::Relocation<RoleIndexFn>           g_roleIndex;
		REL::Relocation<IndexRoleFn>           g_indexRole;
		REL::Relocation<DeviceClassFn>         g_deviceClass;
		REL::Relocation<ConnectedFn>           g_connected;
		REL::Relocation<ControllerStateFn>     g_controllerState;
		REL::Relocation<ControllerStatePoseFn> g_controllerStatePose;

		VRTrackedPoseState ReadPose(const vr::TrackedDevicePose_t& a_pose,
			vr::TrackedDeviceIndex_t                               a_index)
		{
			VRTrackedPoseState out;
			out.available = a_index != vr::k_unTrackedDeviceIndexInvalid;
			if (!out.available)
				return out;
			out.index = a_index;
			out.connected = a_pose.bDeviceIsConnected;
			out.valid = a_pose.bPoseIsValid;
			out.trackingResult = static_cast<std::int32_t>(a_pose.eTrackingResult);
			for (int row = 0; row < 3; ++row)
				for (int col = 0; col < 4; ++col)
					out.matrix[row * 4 + col] = a_pose.mDeviceToAbsoluteTracking.m[row][col];
			for (int i = 0; i < 3; ++i) {
				out.velocity[i] = a_pose.vVelocity.v[i];
				out.angularVelocity[i] = a_pose.vAngularVelocity.v[i];
			}
			return out;
		}

		VRControllerState ReadController(vr::IVRSystem* a_system,
			vr::TrackedDeviceIndex_t                    a_index)
		{
			VRControllerState out;
			if (a_index == vr::k_unTrackedDeviceIndexInvalid)
				return out;
			vr::VRControllerState_t state{};
			if (!g_controllerState(a_system, a_index, &state, sizeof(state)))
				return out;
			out.packetNumber = state.unPacketNum;
			out.pressed = state.ulButtonPressed;
			out.touched = state.ulButtonTouched;
			for (std::size_t i = 0; i < 5; ++i) {
				out.axes[i * 2] = state.rAxis[i].x;
				out.axes[i * 2 + 1] = state.rAxis[i].y;
			}
			return out;
		}

		json ObservePhysicalTrackedSet()
		{
			if (!g_vrReady.load(std::memory_order_relaxed))
				throw ToolError(503, "OpenVR tracked-set observation is not ready");
			auto* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			if (!system)
				throw ToolError(503, "OpenVR system is unavailable");

			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor && openvr)
				compositor = openvr->vrContext.vrCompositor;
			const auto                                                         origin = compositor ? compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
			std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
			g_getTrackingPose(system, origin, 0.0f, poses.data(), static_cast<std::uint32_t>(poses.size()));
			const auto leftIndex = g_roleIndex(system, vr::TrackedControllerRole_LeftHand);
			const auto rightIndex = g_roleIndex(system, vr::TrackedControllerRole_RightHand);
			const auto poseFor = [&](vr::TrackedDeviceIndex_t a_index) {
				return a_index < poses.size() ? ReadPose(poses[a_index], a_index) : VRTrackedPoseState{};
			};

			VRTrackedInputFrame frame;
			frame.tMs = 0;
			frame.seq = 1;
			frame.originCode = static_cast<std::int32_t>(origin);
			frame.hmd = poseFor(vr::k_unTrackedDeviceIndex_Hmd);
			frame.left.pose = poseFor(leftIndex);
			frame.left.controller = ReadController(system, leftIndex);
			frame.right.pose = poseFor(rightIndex);
			frame.right.controller = ReadController(system, rightIndex);
			return json{ { "contract", ContractJson() }, { "device", kDevice },
				{ "action", "observe" }, { "source", "physical_openvr" },
				{ "frame", VRTrackedInputFrameJson(frame) } };
		}

		vr::EVRCompositorError WaitGetPosesHook(vr::IVRCompositor* a_self,
			vr::TrackedDevicePose_t* a_render, std::uint32_t a_renderCount,
			vr::TrackedDevicePose_t* a_game, std::uint32_t a_gameCount)
		{
			const auto result = g_waitGetPoses(a_self, a_render, a_renderCount, a_game, a_gameCount);
			if (result != vr::VRCompositorError_None)
				return result;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame || a_self->GetTrackingSpace() != static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) ||
				!CanOverridePoseArray(*frame, a_render, a_renderCount) ||
				!CanOverridePoseArray(*frame, a_game, a_gameCount))
				return result;
			OverridePoseArray(*frame, a_render, a_renderCount);
			OverridePoseArray(*frame, a_game, a_gameCount);
			return vr::VRCompositorError_None;
		}

		vr::EVRCompositorError GetLastPosesHook(vr::IVRCompositor* a_self,
			vr::TrackedDevicePose_t* a_render, std::uint32_t a_renderCount,
			vr::TrackedDevicePose_t* a_game, std::uint32_t a_gameCount)
		{
			const auto result = g_getLastPoses(a_self, a_render, a_renderCount, a_game, a_gameCount);
			if (result != vr::VRCompositorError_None)
				return result;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame || a_self->GetTrackingSpace() != static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) ||
				!CanOverridePoseArray(*frame, a_render, a_renderCount) ||
				!CanOverridePoseArray(*frame, a_game, a_gameCount))
				return result;
			OverridePoseArray(*frame, a_render, a_renderCount);
			OverridePoseArray(*frame, a_game, a_gameCount);
			return vr::VRCompositorError_None;
		}

		void GetTrackingPoseHook(vr::IVRSystem* a_self, vr::ETrackingUniverseOrigin a_origin,
			float a_prediction, vr::TrackedDevicePose_t* a_poses, std::uint32_t a_count)
		{
			g_getTrackingPose(a_self, a_origin, a_prediction, a_poses, a_count);
			const auto frame = VRTrackedSetManager::Get().Current();
			if (frame && a_origin == static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) &&
				CanOverridePoseArray(*frame, a_poses, a_count))
				OverridePoseArray(*frame, a_poses, a_count);
		}

		vr::TrackedDeviceIndex_t RoleIndexHook(vr::IVRSystem* a_self, vr::ETrackedControllerRole a_role)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if (a_role == vr::TrackedControllerRole_LeftHand && frame->left.pose.available)
					return frame->left.pose.index;
				if (a_role == vr::TrackedControllerRole_RightHand && frame->right.pose.available)
					return frame->right.pose.index;
			}
			return g_roleIndex(a_self, a_role);
		}

		vr::ETrackedControllerRole IndexRoleHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if (frame->left.pose.available && a_index == frame->left.pose.index)
					return vr::TrackedControllerRole_LeftHand;
				if (frame->right.pose.available && a_index == frame->right.pose.index)
					return vr::TrackedControllerRole_RightHand;
			}
			return g_indexRole(a_self, a_index);
		}

		vr::ETrackedDeviceClass DeviceClassHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				if ((frame->hmd.available && a_index == frame->hmd.index))
					return vr::TrackedDeviceClass_HMD;
				if ((frame->left.pose.available && a_index == frame->left.pose.index) ||
					(frame->right.pose.available && a_index == frame->right.pose.index))
					return vr::TrackedDeviceClass_Controller;
			}
			return g_deviceClass(a_self, a_index);
		}

		bool ConnectedHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index)
		{
			if (const auto frame = VRTrackedSetManager::Get().Current()) {
				for (const auto* pose : { &frame->hmd, &frame->left.pose, &frame->right.pose })
					if (pose->available && a_index == pose->index)
						return pose->connected;
			}
			return g_connected(a_self, a_index);
		}

		bool SyntheticController(vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state,
			std::uint32_t a_size, vr::TrackedDevicePose_t* a_pose)
		{
			if (!a_state)
				return false;
			const auto frame = VRTrackedSetManager::Get().Current();
			if (!frame)
				return false;
			const VRTrackedDeviceState* device = nullptr;
			if (frame->left.pose.available && a_index == frame->left.pose.index)
				device = &frame->left;
			else if (frame->right.pose.available && a_index == frame->right.pose.index)
				device = &frame->right;
			if (!device)
				return false;
			const auto state = OpenVRController(device->controller);
			std::memcpy(a_state, &state, std::min<std::size_t>(a_size, sizeof(state)));
			if (a_pose)
				*a_pose = OpenVRPose(device->pose);
			return true;
		}

		bool ControllerStateHook(vr::IVRSystem* a_self, vr::TrackedDeviceIndex_t a_index,
			vr::VRControllerState_t* a_state, std::uint32_t a_size)
		{
			return SyntheticController(a_index, a_state, a_size, nullptr) ||
			       g_controllerState(a_self, a_index, a_state, a_size);
		}

		bool ControllerStatePoseHook(vr::IVRSystem* a_self, vr::ETrackingUniverseOrigin a_origin,
			vr::TrackedDeviceIndex_t a_index, vr::VRControllerState_t* a_state,
			std::uint32_t a_size, vr::TrackedDevicePose_t* a_pose)
		{
			const auto frame = VRTrackedSetManager::Get().Current();
			if (frame && a_origin == static_cast<vr::ETrackingUniverseOrigin>(frame->originCode) &&
				SyntheticController(a_index, a_state, a_size, a_pose))
				return true;
			return g_controllerStatePose(a_self, a_origin, a_index, a_state, a_size, a_pose);
		}

		bool InstallHooks()
		{
			if (!REL::Module::IsVR())
				return false;
			auto* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor && openvr)
				compositor = openvr->vrContext.vrCompositor;
			if (!system || !compositor)
				return false;

			REL::Relocation<std::uintptr_t> compositorVtable{ *reinterpret_cast<std::uintptr_t*>(compositor) };
			g_waitGetPoses = compositorVtable.write_vfunc(kIVRCompositorWaitGetPosesSlot, WaitGetPosesHook);
			g_getLastPoses = compositorVtable.write_vfunc(kIVRCompositorGetLastPosesSlot, GetLastPosesHook);

			REL::Relocation<std::uintptr_t> systemVtable{ *reinterpret_cast<std::uintptr_t*>(system) };
			g_getTrackingPose = systemVtable.write_vfunc(kIVRSystemGetDeviceToAbsoluteTrackingPoseSlot, GetTrackingPoseHook);
			g_roleIndex = systemVtable.write_vfunc(kIVRSystemGetTrackedDeviceIndexForControllerRoleSlot, RoleIndexHook);
			g_indexRole = systemVtable.write_vfunc(kIVRSystemGetControllerRoleForTrackedDeviceIndexSlot, IndexRoleHook);
			g_deviceClass = systemVtable.write_vfunc(kIVRSystemGetTrackedDeviceClassSlot, DeviceClassHook);
			g_connected = systemVtable.write_vfunc(kIVRSystemIsTrackedDeviceConnectedSlot, ConnectedHook);
			g_controllerState = systemVtable.write_vfunc(kIVRSystemGetControllerStateSlot, ControllerStateHook);
			g_controllerStatePose = systemVtable.write_vfunc(kIVRSystemGetControllerStateWithPoseSlot, ControllerStatePoseHook);
			return true;
		}
	}

	json VRInputCapabilities()
	{
		return json{
			{ "version", json{ { "major", kVRContractVersion }, { "minor", 0 } } },
			{ "available", REL::Module::IsVR() },
			{ "ready", g_vrReady.load(std::memory_order_relaxed) },
			{ "device", kDevice },
			{ "atomicDevices", json::array({ "hmd", "left", "right" }) },
			{ "injection", "OpenVR IVRCompositor poses + IVRSystem controller state" },
			{ "actions", json::array({ "status", "observe", "sequence", "stop", "releaseAll" }) },
			{ "timing", "steadyMillisecondsFromSequenceStart" },
			{ "ordering", "tMs then seq" },
			{ "poseEncoding", "OpenVR device-to-absolute 3x4 row-major" },
			{ "controllerEncoding", "OpenVR packetNumber/pressed/touched/five x-y axes" },
			{ "validation", "complete sequence before activation" },
			{ "ownership", "one bounded sequence owner; cleanup always stops the whole set" },
			{ "lifecyclePolicy", "default stop; recording replay may explicitly survive recorded load/new-game boundaries" },
			{ "passThroughWhenInactive", true },
			{ "maximumFrames", kMaximumVRTrackedFrames },
			{ "maximumDurationMs", kMaximumVRTrackedDurationMs },
		};
	}

	bool IsVRInputDevice(std::string_view a_device)
	{
		return a_device == kDevice;
	}

	json HandleVRInput(const json& a_args, const ToolContext& a_ctx)
	{
		try {
			if (a_args.contains("action") && !a_args["action"].is_string())
				throw ToolError(400, "'action' must be a string");
			const std::string action = a_args.value("action", std::string("status"));
			if (action == "status")
				return VRTrackedSetManager::Get().Status();
			if (action == "observe")
				return ObservePhysicalTrackedSet();
			const std::string owner = ResolveOwner(a_args, a_ctx);
			if (action == "sequence")
				return VRTrackedSetManager::Get().Start(a_args, owner);
			if (action == "stop" || action == "releaseAll")
				return VRTrackedSetManager::Get().Stop(owner, BooleanArgument(a_args, "all", false),
					action == "releaseAll" ? "releaseAll" : "request");
			throw ToolError(400, std::format("unknown vrTrackedSet action '{}' (status|observe|sequence|stop|releaseAll)", action));
		} catch (const json::exception& e) {
			throw ToolError(400, std::format("invalid VR tracked-set JSON: {}", e.what()));
		}
	}

	void MarkVRInputReady(EventBus& a_events)
	{
		VRTrackedSetManager::Get().SetEvents(a_events);
		const bool ready = InstallHooks();
		g_vrReady.store(ready, std::memory_order_relaxed);
		if (ready)
			logs::info("devbench: atomic HMD/controller input API ready (contract v{}, OpenVR pass-through hooks)", kVRContractVersion);
		else if (REL::Module::IsVR())
			logs::warn("devbench: atomic HMD/controller input API unavailable (OpenVR interfaces not initialized)");
	}

	void ReleaseVRInputForLifecycle(const char* a_reason)
	{
		if (!g_vrReady.load(std::memory_order_relaxed))
			return;
		const std::string reason = a_reason ? a_reason : "lifecycle";
		std::thread([reason]() {
			try {
				VRTrackedSetManager::Get().StopForLifecycle(reason);
			} catch (const std::exception& e) {
				logs::warn("devbench: lifecycle VR tracked-set release failed ({}): {}", reason, e.what());
			}
		}).detach();
	}
}
