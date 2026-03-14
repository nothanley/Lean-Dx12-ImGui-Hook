#include "user_overlay.hpp"

#include "imgui.h"

void user_overlay::render(const frame_context &ctx)
{
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("My Overlay");
	ImGui::Text("Backend ready. Draw your UI here.");
	ImGui::Text("Resolution: %u x %u", ctx.width, ctx.height);
	ImGui::Text("Delta: %.4f", ctx.delta_time);
	ImGui::End();
}
