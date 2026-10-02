#include "test_framework.h"

#include "KeyboardButtonEvent.h"

#include <type_traits>

namespace
{
	enum class Device { kKeyboard, kMouse };

	struct RecordingQueue
	{
		int           calls = 0;
		Device        device = Device::kMouse;
		std::int32_t  noWandIndex = 0;
		std::uint32_t scancode = 0;
		float         value = -1.0F;
		float         heldSeconds = -1.0F;

		template <class Index>
		void AddButtonEvent(Device a_device, Index a_index, std::uint32_t a_scancode,
			float a_value, float a_heldSeconds)
		{
			static_assert(std::is_same_v<Index, std::int32_t>, "no-wand field must remain signed int32");
			++calls;
			device = a_device;
			noWandIndex = a_index;
			scancode = a_scancode;
			value = a_value;
			heldSeconds = a_heldSeconds;
		}
	};
}

TEST_CASE("keyboard initial enqueue preserves device and scancode with signed no-wand parity")
{
	RecordingQueue queue;
	dvb::EnqueueKeyboardButton(queue, Device::kKeyboard, 0x1C, true, 0.0F);
	CHECK(queue.calls == 1);
	CHECK(queue.device == Device::kKeyboard);
	CHECK(queue.noWandIndex == -1);
	CHECK(queue.scancode == 0x1C);
	CHECK(queue.value == 1.0F);
	CHECK(queue.heldSeconds == 0.0F);
}

TEST_CASE("keyboard repeated enqueue preserves signed no-wand parity and increasing duration")
{
	RecordingQueue queue;
	dvb::EnqueueKeyboardButton(queue, Device::kKeyboard, 0xC8, true, 0.25F);
	CHECK(queue.calls == 1);
	CHECK(queue.device == Device::kKeyboard);
	CHECK(queue.noWandIndex == -1);
	CHECK(queue.scancode == 0xC8);
	CHECK(queue.value == 1.0F);
	CHECK(queue.heldSeconds == 0.25F);
	dvb::EnqueueKeyboardButton(queue, Device::kKeyboard, 0xC8, true, 0.75F);
	CHECK(queue.calls == 2);
	CHECK(queue.device == Device::kKeyboard);
	CHECK(queue.noWandIndex == -1);
	CHECK(queue.scancode == 0xC8);
	CHECK(queue.value == 1.0F);
	CHECK(queue.heldSeconds == 0.75F);
}

TEST_CASE("keyboard release enqueue preserves signed no-wand parity and measured duration")
{
	RecordingQueue queue;
	dvb::EnqueueKeyboardButton(queue, Device::kKeyboard, 0xAA, false, 1.25F);
	CHECK(queue.calls == 1);
	CHECK(queue.device == Device::kKeyboard);
	CHECK(queue.noWandIndex == -1);
	CHECK(queue.scancode == 0xAA);
	CHECK(queue.value == 0.0F);
	CHECK(queue.heldSeconds == 1.25F);
}
