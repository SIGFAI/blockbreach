#include "compositor.h"
#include "crash_guard.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <reshade.hpp>
#include <d3d11.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace reshade::api;

namespace
{
	constexpr const wchar_t *kMappingName = L"Local\\MCPassthroughFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffect = "MCPassthrough.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostNear{0.15f};
	std::atomic<float> g_hostFar{10000.0f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	// RoN's scene depth: the depth-stencil bound with UE's G-buffer (3+ render targets: the base pass). ReShade's
	// generic depth picks a buffer holding only the first-person weapon when the game renders at a lower resolution
	// (DLSS/TSR), so the effect samples this one instead (texture semantic RONDEPTH).
	// RoN clears that depth after the scene and draws the first-person weapon into it again, so at the end of the
	// frame it holds only the weapon: the scene's depth is copied right before that clear (RONDEPTH), and the live
	// buffer (the weapon, RONDEPTHLIVE) is combined with it in the effect.
	uint64_t g_sceneDepthCandidate = 0;
	bool g_basePassSeen = false, g_copiedThisFrame = false, g_inBasePass = false;
	std::atomic<int> g_depthCopyMode{1}; // 0 off, 1 before the clear after the base pass, 2 when the base pass ends
	resource g_sceneDepth = {0};
	resource_view g_sceneDepthSrv = {0};
	resource g_depthCopy = {0};
	resource_view g_depthCopySrv = {0};
	uint32_t g_depthCopyW = 0, g_depthCopyH = 0;
	format g_depthCopyFormat = format::unknown;
	std::atomic<int> g_debugView{-1};
	std::atomic<float> g_depthScale{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	std::atomic<float> g_cursorU{0.5f}, g_cursorV{0.5f}, g_cursorOn{0.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
	};
	std::mutex g_poseLock;
	// GTA's recent camera poses, newest last. The script reads the camera GTA is about to render (the next frame),
	// while the picture being presented was rendered with the one before: re-project to that (g_poseLag back).
	Pose g_hostPoses[4];
	unsigned g_hostPoseCount = 0;
	std::atomic<int> g_poseLag{0}; // measured: 0 matches best (third and first person)
	Pose g_mcPose;

	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	DWORD g_nextOpenAttempt = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 2048.0f;
	int32_t g_mcFlags = 7;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount() + 1000;
		g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
		if (g_mapping == nullptr)
			return false;
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
		if (header == nullptr || read<uint32_t>(header) != kMagic)
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + g_stride * g_slots)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		reshade::log::message(reshade::log::level::info, "MCPassthrough: connected to Minecraft's frame export");
		return true;
	}

	// Textures and views that may still be bound to the effect: ReShade can touch a binding until it is replaced, so
	// they are destroyed a few presents later (after the new ones were bound), never right away. Toggling DLSS made
	// RoN recreate its depth buffer at a new size, our depth copy was recreated with it, and destroying the old view
	// at once crashed ReShade inside render_effects (2026-10-02).
	struct Grave
	{
		resource res;
		resource_view view;
		int frames;
	};
	std::vector<Grave> g_graves;

	void bury(resource res, resource_view view)
	{
		if (res.handle != 0 || view.handle != 0)
			g_graves.push_back(Grave{res, view, 4});
	}

	void collect_graves(device *dev, bool all)
	{
		for (auto it = g_graves.begin(); it != g_graves.end();)
		{
			if (all || --it->frames <= 0)
			{
				if (it->view.handle != 0)
					dev->destroy_resource_view(it->view);
				if (it->res.handle != 0)
					dev->destroy_resource(it->res);
				it = g_graves.erase(it);
			}
			else
				++it;
		}
	}

