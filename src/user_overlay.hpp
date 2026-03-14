#pragma once

namespace user_overlay
{
	struct frame_context
	{
		float delta_time = 0.0f;
		unsigned int width = 0;
		unsigned int height = 0;
	};

	// Called once per frame after ImGui backend setup/new-frame is done.
	// Put all of your menu/window code in the implementation file.
	void render(const frame_context &ctx);
}
