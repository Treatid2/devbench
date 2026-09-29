#pragma once

namespace dvb::CameraOrbit
{
	// Holds the gameplay third-person camera at an angle round the player (heading, tilt, zoom and offset re-applied on
	// every camera update) so a front or side view needs no free camera. The player keeps the gameplay input context,
	// so held input (a charging attack or spell) keeps acting on the player.
	void Install();

	// a_yawRad: heading offset from the player's facing (pi = in front). a_pitchRad below -9 leaves the tilt alone;
	// a_zoom outside [-1, 1] leaves the zoom alone. Turning the orbit off releases the held facing.
	void Set(bool a_on, float a_yawRad, float a_pitchRad, float a_zoom);
	void SetOffset(bool a_set, float a_right, float a_up);
	void Stop();
	bool Active();
}