	void destroy_layers(device *)
	{
		for (Layer *layer : {&g_world, &g_depth, &g_overlay})
		{
			bury(layer->tex, layer->srv);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
	}

	/// Upload the newest published Minecraft frame, if there is one we haven't shown yet.
	void upload(effect_runtime *runtime)
	{
		const int64_t published = read<int64_t>(g_view + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0)
			return;
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			destroy_layers(dev);
			if (!create_layer(dev, g_world, w, h, format::r8g8b8a8_unorm) ||
				!create_layer(dev, g_depth, w, h, format::r32_float) ||
				!create_layer(dev, g_overlay, w, h, format::r8g8b8a8_unorm))
			{
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const uint8_t *base = g_view + kHeader + g_stride * slot;
		const size_t layer = size_t(w) * h * 4;
		subresource_data data;
		data.row_pitch = w * 4;
		data.slice_pitch = static_cast<uint32_t>(layer);
		data.data = const_cast<uint8_t *>(base);
		dev->update_texture_region(data, g_world.tex, 0);
		data.data = const_cast<uint8_t *>(base + layer);
		dev->update_texture_region(data, g_depth.tex, 0);
		data.data = const_cast<uint8_t *>(base + 2 * layer);
		dev->update_texture_region(data, g_overlay.tex, 0);
		if (read<int64_t>(desc) != seq)
			return; // Minecraft rewrote the slot mid-copy: show the next one instead
		g_lastPublish = published;
		g_mcNear = read<float>(desc + 32);
		g_mcFar = read<float>(desc + 36);
		g_mcFlags = read<int32_t>(desc + 44);
		g_mcPose.fov = read<float>(desc + 40);
		g_mcPose.yaw = read<float>(desc + 72);
		g_mcPose.pitch = read<float>(desc + 76);
		g_mcPose.roll = read<float>(desc + 80);
		g_mcPose.x = read<double>(desc + 48);
		g_mcPose.y = read<double>(desc + 56);
		g_mcPose.z = read<double>(desc + 64);
		g_mcPose.valid = true;
		g_hasFrame = true;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in GTA's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload MCPassthrough.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\MCPassthrough.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	// ---- Compositing before Ready or Not's interface ----
	// UE draws the finished scene into the backbuffer (one full-screen draw), then Slate draws the HUD and menus
	// on top of it. ReShade normally renders at present, i.e. over the HUD. In "before UI" mode the effects run right
	// before the g_uiDraw-th draw into the backbuffer of the frame, so the HUD is drawn over Minecraft. If a frame
	// never gets that far (loading screens), ReShade renders them at present as usual.
	effect_runtime *g_runtime = nullptr;
	std::atomic<int> g_uiMode{1}; // 0: at present (over the HUD), 1: before the UI
	std::atomic<int> g_uiDraw{2};  // 1-based: which draw into the backbuffer is the UI's first
	uint64_t g_bbResource = 0;
	resource_view g_boundRtv = {0};
	bool g_onBackbuffer = false;
	int g_bbDraws = 0;
	bool g_renderedEarly = false;
	int g_earlyFrames = 0, g_frames = 0;
	// diagnostic: one frame's render target binds and draw counts, written to ReShade.log
	std::atomic<bool> g_traceRequest{false};
	bool g_tracing = false;
	std::string g_trace;
	int g_traceDraws = 0;
	uint64_t g_traceDs[8] = {};

	char trace_ds_name(uint64_t handle)
	{
		for (int i = 0; i < 8; ++i)
		{
			if (g_traceDs[i] == handle)
				return char('A' + i);
			if (g_traceDs[i] == 0)
			{
				g_traceDs[i] = handle;
				return char('A' + i);
			}
		}
		return '?';
	}

	// Presents to wait before rendering early again: the swapchain was resized/recreated or the effects reloaded (RoN
	// does that when DLSS or the resolution changes). In between ReShade's own back buffer targets are being rebuilt,
	// and rendering effects then crashed inside D3D11 (2026-10-02, twice, on "Apply settings" with DLSS changed).
	std::atomic<int> g_earlyCooldown{120};
	int g_effectsSet = -1; // the effects state on_present last gave ReShade (-1: none yet)

	void cool_down() { g_earlyCooldown = 120; }

	void copy_scene_depth(command_list *cmd_list, resource res, bool again);

	void trace_flush_segment()
	{
		if (g_tracing && g_traceDraws > 0)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), " d=%d", g_traceDraws);
			g_trace += buffer;
		}
		g_traceDraws = 0;
	}

