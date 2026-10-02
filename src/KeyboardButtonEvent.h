#pragma once

#include <cstdint>

namespace dvb
{
	// Internal engine-enqueue seam, shared by press/release and held-repeat paths.
	// The device is supplied by the engine-facing caller so this header also builds
	// with a host-independent recording queue; no engine ABI is duplicated here.
	template <class Queue, class Device>
	void EnqueueKeyboardButton(Queue& a_queue, Device a_keyboardDevice,
		std::uint32_t a_scancode, bool a_down, float a_heldSeconds)
	{
		// Keyboard button events have no VR wand association. Keep this signed.
		constexpr std::int32_t noWandIndex = -1;
		a_queue.AddButtonEvent(a_keyboardDevice, noWandIndex, a_scancode,
			a_down ? 1.0F : 0.0F, a_heldSeconds);
	}
}
