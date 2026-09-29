#include "CameraOrbit.h"

#include <RE/P/PlayerCharacter.h>
#include <RE/T/ThirdPersonState.h>

#include <atomic>

namespace dvb::CameraOrbit
{
	namespace
	{
		std::atomic<bool>  g_on{ false };
		std::atomic<float> g_yaw{ 0.0f };
		std::atomic<float> g_pitch{ 0.0f };
		std::atomic<bool>  g_pitchSet{ false };
		std::atomic<float> g_zoom{ 0.0f };
		std::atomic<bool>  g_zoomSet{ false };
		std::atomic<bool>  g_offsetSet{ false };
		std::atomic<float> g_right{ 0.0f };
		std::atomic<float> g_up{ 0.0f };
		std::atomic<bool>  g_haveBase{ false };
		std::atomic<float> g_baseYaw{ 0.0f };
		std::atomic<float> g_basePitch{ 0.0f };
		std::atomic<bool>  g_restorePitch{ false };

		// Runs on the main thread inside ThirdPersonState::Update. currentYaw / targetYaw are the camera's own heading.
		// The player's facing is captured once per orbit and held: with a weapon or spell drawn and no attack held, the
		// player otherwise turns to face wherever the camera looks, and a camera in front would chase the player round.
		void Apply(RE::ThirdPersonState* a_state)
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc)
				return;
			if (!g_haveBase.load(std::memory_order_acquire)) {
				g_baseYaw.store(pc->data.angle.z, std::memory_order_relaxed);
				g_basePitch.store(pc->data.angle.x, std::memory_order_relaxed);
				g_haveBase.store(true, std::memory_order_release);
			}
			const float base = g_baseYaw.load(std::memory_order_relaxed);
			pc->data.angle.z = base;
			const float yaw = base + g_yaw.load(std::memory_order_relaxed);
			a_state->targetYaw = yaw;
			a_state->currentYaw = yaw;
			if (g_offsetSet.load(std::memory_order_relaxed)) {
				const RE::NiPoint3 off{ g_right.load(std::memory_order_relaxed), 0.0f, g_up.load(std::memory_order_relaxed) };
				a_state->posOffsetExpected = off;
				a_state->posOffsetActual = off;
			}
			// the third-person tilt follows the player's look pitch (radians, positive looks down)
			if (g_pitchSet.load(std::memory_order_relaxed)) {
				pc->data.angle.x = g_pitch.load(std::memory_order_relaxed);
				g_restorePitch.store(true, std::memory_order_relaxed);
			}
			if (g_zoomSet.load(std::memory_order_relaxed)) {
				const float zoom = g_zoom.load(std::memory_order_relaxed);
				a_state->targetZoomOffset = zoom;
				a_state->currentZoomOffset = zoom;
			}
		}

		struct Update
		{
			static void thunk(RE::ThirdPersonState* a_state, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
			{
				const bool on = g_on.load(std::memory_order_relaxed);
				if (on)
					Apply(a_state);
				else if (g_restorePitch.exchange(false, std::memory_order_relaxed)) {
					// the orbit tilted the player's look pitch; hand the player back the pitch it had before
					if (auto* pc = RE::PlayerCharacter::GetSingleton())
						pc->data.angle.x = g_basePitch.load(std::memory_order_relaxed);
				}
				func(a_state, a_next);
				if (on)
					Apply(a_state);  // the update itself resets the heading behind a drawn weapon
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ThirdPersonState[0] };
		Update::func = vtbl.write_vfunc(REL::Module::IsVR() ? 0x04 : 0x03, Update::thunk);
	}

	void SetOffset(bool a_set, float a_right, float a_up)
	{
		g_right.store(a_right, std::memory_order_relaxed);
		g_up.store(a_up, std::memory_order_relaxed);
		g_offsetSet.store(a_set, std::memory_order_relaxed);
	}

	void Set(bool a_on, float a_yawRad, float a_pitchRad, float a_zoom)
	{
		g_yaw.store(a_yawRad, std::memory_order_relaxed);
		g_pitch.store(a_pitchRad, std::memory_order_relaxed);
		g_pitchSet.store(a_pitchRad > -9.0f, std::memory_order_relaxed);
		g_zoom.store(a_zoom, std::memory_order_relaxed);
		g_zoomSet.store(a_zoom >= -1.0f && a_zoom <= 1.0f, std::memory_order_relaxed);
		if (!a_on || !g_on.load(std::memory_order_relaxed))
			g_haveBase.store(false, std::memory_order_release);  // a new orbit captures the facing again
		g_on.store(a_on, std::memory_order_release);
	}

	void Stop()
	{
		g_on.store(false, std::memory_order_release);
		g_restorePitch.store(false, std::memory_order_relaxed);  // a pitch from the old game is never written into the new one
		g_offsetSet.store(false, std::memory_order_relaxed);
		g_haveBase.store(false, std::memory_order_release);
	}

	bool Active() { return g_on.load(std::memory_order_acquire); }
}