	void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
	{
		const resource_view rtv = count != 0 ? rtvs[0] : resource_view{0};
		const bool basePass = count >= 3 && dsv.handle != 0;
		if (basePass)
		{
			g_sceneDepthCandidate = cmd_list->get_device()->get_resource_from_view(dsv).handle;
			g_basePassSeen = true;
		}
		else if (g_inBasePass && g_depthCopyMode == 2)
			copy_scene_depth(cmd_list, resource{g_sceneDepthCandidate}, true);
		g_inBasePass = basePass;
		if (rtv == g_boundRtv && !g_tracing)
			return;
		trace_flush_segment();
		g_boundRtv = rtv;
		g_onBackbuffer = false;
		if (rtv.handle == 0)
		{
			if (g_tracing)
			{
				g_trace += "\n  bind none ds=";
				g_trace += dsv.handle != 0 ? trace_ds_name(cmd_list->get_device()->get_resource_from_view(dsv).handle) : '-';
			}
			return;
		}
		device *dev = cmd_list->get_device();
		const resource res = dev->get_resource_from_view(rtv);
		g_onBackbuffer = g_bbResource != 0 && res.handle == g_bbResource;
		if (g_tracing)
		{
			const resource_desc desc = dev->get_resource_desc(res);
			char buffer[160];
			const char ds = dsv.handle != 0 ? trace_ds_name(dev->get_resource_from_view(dsv).handle) : '-';
			std::snprintf(buffer, sizeof(buffer), "\n  bind %ux%u fmt=%u rts=%u ds=%c%s", desc.texture.width, desc.texture.height,
				unsigned(desc.texture.format), count, ds, g_onBackbuffer ? " BACKBUFFER" : "");
			g_trace += buffer;
		}
	}

	void destroy_depth_copy(device *)
	{
		bury(g_depthCopy, g_depthCopySrv);
		g_depthCopySrv = {0};
		g_depthCopy = {0};
		g_depthCopyW = g_depthCopyH = 0;
		g_depthCopyFormat = format::unknown;
	}

