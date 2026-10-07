// ReShade add-on half: uploads Minecraft's latest frame (shared memory "Local\MCPassthroughFrame") into textures
// that MCPassthrough.fx composites into GTA's picture against GTA's depth buffer.
#pragma once

namespace compositor
{
	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (the script turns it off in menus and cutscenes).
	void set_active(bool active);
	/// GTA's camera clip planes, to turn its depth buffer into metres.
	void set_host_planes(float near_clip, float far_clip);
	/// GTA's current camera in Minecraft's convention (rotation in degrees, vertical fov, position in Minecraft
	/// coordinates): Minecraft's frame is re-projected (with its depth) from the pose it was rendered with to this
	/// one, hiding the link's latency on camera turns and moves.
	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z);
	/// How many poses back the presented picture is (1: the script reads the camera of the frame being prepared).
	void set_pose_lag(int frames);
	/// Minecraft's picture moves with the camera (the flight chase cam frames Steve): composite it as rendered,
	/// without re-projecting it to GTA's newer pose, so Steve stays exactly where the camera framed him.
	void set_camera_locked(bool locked);
	/// A scene's look: the effect's "Match GTA lighting", depth bias and slope bias (negative: the preset's own values).
	void set_look(float light_match, float depth_bias, float slope_bias);
	/// The camera shake, applied to the finished picture (x, y: fraction of the screen height; roll: radians), and the
	/// nether portal's warp (0..1).
	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp);
	/// The virtual cursor over Minecraft's open screen (u, v: 0..1 of the picture), drawn by the effect.
	void set_cursor(bool on, float u, float v);
	/// GTA's backbuffer size as ReShade sees it (0 until the first frame).
	void backbuffer_size(int &width, int &height);
	/// Where the effects run: mode 0 at present (over the HUD), mode 1 right before the ui_draw-th draw into the
	/// backbuffer (the scene's final pass is the first; the HUD then draws over Minecraft).
	void set_ui_mode(int mode, int ui_draw);
	/// Sets the effect's DebugView (0 composite, 1 host depth bands, 2 Minecraft depth bands, 3 depth difference)
	/// and HostDepthScale (negative: leave as is).
	void set_debug(int debug_view, float host_depth_scale);
	/// When RoN's scene depth is copied: 0 never, 1 before the clear that follows the base pass, 2 when the base pass ends.
	void set_depth_copy_mode(int mode);
	/// Log one frame's render target binds and draw counts to ReShade.log.
	void request_trace();
	/// Frames presented, and how many of them had the effects rendered before the UI.
	void stats(int &frames, int &early_frames);
}