	bool on_clear_depth(command_list *cmd_list, resource_view dsv, const float *depth, const uint8_t *, uint32_t, const rect *)
	{
		if (g_tracing)
		{
			trace_flush_segment();
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "\n  CLEAR ds=%c depth=%s%s", trace_ds_name(cmd_list->get_device()->get_resource_from_view(dsv).handle),
				depth ? std::to_string(*depth).c_str() : "-", g_basePassSeen ? " (after base pass)" : "");
			g_trace += buffer;
		}
		if (depth == nullptr || !g_basePassSeen || g_copiedThisFrame || g_sceneDepthCandidate == 0 || g_depthCopyMode != 1)
			return false;
		const resource res = cmd_list->get_device()->get_resource_from_view(dsv);
		if (res.handle != g_sceneDepthCandidate)
			return false;
		copy_scene_depth(cmd_list, res, false);
		return false;
	}

	/// Copies the scene's depth into g_depthCopy (the copy the effect samples as RONDEPTH).
	void copy_scene_depth(command_list *cmd_list, resource res, bool again)
	{
		crash_guard::Scope scope("render_copy_depth");
		if (g_copiedThisFrame && !again)
			return;
		device *dev = cmd_list->get_device();
		const resource_desc desc = dev->get_resource_desc(res);
		if (desc.texture.width != g_depthCopyW || desc.texture.height != g_depthCopyH || desc.texture.format != g_depthCopyFormat)
		{
			destroy_depth_copy(dev);
			resource_desc copy = desc;
			copy.heap = memory_heap::default_;
			copy.usage = resource_usage::shader_resource | resource_usage::copy_dest;
			copy.flags = resource_flags::none;
			copy.texture.format = format_to_typeless(desc.texture.format);
			if (!dev->create_resource(copy, nullptr, resource_usage::shader_resource, &g_depthCopy) ||
				!dev->create_resource_view(g_depthCopy, resource_usage::shader_resource, resource_view_desc(format_to_default_typed(copy.texture.format)), &g_depthCopySrv))
			{
				destroy_depth_copy(dev);
				return;
			}
			cool_down();
			g_depthCopyW = desc.texture.width;
			g_depthCopyH = desc.texture.height;
			g_depthCopyFormat = desc.texture.format;
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer), "MCPassthrough: copying scene depth before its clear (%ux%u fmt=%u)", desc.texture.width, desc.texture.height, unsigned(desc.texture.format));
			reshade::log::message(reshade::log::level::info, buffer);
		}
		const bool explicitStates = dev->get_api() != device_api::d3d11 && dev->get_api() != device_api::d3d10 && dev->get_api() != device_api::d3d9;
		if (explicitStates)
		{
			const resource both[2] = {res, g_depthCopy};
			const resource_usage before[2] = {resource_usage::depth_stencil_write, resource_usage::shader_resource};
			const resource_usage during[2] = {resource_usage::copy_source, resource_usage::copy_dest};
			cmd_list->barrier(2, both, before, during);
			cmd_list->copy_resource(res, g_depthCopy);
			cmd_list->barrier(2, both, during, before);
		}
		else
			cmd_list->copy_resource(res, g_depthCopy);
		g_copiedThisFrame = true;
		if (g_tracing)
			g_trace += "\n  (scene depth copied)";
	}

	void on_init_swapchain(swapchain *, bool) { cool_down(); }
	void on_destroy_swapchain(swapchain *, bool) { cool_down(); }
	void on_init_runtime(effect_runtime *)
	{
		cool_down();
		g_effectsSet = -1; // a new runtime has the preset's effects state: set ours again on its first present
	}

	void render_early(command_list *cmd_list)
	{
		// Only in the game proper, with Minecraft composited: in menus there is nothing to draw under the UI, and all
		// three crashes so far (2026-10-02 18:11, 18:30, 19:02) came from rendering mid-frame while RoN's pause or
		// options menu applied new settings (DLSS, render resolution) and rebuilt its render targets. In menus ReShade
		// renders at present as usual.
		if (!g_active || g_runtime == nullptr || g_earlyCooldown > 0 || cmd_list->get_device()->get_api() != device_api::d3d11)
			return;
		// the target must be the back buffer the runtime is set up for right now, at its size
		device *dev = cmd_list->get_device();
		const resource target = dev->get_resource_from_view(g_boundRtv);
		if (target.handle == 0 || target != g_runtime->get_current_back_buffer())
			return;
		uint32_t w = 0, h = 0;
		g_runtime->get_screenshot_width_and_height(&w, &h);
		const resource_desc desc = dev->get_resource_desc(target);
		if (desc.texture.width != w || desc.texture.height != h)
			return;
		auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
		if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
			return;
		// outside present, ReShade captures the application's state before the effects and restores it after
		crash_guard::Scope scope("render_early");
		g_runtime->render_effects(cmd_list, g_boundRtv, g_boundRtv);
		g_renderedEarly = true;
	}

	bool on_any_draw(command_list *cmd_list)
	{
		if (g_tracing)
			++g_traceDraws;
		if (g_onBackbuffer)
		{
			++g_bbDraws;
			if (g_uiMode == 1 && !g_renderedEarly && g_bbDraws == g_uiDraw)
				render_early(cmd_list);
		}
		return false;
	}
	bool on_draw(command_list *cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) { return on_any_draw(cmd_list); }
	bool on_draw_indexed(command_list *cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return on_any_draw(cmd_list); }
	bool on_draw_indirect(command_list *cmd_list, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
	{
		return type == indirect_command::dispatch ? false : on_any_draw(cmd_list);
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
		g_runtime = runtime;
		collect_graves(runtime->get_device(), false);
		static bool wasActive = false;
		const bool active = g_active;
		if (active && !wasActive)
			cool_down(); // back from a menu: settings may just have changed, let RoN settle first
		wasActive = active;
		// No ReShade effects at all outside the game proper (menus, pause, loading): there is nothing to composite, and
		// rendering them is what crashed RoN when DLSS was switched in the options (2026-10-04 01:30, reproduced; also
		// 2026-10-02 18:11/18:30/19:02). Right after "Apply settings" a shader resource view RoN/DLSS left bound is
		// already destroyed, and ReShade's command_list::barrier (at the start of every technique pass) asks each
		// bound view for its resource: ID3D11View::GetResource on the dead view reads 0x68 in d3d11.dll. With effects
		// off, ReShade's present skips render_effects and never touches the game's bindings. Takes effect next frame.
		// on the change only, so ReShade's own effects toggle key still works in between (a new runtime starts over)
		if (g_effectsSet != int(active))
		{
			g_effectsSet = int(active);
			runtime->set_effects_state(active);
			crash_guard::note("reshade effects %s", active ? "on (in game)" : "off (menu)");
		}
		if (g_earlyCooldown > 0)
			--g_earlyCooldown;
		if (g_tracing)
		{
			trace_flush_segment();
			reshade::log::message(reshade::log::level::info, ("MCPassthrough: frame trace (render target binds, draws per bind)" + g_trace).c_str());
			g_trace.clear();
			g_tracing = false;
		}
		if (g_traceRequest.exchange(false))
		{
			g_tracing = true;
			g_trace = "";
			std::memset(g_traceDs, 0, sizeof(g_traceDs));
		}
		++g_frames;
		if (g_renderedEarly)
			++g_earlyFrames;
		g_bbDraws = 0;
		g_renderedEarly = false;
		g_basePassSeen = false;
		g_copiedThisFrame = false;
		g_inBasePass = false;
		g_bbResource = runtime->get_current_back_buffer().handle;
		g_boundRtv = {0};
		g_onBackbuffer = false;
	}

	void bind_scene_depth(effect_runtime *runtime)
	{
		if (g_sceneDepthCandidate != g_sceneDepth.handle)
		{
			cool_down(); // RoN made a new depth buffer (render resolution / DLSS changed)
			device *dev = runtime->get_device();
			bury(resource{0}, g_sceneDepthSrv);
			g_sceneDepthSrv = {0};
			g_sceneDepth = {g_sceneDepthCandidate};
			if (g_sceneDepth.handle != 0 && dev->get_api() != device_api::d3d11)
			{
				// DX12: the depth buffer sits in its write state outside the copy; the effect reads the copy only
				g_sceneDepthSrv = {0};
			}
			else if (g_sceneDepth.handle != 0)
			{
				const resource_desc desc = dev->get_resource_desc(g_sceneDepth);
				if (!dev->create_resource_view(g_sceneDepth, resource_usage::shader_resource, resource_view_desc(format_to_default_typed(desc.texture.format)), &g_sceneDepthSrv))
					g_sceneDepthSrv = {0};
				char buffer[160];
				std::snprintf(buffer, sizeof(buffer), "MCPassthrough: scene depth %ux%u fmt=%u srv=%s", desc.texture.width, desc.texture.height,
					unsigned(desc.texture.format), g_sceneDepthSrv.handle != 0 ? "ok" : "failed");
				reshade::log::message(reshade::log::level::info, buffer);
			}
			runtime->update_texture_bindings("RONDEPTHLIVE", g_sceneDepthSrv, g_sceneDepthSrv);
		}
		static uint64_t boundCopy = 0;
		if (g_depthCopySrv.handle != boundCopy)
		{
			boundCopy = g_depthCopySrv.handle;
			runtime->update_texture_bindings("RONDEPTH", g_depthCopySrv, g_depthCopySrv);
		}
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostDepthOwn"); v.handle != 0)
			runtime->set_uniform_value_bool(v, g_sceneDepthSrv.handle != 0);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostDepthCopy"); v.handle != 0)
			runtime->set_uniform_value_bool(v, g_copiedThisFrame && g_depthCopySrv.handle != 0);
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		crash_guard::Scope scope("render_begin_effects");
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		bool on = g_active && open_mapping();
		if (on)
			upload(runtime);
		on = on && g_hasFrame;
		// The technique stays enabled (preset); McActive gates it, so GTA passes through untouched until a
		// Minecraft frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (!on)
			return;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_mcNear, g_mcFar, float(g_mcFlags));
		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		bind_scene_depth(runtime);
		if (const int dv = g_debugView.exchange(-1); dv >= 0)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "DebugView"); v.handle != 0)
				runtime->set_uniform_value_int(v, dv);
		if (const float ds = g_depthScale.exchange(-1.0f); ds > 0.0f)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostDepthScale"); v.handle != 0)
				runtime->set_uniform_value_float(v, ds);
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "Shake"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "Cursor"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_cursorU.load(), g_cursorV.load(), g_cursorOn.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());

		// Re-projection from Minecraft's pose to GTA's latest (extrapolated by the effect's PosePrediction frames).
		Pose host, prev;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 2));
			if (g_hostPoseCount > lag)
				host = g_hostPoses[(g_hostPoseCount - 1 - lag) & 3];
			if (g_hostPoseCount > lag + 1)
				prev = g_hostPoses[(g_hostPoseCount - 2 - lag) & 3];
		}
		float predict = 0.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		const bool warp = host.valid && g_mcPose.valid && !g_cameraLocked;
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, g_mcPose, m);
			// T = R_mc^T (host position - Minecraft's camera position), in Minecraft's camera space
			float rm[3][3];
			camera_rotation(g_mcPose, rm);
			const float d[3] = {float(host.x - g_mcPose.x), float(host.y - g_mcPose.y), float(host.z - g_mcPose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
		}
		const char *rows[3] = {"WarpRow0", "WarpRow1", "WarpRow2"};
		for (int i = 0; i < 3; ++i)
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, rows[i]); v.handle != 0)
				runtime->set_uniform_value_float(v, m[i][0], m[i][1], m[i][2]);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpT"); v.handle != 0)
			runtime->set_uniform_value_float(v, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : g_mcPose.fov) * d2r * 0.5f), tanMc = std::tan(g_mcPose.fov * d2r * 0.5f);
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, tanHost, tanMc, float(g_width) / float(g_height));
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		cool_down();
		if (g_width != 0)
			bind(runtime);
		if (g_sceneDepthSrv.handle != 0)
			runtime->update_texture_bindings("RONDEPTHLIVE", g_sceneDepthSrv, g_sceneDepthSrv);
		if (g_depthCopySrv.handle != 0)
			runtime->update_texture_bindings("RONDEPTH", g_depthCopySrv, g_depthCopySrv);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
		bury(resource{0}, g_sceneDepthSrv);
		g_sceneDepthSrv = {0};
		g_sceneDepth = {0};
		destroy_depth_copy(runtime->get_device());
		collect_graves(runtime->get_device(), true); // the runtime is gone: nothing can use them any more
		if (runtime == g_runtime)
			g_runtime = nullptr;
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth);
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "MCPassthrough: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(module);
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		g_hostPoses[g_hostPoseCount & 3] = {yaw, pitch, roll, fov, x, y, z, true};
		++g_hostPoseCount;
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}

	void set_ui_mode(int mode, int ui_draw)
	{
		g_uiMode = mode;
		g_uiDraw = std::max(1, ui_draw);
	}

	void set_depth_copy_mode(int mode)
	{
		g_depthCopyMode = mode;
	}

	void set_cursor(bool on, float u, float v)
	{
		g_cursorU = u;
		g_cursorV = v;
		g_cursorOn = on ? 1.0f : 0.0f;
	}

	void set_debug(int debug_view, float host_depth_scale)
	{
		if (debug_view >= 0)
			g_debugView = debug_view;
		if (host_depth_scale > 0.0f)
			g_depthScale = host_depth_scale;
	}

	void request_trace()
	{
		g_traceRequest = true;
	}

	void stats(int &frames, int &early_frames)
	{
		frames = g_frames;
		early_frames = g_earlyFrames;
	}
}
