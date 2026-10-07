// RoNPassthrough: the Ready or Not half of the Minecraft passthrough (UE4SS C++ mod + ReShade add-on in one DLL).
//
// Every engine tick (game thread) it reads the player's camera (PlayerCameraManager.CameraCachePrivate.POV) and sends
// it to the Minecraft mod over the WebSocket on 127.0.0.1:25599 ({"t":"cam",...}, Minecraft coordinates). The
// compositor (ReShade add-on, compositor.cpp) draws Minecraft's frame into RoN's picture against RoN's depth buffer.
//
// Coordinates: 1 m = 1 block. UE (x, y, z) in cm, Z up, left-handed -> Minecraft (x + xOff, z + yOffset, y + zOff) / 100,
// right-handed (swapping two axes flips the handedness). Minecraft yaw = UE yaw - 90, pitch = -pitch.
// yOffset puts the player's feet at y = 64 (re-levelled with F6). xOff/zOff give every RoN map its own part of
// Minecraft's world (from a hash of the map's package name), so what is built in one map stays out of the others.
//
// Keys: NumPad 0 build mode (the mouse buttons, wheel and 1-9 go to Minecraft instead of RoN), F5 passthrough on/off,
// F6 re-level + test blocks in front of the player, F7 composite before UI / over UI, F9 pose lag 0/1/2,
// Ctrl+F7 log one frame's render target trace to ReShade.log, Ctrl+F9 roll sign.
//
// Minecraft's world in RoN: every solid block Minecraft reports ({"t":"blocks"}) becomes an invisible 1 m box with
// BlockAll collision on one actor of ours, so RoN's player and AI bump into what was built; Minecraft explosions
// ({"t":"explosion"}) apply radial damage in RoN. Steve's arrows and crossbow fireworks in flight ({"t":"proj"}) are
// traced through RoN's world: a firework bursts where it hits (radial damage), an arrow damages what it hits or sticks
// in the wall, and Minecraft is told where ({"t":"projhit"}). A sword swing ({"t":"melee"}) hits what is in front.
// Mobs vs RoN's people: the living AI characters go to Minecraft as "peds" (invisible proxies its hostile mobs hunt),
// and a mob's hit on one ({"t":"mobhit"}) is bullet damage on that character in RoN. The other way round, a suspect
// with a clear line to a mob within 15 m turns to it and fires its gun (ForceFireGun) twice a second; the mob takes the
// hit in Minecraft ({"t":"mobdmg"}).
#include <Windows.h>
#include <share.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectArray.hpp>

#include "compositor.h"
#include "crash_guard.h"
#include "ue_call.h"
#include "ws.h"

using namespace RC;
using namespace RC::Unreal;

namespace
{
	constexpr int kPort = 25599;
	constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;
	constexpr double kFeetY = 64.0;
	// RoN's floor as Minecraft barriers around the player: columns within this many blocks, probes per tick
	constexpr int kGroundRadius = 32;      // around the player
	constexpr int kSuspectRadius = 12;     // around every suspect (their own storey, wherever they are)
	constexpr int kGroundProbesPerTick = 40;       // at most, and
	constexpr double kGroundProbeMsPerTick = 0.5;  // no more than this much of a frame (at least one column a tick)
	// the storeys looked for in every column: from 15 m below to 25 m above the level the player started on
	constexpr double kFloorsBelow = 1500.0, kFloorsAbove = 2500.0;
	constexpr int kMaxFloors = 6;

	HMODULE g_module = nullptr;

	/// The mod's folder (...\ue4ss\Mods\RoNPassthrough), from main.dll's path; empty if unknown.
	std::wstring mod_dir()
	{
		wchar_t path[MAX_PATH];
		if (g_module == nullptr || !GetModuleFileNameW(g_module, path, MAX_PATH))
			return {};
		std::wstring dir(path);
		dir = dir.substr(0, dir.find_last_of(L'\\')); // ...\RoNPassthrough\dlls
		return dir.substr(0, dir.find_last_of(L'\\'));  // ...\RoNPassthrough
	}

	/// The mod's own lines, appended to Mods\RoNPassthrough\history.log across sessions (UE4SS.log starts over on every
	/// launch, so a session the owner played and then restarted was lost). Rotated to history.old.log past 20 MB.
	void history(const char *line)
	{
		static std::mutex lock;
		static FILE *file = nullptr;
		static bool tried = false;
		std::lock_guard<std::mutex> guard(lock);
		if (!tried)
		{
			tried = true;
			if (const std::wstring dir = mod_dir(); !dir.empty())
			{
				const std::wstring log = dir + L"\\history.log", old = dir + L"\\history.old.log";
				WIN32_FILE_ATTRIBUTE_DATA info;
				if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 20u * 1024 * 1024)
					MoveFileExW(log.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
				file = _wfsopen(log.c_str(), L"a", _SH_DENYNO);
				if (file != nullptr)
				{
					SYSTEMTIME t;
					GetLocalTime(&t);
					std::fprintf(file, "\n===== session %04d-%02d-%02d %02d:%02d:%02d =====\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
				}
			}
		}
		if (file == nullptr)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(file, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
		std::fflush(file);
	}

	struct RawByteArray
	{
		uint8_t *data;
		int32_t num, max;
	};

	struct RawPtrArray
	{
		UObject **data;
		int32_t num, max;
	};

	struct FVectorD
	{
		double x, y, z;
	};
	struct FRotatorD
	{
		double pitch, yaw, roll;
	};

	/// A UObject member found by reflection (offset cached per class: the chain is walked once per class).
	template <typename T>
	struct Member
	{
		const TCHAR *name;
		UClass *cls = nullptr;
		int32_t offset = -1;

		T *in(UObject *object)
		{
			if (object == nullptr)
				return nullptr;
			UClass *c = object->GetClassPrivate();
			if (c != cls)
			{
				cls = c;
				FProperty *p = object->GetPropertyByNameInChain(name);
				offset = p ? p->GetOffset_Internal() : -1;
			}
			return offset < 0 ? nullptr : reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(object) + offset);
		}
	};

	/// A bool UPROPERTY (bitfields too: FBoolProperty knows its byte and mask), found once per class.
	struct BoolMember
	{
		const TCHAR *name;
		UClass *cls = nullptr;
		FBoolProperty *prop = nullptr;

		bool get(UObject *object, bool fallback)
		{
			if (object == nullptr)
				return fallback;
			UClass *c = object->GetClassPrivate();
			if (c != cls)
			{
				cls = c;
				prop = CastField<FBoolProperty>(object->GetPropertyByNameInChain(name));
			}
			return prop ? prop->GetPropertyValueInContainer(object) : fallback;
		}
	};

	/// Offsets inside the camera cache: CameraCachePrivate (FCameraCacheEntry) -> POV (FMinimalViewInfo) -> fields.
	struct PovLayout
	{
		bool resolved = false, ok = false;
		int32_t cache = -1, pov = -1, location = -1, rotation = -1, fov = -1, aspect = -1;
	};

	// Minecraft blocks as RoN collision boxes at most (each is a component on one actor; a creative player builds a lot)
	constexpr int kMaxBlocks = 2500;

	// Build mode: RoN's window procedure is subclassed; while it is on (and the player is in the game), the mouse
	// buttons, the wheel and the number keys go to Minecraft over the link and RoN never sees them (so it doesn't fire).
	WNDPROC g_origWndProc = nullptr;
	HWND g_gameWindow = nullptr;
	std::atomic<bool> g_build{false}, g_inGame{false};
	WsClient *g_link = nullptr;
	// Minecraft's inventory (or any screen of its) open in build mode: RoN's mouse moves a virtual cursor over it instead
	// of the camera (raw mouse deltas, RoN never sees them), the buttons and the wheel click and scroll there.
	std::atomic<bool> g_mcScreen{false}, g_cursorDirty{false};
	std::atomic<float> g_cursorX{0.5f}, g_cursorY{0.5f};
	constexpr float kCursorSpeed = 1.25f; // raw mouse counts per screen pixel

	void link_send(const char *format, ...)
	{
		char buffer[128];
		va_list args;
		va_start(args, format);
		std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		if (g_link)
			g_link->send(buffer);
	}

	/// Minecraft's screen is open: the mouse and the keys go there (RoN gets none of them, so its camera stays put).
	bool screen_input(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT &result)
	{
		auto click = [](int b, bool down) { link_send("{\"t\":\"click\",\"b\":%d,\"down\":%s}", b, down ? "true" : "false"); };
		result = 0;
		switch (msg)
		{
		case WM_INPUT:
		{
			RAWINPUT raw{};
			UINT size = sizeof(raw);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
				raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
			{
				RECT rc{};
				GetClientRect(hwnd, &rc);
				const float w = float(std::max<LONG>(1, rc.right - rc.left)), h = float(std::max<LONG>(1, rc.bottom - rc.top));
				g_cursorX = std::clamp(g_cursorX.load() + raw.data.mouse.lLastX * kCursorSpeed / w, 0.0f, 1.0f);
				g_cursorY = std::clamp(g_cursorY.load() + raw.data.mouse.lLastY * kCursorSpeed / h, 0.0f, 1.0f);
				g_cursorDirty = true;
			}
			// the raw input is released, but RoN never hears of it
			result = DefWindowProcW(hwnd, msg, wp, lp);
			return true;
		}
		case WM_MOUSEMOVE:
			return true;
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
			click(0, true);
			return true;
		case WM_LBUTTONUP:
			click(0, false);
			return true;
		case WM_RBUTTONDOWN:
		case WM_RBUTTONDBLCLK:
			click(1, true);
			return true;
		case WM_RBUTTONUP:
			click(1, false);
			return true;
		case WM_MBUTTONDOWN:
			click(2, true);
			return true;
		case WM_MBUTTONUP:
			click(2, false);
			return true;
		case WM_MOUSEWHEEL:
			link_send("{\"t\":\"wheel\",\"d\":%d}", GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
			return true;
		case WM_KEYDOWN:
			// E (the inventory key) or Esc closes the screen; Esc must not open RoN's pause menu either
			if ((wp == 'E' || wp == VK_ESCAPE) && !(lp & (1 << 30)))
				link_send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
			return true;
		case WM_KEYUP:
		case WM_CHAR:
			return true;
		}
		return false;
	}

	LRESULT CALLBACK build_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (g_build && g_inGame && g_mcScreen)
		{
			LRESULT result = 0;
			if (screen_input(hwnd, msg, wp, lp, result))
				return result;
		}
		if (g_build && g_inGame)
		{
			auto key = [](const char *k, bool down) { link_send("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", k, down ? "true" : "false"); };
			switch (msg)
			{
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
				key("attack", true);
				return 0;
			case WM_LBUTTONUP:
				key("attack", false);
				return 0;
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
				key("use", true);
				return 0;
			case WM_RBUTTONUP:
				key("use", false);
				return 0;
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
				key("pick", true);
				return 0;
			case WM_MBUTTONUP:
				key("pick", false);
				return 0;
			case WM_MOUSEWHEEL:
				link_send("{\"t\":\"scroll\",\"d\":%d}", GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
				return 0;
			case WM_KEYDOWN:
			case WM_KEYUP:
				if (wp >= '1' && wp <= '9')
				{
					if (msg == WM_KEYDOWN && !(lp & (1 << 30)))
						link_send("{\"t\":\"slot\",\"n\":%d}", int(wp - '1'));
					return 0;
				}
				if (wp == 'E')
				{
					// Minecraft's inventory (creative: every item); RoN's lean-right key is not used while building
					if (!(lp & (1 << 30)) || msg == WM_KEYUP)
						link_send("{\"t\":\"key\",\"k\":\"inventory\",\"down\":%s}", msg == WM_KEYDOWN ? "true" : "false");
					return 0;
				}
				break;
			}
		}
		return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
	}

	BOOL CALLBACK find_game_window(HWND hwnd, LPARAM out)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		wchar_t cls[64];
		if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetClassNameW(hwnd, cls, 64) && std::wcscmp(cls, L"UnrealWindow") == 0)
		{
			*reinterpret_cast<HWND *>(out) = hwnd;
			return FALSE;
		}
		return TRUE;
	}

	void hook_window()
	{
		if (g_gameWindow != nullptr && IsWindow(g_gameWindow))
			return;
		HWND hwnd = nullptr;
		EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&hwnd));
		if (hwnd == nullptr)
			return;
		g_gameWindow = hwnd;
		g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(build_wndproc)));
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] input hook on RoN's window\n"));
	}

	void unhook_window()
	{
		if (g_gameWindow != nullptr && IsWindow(g_gameWindow) && g_origWndProc != nullptr &&
			GetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(build_wndproc))
			SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
		g_gameWindow = nullptr;
	}

	/// The numbers in a flat JSON array: "key":[1,2,3]
	std::vector<int> json_ints(const std::string &m, const char *key)
	{
		std::vector<int> out;
		const size_t at = m.find(std::string("\"") + key + "\":[");
		if (at == std::string::npos)
			return out;
		const char *p = m.c_str() + at + std::strlen(key) + 4;
		while (*p && *p != ']')
		{
			char *end = nullptr;
			const long v = std::strtol(p, &end, 10);
			if (end == p)
				break;
			out.push_back(int(v));
			p = end;
			while (*p == ',' || *p == ' ')
				++p;
		}
		return out;
	}

	std::vector<double> json_doubles(const std::string &m, const char *key)
	{
		std::vector<double> out;
		const size_t at = m.find(std::string("\"") + key + "\":[");
		if (at == std::string::npos)
			return out;
		const char *p = m.c_str() + at + std::strlen(key) + 4;
		while (*p && *p != ']')
		{
			char *end = nullptr;
			const double v = std::strtod(p, &end);
			if (end == p)
				break;
			out.push_back(v);
			p = end;
			while (*p == ',' || *p == ' ')
				++p;
		}
		return out;
	}

	/// The text of a plain number field "key":<number> (empty if missing).
	std::string json_number(const std::string &m, const char *key)
	{
		const size_t at = m.find(std::string("\"") + key + "\":");
		if (at == std::string::npos)
			return {};
		const size_t start = at + std::strlen(key) + 3;
		size_t end = start;
		while (end < m.size() && (std::isdigit(static_cast<unsigned char>(m[end])) || m[end] == '-' || m[end] == '.' || m[end] == 'e'))
			++end;
		return m.substr(start, end - start);
	}

	/// A UObject kept across frames: the pointer and its slot in GUObjectArray, remembered while it was known to be
	/// valid. get() returns it only while that slot still holds this same object and the object isn't being destroyed,
	/// so a character killed or a level unloaded since never gets used (it crashed RoN on 2026-10-02, 17:26).
	struct Ref
	{
		UObject *p = nullptr;
		int32_t index = -1;

		Ref() = default;
		explicit Ref(UObject *object) : p(object), index(object ? object->GetInternalIndex() : -1)
		{
		}

		UObject *get() const
		{
			if (p == nullptr || index < 0)
				return nullptr;
			FUObjectItem *item = UObjectArray::IndexToObject(index);
			if (item == nullptr || item->GetUObject() != p || item->IsUnreachable() || item->IsPendingKill())
				return nullptr;
			return p;
		}
	};

	float wrap_degrees(float a)
	{
		a = std::fmod(a + 180.0f, 360.0f);
		if (a < 0)
			a += 360.0f;
		return a - 180.0f;
	}
}

/// UKismetSystemLibrary::LineTraceSingle through ProcessEvent, its parameter offsets found by reflection once.
class LineTracer
{
public:
	/// fn: a KismetSystemLibrary trace, e.g. "LineTraceSingle" (the Visibility channel: characters block it too),
	/// "LineTraceSingleForObjects" / "SphereTraceSingleForObjects" (only the level: WorldStatic + WorldDynamic).
	/// staticOnly: object traces see WorldStatic only (walls, floors, fixed furniture), not WorldDynamic (doors, props).
	explicit LineTracer(const TCHAR *fn = STR("LineTraceSingle"), bool staticOnly = false) : m_name(fn), m_staticOnly(staticOnly)
	{
	}

	/// Traces from a to b (UE coordinates, cm), ignoring the context actor (and `ignore`, and up to 3 `more`);
	/// `radius` for sphere traces. Game thread only.
	bool trace(UObject *context, const FVectorD &a, const FVectorD &b, FVectorD &hit, UObject *ignore = nullptr, float radius = 0.0f,
		UObject *const *more = nullptr, int moreCount = 0)
	{
		if (!m_resolved)
			resolve();
		if (!m_ok)
			return false;
		uint8_t *params = m_params;
		std::memset(params, 0, m_size);
		*reinterpret_cast<UObject **>(params + m_context) = context;
		*reinterpret_cast<FVectorD *>(params + m_start) = a;
		*reinterpret_cast<FVectorD *>(params + m_end) = b;
		static uint8_t levelTypes[2] = {0, 1}; // ObjectTypeQuery1/2: WorldStatic, WorldDynamic
		if (m_channel >= 0)
			params[m_channel] = 0; // TraceTypeQuery1: Visibility
		if (m_objectTypes >= 0)
			*reinterpret_cast<RawByteArray *>(params + m_objectTypes) = RawByteArray{levelTypes, m_staticOnly ? 1 : 2, 2};
		if (m_radius >= 0)
			*reinterpret_cast<float *>(params + m_radius) = radius;
		m_ignoreSelf->SetPropertyValueInContainer(params, true);
		UObject *ignored[4] = {};
		int ignoredCount = 0;
		if (ignore != nullptr)
			ignored[ignoredCount++] = ignore;
		for (int i = 0; i < moreCount && ignoredCount < 4; ++i)
			if (more[i] != nullptr)
				ignored[ignoredCount++] = more[i];
		if (ignoredCount > 0 && m_ignoreActors >= 0)
			*reinterpret_cast<ue::PtrArray *>(params + m_ignoreActors) = ue::PtrArray{ignored, ignoredCount, ignoredCount};
		m_cdo->ProcessEvent(m_fn, params);
		if (!m_returnValue->GetPropertyValueInContainer(params))
			return false;
		hit = *reinterpret_cast<FVectorD *>(params + m_outHit + m_impactPoint);
		return true;
	}

	bool ok() const
	{
		return m_ok;
	}

	/// The full name of the component the last trace hit (FHitResult.Component, a weak pointer), for debugging.
	/// The last trace's FHitResult, raw (to pass on to ApplyPointDamage), and its size.
	const uint8_t *last_hit(int32_t &size) const
	{
		size = m_hitSize;
		return m_params + m_outHit;
	}

	/// The actor owning the component the last trace hit, or null.
	UObject *last_actor() const
	{
		if (m_component < 0)
			return nullptr;
		const int32_t index = *reinterpret_cast<const int32_t *>(m_params + m_outHit + m_component);
		FUObjectItem *item = index > 0 ? UObjectArray::IndexToObject(index) : nullptr;
		UObject *component = item ? item->GetUObject() : nullptr;
		if (component == nullptr)
			return nullptr;
		ue::Call owner(STR("/Script/Engine.ActorComponent:GetOwner"));
		owner.run(component);
		return owner.get<UObject *>(STR("ReturnValue"));
	}

	/// Whether the last trace hit a volume's brush (BlockingVolume and the like: invisible, no floors inside).
	bool last_hit_volume() const
	{
		if (m_component < 0)
			return false;
		const int32_t index = *reinterpret_cast<const int32_t *>(m_params + m_outHit + m_component);
		FUObjectItem *item = index > 0 ? UObjectArray::IndexToObject(index) : nullptr;
		UObject *component = item ? item->GetUObject() : nullptr;
		return component != nullptr && component->GetClassPrivate()->GetName() == STR("BrushComponent");
	}

	StringType last_component() const
	{
		if (m_component < 0)
			return STR("?");
		const int32_t index = *reinterpret_cast<const int32_t *>(m_params + m_outHit + m_component);
		if (index <= 0)
			return STR("none");
		FUObjectItem *item = UObjectArray::IndexToObject(index);
		UObject *object = item ? item->GetUObject() : nullptr;
		return object ? object->GetFullName() : STR("gone");
	}

private:
	void resolve()
	{
		m_resolved = true;
		const StringType path = StringType(STR("/Script/Engine.KismetSystemLibrary:")) + m_name;
		m_fn = UObjectGlobals::StaticFindObject<UFunction *>(nullptr, nullptr, path.c_str());
		m_cdo = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
		if (m_fn == nullptr || m_cdo == nullptr)
			return;
		auto offset = [&](const TCHAR *name) {
			FProperty *p = m_fn->FindProperty(FName(name, FNAME_Find));
			return p ? p->GetOffset_Internal() : -1;
		};
		m_context = offset(STR("WorldContextObject"));
		m_start = offset(STR("Start"));
		m_end = offset(STR("End"));
		m_channel = offset(STR("TraceChannel"));
		m_objectTypes = offset(STR("ObjectTypes"));
		m_radius = offset(STR("Radius"));
		m_outHit = offset(STR("OutHit"));
		m_ignoreActors = offset(STR("ActorsToIgnore"));
		m_ignoreSelf = CastField<FBoolProperty>(m_fn->FindProperty(FName(STR("bIgnoreSelf"), FNAME_Find)));
		m_returnValue = CastField<FBoolProperty>(m_fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
		FProperty *hitProp = m_fn->FindProperty(FName(STR("OutHit"), FNAME_Find));
		auto *hitStruct = hitProp ? static_cast<FStructProperty *>(hitProp)->GetStruct().Get() : nullptr;
		FProperty *impact = hitStruct ? hitStruct->FindProperty(FName(STR("ImpactPoint"), FNAME_Find)) : nullptr;
		m_impactPoint = impact ? impact->GetOffset_Internal() : -1;
		FProperty *component = hitStruct ? hitStruct->FindProperty(FName(STR("Component"), FNAME_Find)) : nullptr;
		m_component = component ? component->GetOffset_Internal() : -1;
		m_hitSize = hitProp ? hitProp->GetSize() : 0;
		m_size = m_fn->GetParmsSize();
		m_ok = m_context >= 0 && m_start >= 0 && m_end >= 0 && (m_channel >= 0 || m_objectTypes >= 0) && m_outHit >= 0 && m_impactPoint >= 0 && m_ignoreSelf &&
			m_returnValue && m_size > 0 && m_size <= 2048;
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] {}: params {} bytes, start +{}, end +{}, hit +{} (+{}), {}\n"), m_name, m_size,
			m_start, m_end, m_outHit, m_impactPoint, m_ok ? STR("ok") : STR("NOT usable"));
	}

	const TCHAR *m_name;
	int32_t m_objectTypes = -1, m_radius = -1;
	bool m_staticOnly = false;
	bool m_resolved = false, m_ok = false;
	UFunction *m_fn = nullptr;
	UObject *m_cdo = nullptr;
	int32_t m_context = -1, m_start = -1, m_end = -1, m_channel = -1, m_outHit = -1, m_impactPoint = -1, m_ignoreActors = -1, m_size = 0;
	FBoolProperty *m_ignoreSelf = nullptr, *m_returnValue = nullptr;
	int32_t m_component = -1, m_hitSize = 0;
	alignas(16) uint8_t m_params[2048] = {};
};

class RoNPassthrough : public CppUserModBase
{
public:
	RoNPassthrough()
	{
		ModName = STR("RoNPassthrough");
		ModVersion = STR("0.1");
		ModDescription = STR("Minecraft passthrough host for Ready or Not");
		ModAuthors = STR("ron-mc-passthrough");
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] loaded\n"));
	}

	~RoNPassthrough() override
	{
		unhook_window();
		g_link = nullptr;
		m_ws.stop();
		compositor::unregister(g_module);
	}

	/// Mods\RoNPassthrough\config.ini (written by the BlockBreach installer's settings page), read once at start:
	/// [blockbreach] mc_scale=25..100, mobs_hunt_player=0|1, squad_fights_mobs=0|1.
	void read_config()
	{
		const std::wstring dir = mod_dir();
		if (dir.empty())
			return;
		const std::wstring ini = dir + L"\\config.ini";
		const int scale = int(GetPrivateProfileIntW(L"blockbreach", L"mc_scale", 75, ini.c_str()));
		m_mcScale = std::clamp(scale, 25, 100) / 100.0;
		m_mobsHuntPlayer = GetPrivateProfileIntW(L"blockbreach", L"mobs_hunt_player", 1, ini.c_str()) != 0;
		m_squadFights = GetPrivateProfileIntW(L"blockbreach", L"squad_fights_mobs", 1, ini.c_str()) != 0;
		log("config: Minecraft at %d%%, mobs hunt the player %s, the squad fights mobs %s", int(m_mcScale * 100.0 + 0.5),
			m_mobsHuntPlayer ? "yes" : "no", m_squadFights ? "yes" : "no");
	}

	auto on_unreal_init() -> void override
	{
		read_config();
		m_ws.start("127.0.0.1", kPort);
		g_link = &m_ws;
		register_keydown_event(Input::Key::NUM_ZERO, [this] { m_toggleBuild = true; });
		compositor::try_register(g_module);
		register_keydown_event(Input::Key::F5, [this] { m_toggle = true; });
		register_keydown_event(Input::Key::F6, [this] { m_placeTest = true; });
		register_keydown_event(Input::Key::F7, [this] {
			m_uiMode = 1 - m_uiMode;
			compositor::set_ui_mode(m_uiMode, m_uiDraw);
			log("composite %s", m_uiMode ? "before the UI" : "at present (over the UI)");
		});
		register_keydown_event(Input::Key::F7, {Input::ModifierKey::CONTROL}, [] { compositor::request_trace(); });
		register_keydown_event(Input::Key::F9, [this] {
			m_poseLag = (m_poseLag + 1) % 3;
			compositor::set_pose_lag(m_poseLag);
			log("pose lag %d", m_poseLag);
		});
		register_keydown_event(Input::Key::F9, {Input::ModifierKey::CONTROL}, [this] {
			m_rollSign = -m_rollSign;
			log("roll sign %d", m_rollSign);
		});
		if (crash_guard::previous_run_crashed_in_early_render())
		{
			// the last session crashed while drawing Minecraft under RoN's interface: draw over it this time instead
			m_uiMode = 0;
			log("SAFE MODE: the previous run crashed in early rendering (crash_breadcrumbs.log); compositing at present, F7 switches back");
		}
		compositor::set_ui_mode(m_uiMode, m_uiDraw);
		compositor::set_pose_lag(m_poseLag);
		Hook::RegisterEngineTickPostCallback([this](auto &, UEngine *engine, float, bool) { tick(engine); },
			{false, false, STR("RoNPassthrough"), STR("Camera")});
		log("ready: NumPad 0 build mode, F5 on/off, F6 test blocks, F7 before/over UI, F9 pose lag, Ctrl+F7 frame trace");
	}

private:
	WsClient m_ws;
	int m_generation = 0;
	std::atomic<bool> m_toggle{false}, m_placeTest{false};
	bool m_enabled = true;
	int m_uiMode = 1, m_uiDraw = 2, m_poseLag = 0, m_rollSign = -1;
	int m_viewSent = 0;
	// Minecraft's render size relative to RoN's picture (op mcscale). 75 %: measured in "Twisted Nerve" (laptop,
	// 1920x1200) RoN runs at 66 fps instead of 55 at 100 %, and the pixel-art blocks look the same (50 %: 73 fps,
	// slightly coarser edges).
	double m_mcScale = 0.75;
	bool m_haveOffset = false;
	double m_yOffset = 0.0;
	double m_xOff = 0.0, m_zOff = 0.0; // blocks: this RoN map's part of Minecraft's world
	StringType m_mapName;              // the map those offsets are for

	// UE (cm) <-> Minecraft (blocks)
	double mcx(double ux) const { return ux / 100.0 + m_xOff; }
	double mcy(double uz) const { return uz / 100.0 + m_yOffset; }
	double mcz(double uy) const { return uy / 100.0 + m_zOff; }
	double uex(double mx) const { return (mx - m_xOff) * 100.0; }
	double uey(double mz) const { return (mz - m_zOff) * 100.0; }
	double uez(double my) const { return (my - m_yOffset) * 100.0; }

	/// Every RoN map gets its own Minecraft region: 4096 blocks apart along x, slot from the map's package name.
	void map_offset(UObject *pawn)
	{
		UObject *package = pawn ? pawn->GetOutermost() : nullptr;
		const StringType name = package ? package->GetName() : StringType();
		m_mapName = name;
		uint32_t hash = 2166136261u;
		for (auto ch : name)
			hash = (hash ^ uint32_t(ch)) * 16777619u;
		// a 128 x 128 grid of regions 4096 blocks apart (one row of 128 had two DLC maps in the same place: Home and
		// Prison, 2026-10-06; none of the 31 missions collide on the grid)
		const int slotX = int(hash % 128u) - 64, slotZ = int((hash / 128u) % 128u) - 64;
		m_xOff = slotX * 4096.0;
		m_zOff = slotZ * 4096.0;
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] map {} -> Minecraft region x {} z {}\n"), name, int(m_xOff), int(m_zOff));
		std::string narrow(name.begin(), name.end());
		log("map %s -> Minecraft region x %d z %d", narrow.c_str(), int(m_xOff), int(m_zOff));
	}
	uint64_t m_frame = 0;
	bool m_loggedChain = false;
	int m_aspectConstraint = -1;
	int m_camFrames = 0;
	bool m_testFar = false;
	UObject *m_leveledPawn = nullptr; // compared only, never dereferenced
	float m_nearClip = 0.001f; // RoN's near clip plane: 1 mm (measured with the depth bands; UE's default is 10 cm)

	Member<UObject *> m_gameViewport{STR("GameViewport")};
	Member<UObject *> m_gameInstance{STR("GameInstance")};
	Member<RawPtrArray> m_localPlayers{STR("LocalPlayers")};
	Member<UObject *> m_playerController{STR("PlayerController")};
	Member<UObject *> m_cameraManager{STR("PlayerCameraManager")};
	Member<uint8_t> m_aspectAxis{STR("AspectRatioAxisConstraint")};
	Member<UObject *> m_pawn{STR("AcknowledgedPawn")};
	BoolMember m_showCursor{STR("bShowMouseCursor")};
	UClass *m_pcClass = nullptr;
	bool m_menuController = false;
	int m_inGame = -1;      // last state logged
	int m_inGameAlone = -1; // last state logged while Minecraft isn't connected
	LineTracer m_tracer;                                           // Visibility: projectiles, what the player hits
	LineTracer m_ground{STR("LineTraceSingleForObjects")};         // the level only (characters don't count as floor)
	// static geometry only: an open door (WorldDynamic) must not leave a permanent wall in its doorway
	LineTracer m_room{STR("SphereTraceSingleForObjects"), true};         // is there room for a mob to stand
	LineTracer m_edge{STR("LineTraceSingleForObjects"), true};           // a wall between two column centres
	std::vector<std::pair<int, int>> m_spiral;
	std::unordered_set<int64_t> m_sampled;
	std::vector<std::pair<int, int>> m_suspectCells; // Minecraft columns the suspects stand in (from send_peds)
	double m_levelFeetZ = 0.0;                        // UE z of the player's feet when the ground was levelled
	int m_groundSent = 0;
	int m_wallColumns = 0;
	// "walls" sent per column (x, z) -> the floor tops they stand on; and where RoN's player or AI walked (block_key of
	// x, floor top, z): a column someone walked through is passable, its wall a false one (a doorway on a cell edge)
	std::unordered_map<int64_t, std::vector<int>> m_wallTops;
	std::unordered_map<int64_t, std::vector<int>> m_floorTops; // every storey's floor block per column (never opened)
	std::unordered_map<int64_t, std::vector<std::pair<int, int>>> m_pillars; // solid things sent per column (bottom, top)
	std::unordered_map<int64_t, std::vector<int>> m_openedTops;          // walls opened where people walked
	std::unordered_set<int64_t> m_walked;
	int m_wallsOpened = 0;
	int m_edgeWalls = 0; // walls found between column centres (the edge test), per level
	int64_t m_probeTicks = 0; // QueryPerformanceCounter ticks spent probing the ground, since the last stats line
	int m_probeFrames = 0;
	int64_t m_probeDeadline = 0; // this tick's probing ends at this QueryPerformanceCounter value
	int m_probesThisTick = 0;
	std::atomic<bool> m_toggleBuild{false};
	// Minecraft's blocks as RoN collision: one actor of ours with a box component per block (keyed by block position)
	Ref m_blockActor;
	UObject *m_blockActorPawn = nullptr; // the pawn (level) it was made in; compared only
	std::unordered_map<int64_t, Ref> m_blockBoxes;
	UObject *m_lastPawn = nullptr;
	UObject *m_pc = nullptr;
	FVectorD m_camLoc{}, m_camFwd{1, 0, 0};
	struct Projectile
	{
		FVectorD at;
		bool firework, seen;
	};
	std::unordered_map<int, Projectile> m_projectiles;
	// RoN's people as Minecraft "peds": stable handles per character object, the current list by handle
	std::unordered_map<UObject *, int> m_pedHandle;
	std::unordered_map<int, Ref> m_pedByHandle;
	std::unordered_map<UClass *, bool> m_pedClassOk; // RoN's suspects only (not the SWAT team, civilians, trailers)
	std::unordered_set<UClass *> m_walkerClass;      // classes of RoN's real people (not the trailers' dummies)
	bool m_targetsAll = false;
	int m_nextPed = 1;
	Member<UObject *> m_pedHealth{STR("CharacterHealth")};
	Member<float> m_pedResource{STR("Resource")};
	Member<UObject *> m_pedRoot{STR("RootComponent")};
	Member<FVectorD> m_pedLocation{STR("RelativeLocation")};
	Member<UObject *> m_pedController{STR("Controller")};
	struct Mob
	{
		int id;
		FVectorD at;          // UE, feet
		double half, height;  // bounding box, cm: half its width, its height
		bool hostile;         // a fighter (RoN's people shoot only those; the player shoots anything)
	};
	std::vector<Mob> m_mobs;
	// the player's gun: shots counted from its magazine (same magazine, fewer rounds, not reloading) hit Minecraft's mobs
	Ref m_weapon;
	int m_weaponMag = -1;
	float m_weaponAmmo = -1.0f;
	int m_playerShots = 0, m_playerMobHits = 0;
	bool m_mobsHuntPlayer = true; // the player has a proxy too: Minecraft's mobs come for RoN's player ({"op":"hunt"})
	bool m_squadFights = true;    // the SWAT team are mobs' targets and shoot back, like the suspects
	uint64_t m_resyncAt = 0;
	std::atomic<bool> m_relevel{false};
	std::atomic<bool> m_probe{false};
	std::mutex m_consoleLock;
	std::vector<std::wstring> m_consoleQueue;
	std::atomic<int> m_mapRequest{0};
	std::atomic<int> m_colsRequest{0};
	std::atomic<int> m_probeColRequest{-1};
	bool m_probeDebug = false;
	Member<UObject *> m_rootComponent{STR("RootComponent")};
	Member<UObject *> m_movement{STR("CharacterMovement")};
	Member<uint8_t> m_movementMode{STR("MovementMode")};
	Member<UObject *> m_capsule{STR("CapsuleComponent")};
	Member<FVectorD> m_relativeLocation{STR("RelativeLocation")};
	Member<float> m_capsuleHalfHeight{STR("CapsuleHalfHeight")};
	PovLayout m_pov;

	static void log(const char *format, ...)
	{
		char buffer[512];
		va_list args;
		va_start(args, format);
		std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		std::wstring wide(buffer, buffer + std::strlen(buffer));
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] {}\n"), wide);
		history(buffer);
	}

	bool send(const char *format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		return m_ws.send(buffer);
	}

	/// Ops that only change the mod's own settings (no UObject, no game call): safe in any state.
	static bool settings_op(const std::string &m)
	{
		if (m.find("\"t\":\"gta\"") == std::string::npos)
			return false;
		for (const char *op : {"toggle", "trace", "uimode", "uidraw", "lag", "roll", "debug", "dscale", "near", "dcopy", "console", "build", "mcscale",
				 "relevel", "targets", "map", "probe"})
			if (m.find(std::string("\"op\":\"") + op + "\"") != std::string::npos)
				return true;
		return false;
	}

	/// Test ops relayed by the Minecraft mod from a script on the link: {"t":"gta","op":"test|relevel|build|ai|hurt|uimode|uidraw|lag|roll|trace|toggle|debug|dscale(1/1000)|near(mm)|dcopy","v":n}
	void handle_op(const std::string &m)
	{
		crash_guard::Scope scope("op");
		crash_guard::note("op %.80s", m.c_str());
		auto has = [&](const char *op) { return m.find(std::string("\"op\":\"") + op + "\"") != std::string::npos; };
		if (m.find("\"t\":\"blocks\"") != std::string::npos)
		{
			on_blocks(json_ints(m, "set"), json_ints(m, "clear"));
			return;
		}
		if (m.find("\"t\":\"explosion\"") != std::string::npos)
		{
			on_explosion(m);
			return;
		}
		if (m.find("\"t\":\"proj\"") != std::string::npos)
		{
			on_projectiles(m);
			return;
		}
		if (m.find("\"t\":\"melee\"") != std::string::npos)
		{
			on_melee();
			return;
		}
		if (m.find("\"t\":\"mobhit\"") != std::string::npos)
		{
			on_mobhit(m);
			return;
		}
		if (m.find("\"t\":\"mobs\"") != std::string::npos)
		{
			on_mobs(m);
			return;
		}
		if (m.find("\"t\":\"screen\"") != std::string::npos)
		{
			const bool open = m.find("\"open\":true") != std::string::npos;
			if (open && !g_mcScreen)
			{
				g_cursorX = 0.5f;
				g_cursorY = 0.5f;
				g_cursorDirty = true;
			}
			// Minecraft repeats its state every 2 s: logged only when it changes
			if (g_mcScreen.exchange(open) != open)
				log("Minecraft screen %s", open ? "open: the mouse steers its cursor" : "closed");
			return;
		}
		if (m.find("\"t\":\"gta\"") == std::string::npos)
			return;
		if (has("relevel"))
		{
			m_relevel = true;
			return;
		}
		if (has("probe"))
		{
			m_probe = true;
			return;
		}
		if (has("build"))
		{
			m_toggleBuild = true;
			return;
		}
		int v = -1;
		if (const size_t at = m.find("\"v\":"); at != std::string::npos)
			v = std::atoi(m.c_str() + at + 4);
		if (has("god") && v >= 0)
		{
			god_mode(v > 0);
			return;
		}
		if (has("freeze") && v >= 0)
		{
			freeze_squad(v > 0);
			return;
		}
		if (has("map"))
		{
			m_mapRequest = v > 0 ? v : 12;
			return;
		}
		if (has("probecol") && v >= 0)
		{
			// one column, every trace logged: v = (dx + 50) * 100 + (dz + 50) from the player's column
			m_probeColRequest = v;
			return;
		}
		if (has("cols"))
		{
			m_colsRequest = v > 0 ? v : 2;
			return;
		}
		if (has("mobs") && v > 0)
		{
			send("{\"t\":\"spawnmobs\",\"k\":\"zombie\",\"n\":%d,\"rmin\":6,\"rmax\":14,\"arc\":60}", v);
			log("spawning %d zombies", v);
			return;
		}
		if (has("mobsat") && v > 0)
		{
			mobs_at_suspect(v);
			return;
		}
		if (has("targets") && v >= 0)
		{
			m_targetsAll = v > 0;
			m_pedClassOk.clear();
			m_walkerClass.clear();
			log("mob targets: %s", m_targetsAll ? "every AI person" : "suspects only");
			return;
		}
		if (has("hunt") && v >= 0)
		{
			m_mobsHuntPlayer = v > 0;
			log("mobs hunt the player: %s", m_mobsHuntPlayer ? "yes" : "no");
			return;
		}
		if (has("mobsclear"))
		{
			m_ws.send("{\"t\":\"mobsclear\"}");
			return;
		}
		if (has("ai"))
		{
			list_characters();
			return;
		}
		if (has("pose"))
		{
			// the camera right now (scripts turn the player and want to know where it looks)
			const double yaw = std::atan2(m_camFwd.y, m_camFwd.x) * 180.0 / 3.14159265358979;
			const double pitch = std::asin(std::clamp(m_camFwd.z, -1.0, 1.0)) * 180.0 / 3.14159265358979;
			log("pose: cam UE (%.0f, %.0f, %.0f) yaw %.2f pitch %.2f -> MC (%.2f, %.2f, %.2f)", m_camLoc.x, m_camLoc.y, m_camLoc.z, yaw, pitch,
				mcx(m_camLoc.x), mcy(m_camLoc.z), mcz(m_camLoc.y));
			return;
		}
		if (has("state"))
		{
			// why the host is (not) "in game": each condition of in_game()
			UObject **pawn = m_pc ? m_pawn.in(m_pc) : nullptr;
			log("state: pc %p (%ls) pawn %p, menu controller %d, cursor %d, paused %d, others in session %d, in game %d",
				static_cast<void *>(m_pc), m_pc ? m_pc->GetClassPrivate()->GetName().c_str() : STR("-"), pawn ? static_cast<void *>(*pawn) : nullptr,
				int(m_menuController), m_pc ? int(m_showCursor.get(m_pc, false)) : -1, m_pc ? int(game_paused(m_pc)) : -1, int(multiplayer()),
				int(g_inGame.load()));
			return;
		}
		if (has("aim"))
		{
			aim_at_mob();
			return;
		}
		if (has("pshot"))
		{
			// a test shot: what one round from the player's gun does to Minecraft's mobs (no RoN input needed)
			if (player_alive())
				shot_along_camera(false);
			return;
		}
		if (has("look"))
		{
			// {"t":"gta","op":"look","yaw":d,"pitch":d}: turn the view by these degrees
			const std::vector<double> yaw = json_doubles("{\"y\":[" + json_number(m, "yaw") + "]}", "y");
			const std::vector<double> pitch = json_doubles("{\"p\":[" + json_number(m, "pitch") + "]}", "p");
			look_by(yaw.empty() ? 0.0 : yaw[0], pitch.empty() ? 0.0 : pitch[0], m.find("\"abs\":1") != std::string::npos);
			return;
		}
		if (has("console"))
		{
			// {"t":"gta","op":"console","c":"r.ScreenPercentage 50"}: a RoN console command, run on the next tick
			const size_t at = m.find("\"c\":\"");
			if (at != std::string::npos)
			{
				const size_t end = m.find('"', at + 5);
				const std::string cmd = m.substr(at + 5, end - at - 5);
				std::lock_guard<std::mutex> lock(m_consoleLock);
				m_consoleQueue.emplace_back(cmd.begin(), cmd.end());
			}
			return;
		}
		if (has("shoot") && v >= 0)
		{
			// an arrow's path from the camera through character v's chest, as if Minecraft had reported it
			const std::vector<UObject *> list = characters();
			if (v < int(list.size()))
			{
				UObject **root = m_pedRoot.in(list[v]);
				FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr;
				if (at != nullptr)
				{
					const FVectorD aim{at->x, at->y, at->z + 30.0};
					const double dx = aim.x - m_camLoc.x, dy = aim.y - m_camLoc.y, dz = aim.z - m_camLoc.z;
					const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
					const std::string before = health_of(list[v]);
					projectile_segment(-1, Projectile{m_camLoc, false, true},
						FVectorD{aim.x + dx / len * 100.0, aim.y + dy / len * 100.0, aim.z + dz / len * 100.0});
					log("shoot [%d] %ls: health before %s", v, list[v]->GetClassPrivate()->GetName().c_str(), before.substr(0, 200).c_str());
				}
			}
			return;
		}
		if (has("hurt") && v >= 0)
		{
			hurt_character(v);
			return;
		}
		if (has("test"))
		{
			m_testFar = v > 0;
			m_placeTest = true;
		}
		else if (has("toggle"))
			m_toggle = true;
		else if (has("trace"))
			compositor::request_trace();
		else if (has("uimode") && v >= 0)
		{
			m_uiMode = v;
			compositor::set_ui_mode(m_uiMode, m_uiDraw);
			log("composite %s", m_uiMode ? "before the UI" : "at present (over the UI)");
		}
		else if (has("uidraw") && v >= 1)
		{
			m_uiDraw = v;
			compositor::set_ui_mode(m_uiMode, m_uiDraw);
			log("UI starts at backbuffer draw %d", m_uiDraw);
		}
		else if (has("lag") && v >= 0)
		{
			m_poseLag = v;
			compositor::set_pose_lag(m_poseLag);
			log("pose lag %d", m_poseLag);
		}
		else if (has("dcopy") && v >= 0)
		{
			compositor::set_depth_copy_mode(v);
			log("depth copy mode %d", v);
		}
		else if (has("debug") && v >= 0)
			compositor::set_debug(v, -1.0f);
		else if (has("dscale") && v > 0)
			compositor::set_debug(-1, v / 1000.0f);
		else if (has("near") && v > 0)
		{
			m_nearClip = v / 1000.0f;
			log("host near plane %.3f m", m_nearClip);
		}
		else if (has("mcscale") && v >= 25 && v <= 100)
		{
			// Minecraft's render size, % of RoN's picture (up to 1920x1080): less GPU for Minecraft, softer blocks
			m_mcScale = v / 100.0;
			m_viewSent = 0;
			log("Minecraft renders at %d%% of RoN's picture", v);
		}
		else if (has("roll") && v >= 0)
		{
			m_rollSign = v > 0 ? 1 : -1;
			log("roll sign %d", m_rollSign);
		}
	}

	static int64_t block_key(int x, int y, int z)
	{
		return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF);
	}

	/// Our block actor if it belongs to the current level and still exists.
	UObject *live_block_actor() const
	{
		return m_blockActorPawn != nullptr && m_blockActorPawn == m_lastPawn ? m_blockActor.get() : nullptr;
	}

	/// Our actor holding the block boxes, spawned on first use in the current level (a plain AActor with a scene root
	/// at the origin, so each box's relative transform is its world transform).
	UObject *block_actor()
	{
		if (UObject *live = live_block_actor())
			return live;
		m_blockActor = Ref();
		m_blockBoxes.clear();
		if (m_lastPawn == nullptr)
			return nullptr;
		const ue::Transform identity;
		ue::Call begin(STR("/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass"));
		begin.set(STR("WorldContextObject"), m_lastPawn)
			.set(STR("ActorClass"), ue::find(STR("/Script/Engine.Actor")))
			.set(STR("SpawnTransform"), identity)
			.set(STR("CollisionHandlingOverride"), uint8_t(1)); // AlwaysSpawn
		if (!begin.run())
			return nullptr;
		UObject *actor = begin.get<UObject *>(STR("ReturnValue"));
		if (actor == nullptr)
		{
			log("could not spawn the block actor");
			return nullptr;
		}
		ue::Call finish(STR("/Script/Engine.GameplayStatics:FinishSpawningActor"));
		finish.set(STR("Actor"), actor).set(STR("SpawnTransform"), identity).run();
		ue::Call root(STR("/Script/Engine.Actor:AddComponentByClass"));
		root.set(STR("Class"), ue::find(STR("/Script/Engine.SceneComponent")))
			.set(STR("bManualAttachment"), false)
			.set(STR("RelativeTransform"), identity)
			.set(STR("bDeferredFinish"), false)
			.run(actor);
		m_blockActor = Ref(actor);
		m_blockActorPawn = m_lastPawn;
		log("block actor spawned");
		return actor;
	}

	void add_block_box(int x, int y, int z)
	{
		const int64_t key = block_key(x, y, z);
		if (m_blockBoxes.count(key))
			return;
		if (m_blockBoxes.size() >= size_t(kMaxBlocks))
		{
			static uint64_t warnedAt = 0;
			if (m_frame - warnedAt > 600 || warnedAt == 0)
			{
				warnedAt = m_frame;
				log("block limit (%d): new Minecraft blocks are not solid in RoN", kMaxBlocks);
			}
			return;
		}
		UObject *actor = block_actor();
		if (actor == nullptr)
			return;
		static UObject *boxClass = ue::find(STR("/Script/Engine.BoxComponent"));
		ue::Transform t;
		t.tx = uex(x + 0.5);
		t.ty = uey(z + 0.5);
		t.tz = uez(y + 0.5);
		ue::Call add(STR("/Script/Engine.Actor:AddComponentByClass"));
		add.set(STR("Class"), boxClass).set(STR("bManualAttachment"), false).set(STR("RelativeTransform"), t).set(STR("bDeferredFinish"), false).run(actor);
		UObject *box = add.get<UObject *>(STR("ReturnValue"));
		if (box == nullptr)
			return;
		ue::Call extent(STR("/Script/Engine.BoxComponent:SetBoxExtent"));
		extent.set(STR("InBoxExtent"), ue::Vec{50.0, 50.0, 50.0}).set(STR("bUpdateOverlaps"), true).run(box);
		ue::Call profile(STR("/Script/Engine.PrimitiveComponent:SetCollisionProfileName"));
		profile.name(STR("InCollisionProfileName"), STR("BlockAll")).set(STR("bUpdateOverlaps"), true).run(box);
		m_blockBoxes.emplace(key, Ref(box));
	}

	void remove_block_box(int x, int y, int z)
	{
		auto it = m_blockBoxes.find(block_key(x, y, z));
		if (it == m_blockBoxes.end())
			return;
		if (UObject *box = live_block_actor() ? it->second.get() : nullptr)
		{
			ue::Call destroy(STR("/Script/Engine.ActorComponent:K2_DestroyComponent"));
			destroy.set(STR("Object"), box).run(box);
		}
		m_blockBoxes.erase(it);
	}

	/// All block boxes away (the ground was re-levelled: their heights changed); Minecraft re-sends the blocks.
	void reset_blocks()
	{
		if (live_block_actor() != nullptr)
			for (auto &[key, ref] : m_blockBoxes)
				if (UObject *box = ref.get())
				{
					ue::Call destroy(STR("/Script/Engine.ActorComponent:K2_DestroyComponent"));
					destroy.set(STR("Object"), box).run(box);
				}
		m_blockBoxes.clear();
		m_ws.send("{\"t\":\"blocksync\",\"r\":48}");
		m_resyncAt = m_frame + 300; // again in ~3 s: right after joining, Minecraft's player isn't there to answer yet
	}

	void on_blocks(const std::vector<int> &set, const std::vector<int> &clear)
	{
		if (!m_haveOffset || m_lastPawn == nullptr)
			return;
		for (size_t i = 0; i + 2 < clear.size(); i += 3)
			remove_block_box(clear[i], clear[i + 1], clear[i + 2]);
		for (size_t i = 0; i + 2 < set.size(); i += 3)
			add_block_box(set[i], set[i + 1], set[i + 2]);
		log("blocks: +%d -%d, %d boxes in RoN", int(set.size() / 3), int(clear.size() / 3), int(m_blockBoxes.size()));
	}

	static bool health_name(const StringType &name)
	{
		StringType lower = name;
		for (auto &ch : lower)
			ch = wchar_t(std::towlower(ch));
		return lower.find(STR("health")) != StringType::npos || lower.find(STR("dead")) != StringType::npos ||
			lower.find(STR("alive")) != StringType::npos || lower.find(STR("incapac")) != StringType::npos ||
			lower == STR("resource") || lower == STR("maxresource");
	}

	/// The health-like UPROPERTYs of an object (floats, ints, bools named *health*, *dead*, *alive*, *incapac*), and,
	/// one level down, of the objects it points to through properties named that way (health components).
	static std::string health_of(UObject *object, int depth = 0)
	{
		std::string out;
		if (object == nullptr)
			return out;
		for (FProperty *prop : TFieldRange<FProperty>(object->GetClassPrivate(), EFieldIterationFlags::IncludeSuper))
		{
			const StringType name = prop->GetName();
			if (!health_name(name))
				continue;
			char buffer[128];
			buffer[0] = 0;
			if (auto *f = CastField<FFloatProperty>(prop))
				std::snprintf(buffer, sizeof(buffer), "%ls=%.1f ", name.c_str(), *f->ContainerPtrToValuePtr<float>(object));
			else if (auto *d = CastField<FDoubleProperty>(prop))
				std::snprintf(buffer, sizeof(buffer), "%ls=%.1f ", name.c_str(), *d->ContainerPtrToValuePtr<double>(object));
			else if (auto *i = CastField<FIntProperty>(prop))
				std::snprintf(buffer, sizeof(buffer), "%ls=%d ", name.c_str(), *i->ContainerPtrToValuePtr<int32_t>(object));
			else if (auto *b = CastField<FBoolProperty>(prop))
				std::snprintf(buffer, sizeof(buffer), "%ls=%d ", name.c_str(), int(b->GetPropertyValueInContainer(object)));
			else if (auto *o = CastField<FObjectProperty>(prop); o && depth == 0)
			{
				UObject *sub = *o->ContainerPtrToValuePtr<UObject *>(object);
				std::snprintf(buffer, sizeof(buffer), "%ls->{ ", name.c_str());
				out += buffer;
				out += health_of(sub, 1);
				out += "} ";
				continue;
			}
			out += buffer;
		}
		return out;
	}

	std::vector<UObject *> characters()
	{
		std::vector<UObject *> all;
		UObjectGlobals::FindAllOf(STR("Character"), all);
		std::vector<UObject *> out;
		for (UObject *c : all)
			if (c != nullptr && c->GetName().find(STR("Default__")) == StringType::npos)
				out.push_back(c);
		return out;
	}

	/// Logs RoN's characters: index, class, position (UE), health properties. ({"op":"ai"})
	void list_characters()
	{
		const std::vector<UObject *> list = characters();
		log("%d characters", int(list.size()));
		for (size_t i = 0; i < list.size() && i < 40; ++i)
		{
			UObject *c = list[i];
			UObject **root = m_rootComponent.in(c);
			FVectorD *at = root && *root ? m_relativeLocation.in(*root) : nullptr;
			const StringType cls = c->GetClassPrivate()->GetName();
			log("  [%d] %ls at (%.0f, %.0f, %.0f)%s %s", int(i), cls.c_str(), at ? at->x : 0.0, at ? at->y : 0.0, at ? at->z : 0.0,
				c == m_lastPawn ? " (player)" : "", health_of(c).c_str());
		}
	}

	/// Every ~0.2 s: RoN's living AI characters (not the player) as Minecraft "peds", feet positions in Minecraft
	/// coordinates: [[handle,x,y,z],...].
	void send_peds()
	{
		crash_guard::Scope scope("tick_send_peds");
		m_pedByHandle.clear();
		m_suspectCells.clear();
		std::string list;
		std::string opens;
		for (UObject *c : characters())
		{
			if (c == m_lastPawn)
				continue;
			UObject **health = m_pedHealth.in(c);
			float *resource = health && *health ? m_pedResource.in(*health) : nullptr;
			if (resource != nullptr && *resource <= 0.0f)
				continue; // dead (a body on the floor is no proof of a passable column either)
			UObject **root = m_pedRoot.in(c);
			FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr;
			if (at == nullptr)
				continue;
			UClass *cls = c->GetClassPrivate();
			auto ok = m_pedClassOk.find(cls);
			if (ok == m_pedClassOk.end())
			{
				const StringType name = cls->GetName();
				const bool suspect = name.find(STR("Suspect")) != StringType::npos;
				const bool trailer = name.find(STR("Trailer")) != StringType::npos;
				if (!trailer)
					m_walkerClass.insert(cls);
				// suspects and the SWAT team fight Minecraft's mobs; "targets 1": civilians too. Never the trailers' dummies
				const bool swat = name.find(STR("Swat")) != StringType::npos;
				ok = m_pedClassOk.emplace(cls, !trailer && (suspect || (swat && m_squadFights) || m_targetsAll)).first;
			}
			// every living AI person walking (the squad clears every doorway): its column is passable
			if (m_walkerClass.count(cls) && on_ground(c))
				walked(FVectorD{at->x, at->y, at->z - 90.0}, opens);
			if (!ok->second)
				continue;
			auto [it, added] = m_pedHandle.try_emplace(c, m_nextPed);
			if (added)
				++m_nextPed;
			m_pedByHandle[it->second] = Ref(c);
			m_suspectCells.emplace_back(int(std::floor(mcx(at->x))), int(std::floor(mcz(at->y))));
			char entry[96];
			// capsule centre -> feet (RoN's characters: ~90 cm half height)
			std::snprintf(entry, sizeof(entry), "%s[%d,%.3f,%.3f,%.3f]", list.empty() ? "" : ",", it->second, mcx(at->x),
				mcy(at->z - 90.0), mcz(at->y));
			list += entry;
		}
		// the player: handle 0, never in m_pedByHandle (it doesn't turn or fire on its own)
		if (m_mobsHuntPlayer && player_alive())
		{
			UObject **root = m_pedRoot.in(m_lastPawn);
			if (FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr)
			{
				char entry[96];
				std::snprintf(entry, sizeof(entry), "%s[0,%.3f,%.3f,%.3f]", list.empty() ? "" : ",", mcx(at->x), mcy(at->z - 90.0), mcz(at->y));
				list += entry;
			}
		}
		m_ws.send("{\"t\":\"peds\",\"p\":[" + list + "]}");
		send_opens(opens);
	}

	/// {"t":"mobs","m":[[id,"zombie",x,y,z],...]}: Minecraft's fighting mobs near the player (feet, Minecraft coordinates).
	void on_mobs(const std::string &m)
	{
		m_mobs.clear();
		const char *at = std::strstr(m.c_str(), "\"m\":[");
		while (at && (at = std::strchr(at + 1, '[')) != nullptr)
		{
			int id = 0, hostile = 1;
			char kind[32] = {};
			double x, y, z, w = 0.6, h = 1.95;
			// [id,"zombie",x,y,z] or, from newer Minecraft mods, [id,"zombie",x,y,z,width,height,hostile]
			const int n = sscanf_s(at, "[%d,\"%31[^\"]\",%lf,%lf,%lf,%lf,%lf,%d]", &id, kind, unsigned(sizeof(kind)), &x, &y, &z, &w, &h, &hostile);
			if (n < 5)
				continue;
			const ue::Vec u = to_ue(x, y, z);
			m_mobs.push_back(Mob{id, FVectorD{u.x, u.y, u.z}, w * 50.0, h * 100.0, hostile != 0});
		}
	}

	/// The nearest mob a ray from `from` along the unit vector `dir` passes through (its bounding box) before RoN's world
	/// (walls, people, Minecraft's blocks as boxes) stops it, within `range` cm. Null if none; `t` = distance, `head` =
	/// it hit the top fifth of the box.
	const Mob *mob_on_ray(const FVectorD &from, const FVectorD &dir, double range, double &t, bool &head)
	{
		FVectorD wall{};
		double limit = range;
		if (m_tracer.trace(m_lastPawn, from, FVectorD{from.x + dir.x * range, from.y + dir.y * range, from.z + dir.z * range}, wall))
			limit = std::sqrt((wall.x - from.x) * (wall.x - from.x) + (wall.y - from.y) * (wall.y - from.y) + (wall.z - from.z) * (wall.z - from.z));
		const Mob *best = nullptr;
		t = limit;
		for (const Mob &mob : m_mobs)
		{
			const double lo[3] = {mob.at.x - mob.half, mob.at.y - mob.half, mob.at.z};
			const double hi[3] = {mob.at.x + mob.half, mob.at.y + mob.half, mob.at.z + mob.height};
			const double o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
			double tmin = 0.0, tmax = t;
			bool miss = false;
			for (int i = 0; i < 3 && !miss; ++i)
			{
				if (std::abs(d[i]) < 1e-9)
				{
					miss = o[i] < lo[i] || o[i] > hi[i];
					continue;
				}
				double a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
				if (a > b)
					std::swap(a, b);
				tmin = std::max(tmin, a);
				tmax = std::min(tmax, b);
				miss = tmin > tmax;
			}
			if (miss || tmin >= t)
				continue;
			t = tmin;
			best = &mob;
			head = from.z + dir.z * tmin > mob.at.z + mob.height * 0.8;
		}
		return best;
	}

	/// The player's view straight at the nearest Minecraft mob's chest it can see (for tests and staged shots).
	/// ({"op":"aim"})
	void aim_at_mob()
	{
		if (m_pc == nullptr || m_mobs.empty())
		{
			log("aim: no mobs");
			return;
		}
		const Mob *best = nullptr;
		double bestD = 1e18;
		for (const Mob &mob : m_mobs)
		{
			const FVectorD chest{mob.at.x, mob.at.y, mob.at.z + mob.height * 0.6};
			const double dx = chest.x - m_camLoc.x, dy = chest.y - m_camLoc.y, dz = chest.z - m_camLoc.z;
			const double d = dx * dx + dy * dy + dz * dz;
			FVectorD hit{};
			if (d < bestD && !m_tracer.trace(m_lastPawn, m_camLoc, chest, hit))
			{
				bestD = d;
				best = &mob;
			}
		}
		if (best == nullptr)
		{
			log("aim: no mob in sight");
			return;
		}
		const double dx = best->at.x - m_camLoc.x, dy = best->at.y - m_camLoc.y, dz = best->at.z + best->height * 0.6 - m_camLoc.z;
		const double yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979;
		const double pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.0 / 3.14159265358979;
		ue::Call set(STR("/Script/Engine.Controller:SetControlRotation"));
		set.set(STR("NewRotation"), FRotatorD{pitch, yaw, 0.0}).run(m_pc);
		log("aim: mob %d at %.1f m (yaw %.1f pitch %.1f)", best->id, std::sqrt(bestD) / 100.0, yaw, pitch);
	}

	/// Every tick in the game (not in build mode): a round gone from the player's magazine (the same magazine, not
	/// reloading) is a shot, and a shot along the camera's line hits the first Minecraft mob in it before any wall.
	void player_shots(UObject *pawn)
	{
		crash_guard::Scope scope("tick_player_shots");
		static UClass *magWeapon = static_cast<UClass *>(ue::find(STR("/Script/ReadyOrNot.BaseMagazineWeapon")));
		static Member<int32_t> magIndex{STR("MagIndex")};
		static Member<int32_t> pellets{STR("SpawnProjectileCount")};
		static BoolMember reloading{STR("bReloading")};
		ue::Call item(STR("/Script/ReadyOrNot.ReadyOrNotCharacter:GetEquippedItem"));
		item.run(pawn);
		UObject *weapon = Ref(item.get<UObject *>(STR("ReturnValue"))).get();
		if (weapon == nullptr || magWeapon == nullptr || !weapon->IsA(magWeapon))
		{
			m_weapon = Ref();
			return;
		}
		ue::Call ammoCall(STR("/Script/ReadyOrNot.BaseMagazineWeapon:GetAmmo"));
		ammoCall.run(weapon);
		const float ammo = ammoCall.get<float>(STR("ReturnValue"));
		int32_t *mag = magIndex.in(weapon);
		const int magNow = mag ? *mag : -1;
		const bool same = m_weapon.get() == weapon && magNow == m_weaponMag && !reloading.get(weapon, false);
		const int shots = same && ammo < m_weaponAmmo - 0.5f ? std::min(3, int(m_weaponAmmo - ammo + 0.5f)) : 0;
		m_weapon = Ref(weapon);
		m_weaponMag = magNow;
		m_weaponAmmo = ammo;
		if (shots == 0)
			return;
		int32_t *count = pellets.in(weapon);
		const bool shotgun = count && *count > 1;
		for (int i = 0; i < shots; ++i)
			shot_along_camera(shotgun);
	}

	/// One of the player's shots along the camera's line: the first mob in it before any wall takes the hit.
	void shot_along_camera(bool shotgun)
	{
		++m_playerShots;
		double t = 0.0;
		bool head = false;
		const Mob *mob = m_mobs.empty() ? nullptr : mob_on_ray(m_camLoc, m_camFwd, 15000.0, t, head);
		if (mob == nullptr)
		{
			log("player shot: no mob in the line");
			return;
		}
		++m_playerMobHits;
		// a zombie has 20 health: three rifle rounds, one to the head; a shotgun shell close up
		const double damage = (shotgun ? 16.0 : 7.0) * (head ? 3.0 : 1.0);
		send("{\"t\":\"mobdmg\",\"id\":%d,\"d\":%.1f,\"p\":1}", mob->id, damage);
		log("player shot mob %d (%.1f m)%s: %.0f damage", mob->id, t / 100.0, head ? ", head" : "", damage);
	}

	/// The player's view turned by (yaw, pitch) degrees from where it looks now (tests turn without RoN's input).
	/// With `absolute`, dpitch is the pitch itself (degrees, up positive).
	void look_by(double dyaw, double dpitch, bool absolute = false)
	{
		if (m_pc == nullptr)
			return;
		const double r2d = 180.0 / 3.14159265358979;
		const double yaw = std::atan2(m_camFwd.y, m_camFwd.x) * r2d + dyaw;
		const double now = std::asin(std::clamp(m_camFwd.z, -1.0, 1.0)) * r2d;
		const double pitch = std::clamp(absolute ? dpitch : now + dpitch, -89.0, 89.0);
		ue::Call set(STR("/Script/Engine.Controller:SetControlRotation"));
		set.set(STR("NewRotation"), FRotatorD{pitch, yaw, 0.0}).run(m_pc);
		log("look: yaw %.1f pitch %.1f", yaw, pitch);
	}

	/// RoN's first-person arms (APlayerCharacter.Mesh1P, with what is attached to them) and the equipped item.
	/// Only on the living player: a dead one's equipped item may already be destroyed (calling it crashed RoN:
	/// 2026-10-06 15:29, build mode turned off after zombies had killed the player).
	void show_first_person(UObject *pawn, bool visible)
	{
		if (pawn == nullptr || pawn != m_lastPawn || !player_alive() || Ref(pawn).get() == nullptr)
			return;
		static Member<UObject *> mesh1P{STR("Mesh1P")};
		if (UObject **arms = mesh1P.in(pawn); arms && *arms)
		{
			ue::Call vis(STR("/Script/Engine.SceneComponent:SetVisibility"));
			vis.set(STR("bNewVisibility"), visible).set(STR("bPropagateToChildren"), true).run(*arms);
		}
		ue::Call item(STR("/Script/ReadyOrNot.ReadyOrNotCharacter:GetEquippedItem"));
		item.run(pawn);
		if (UObject *equipped = Ref(item.get<UObject *>(STR("ReturnValue"))).get())
		{
			ue::Call hide(STR("/Script/Engine.Actor:SetActorHiddenInGame"));
			hide.set(STR("bNewHidden"), !visible).run(equipped);
		}
	}

	/// Twice a second: each living suspect with a clear line to a mob within 15 m turns to the nearest one and shoots it.
	void suspects_fight_back()
	{
		crash_guard::Scope scope("tick_fight_back");
		if (m_mobs.empty())
			return;
		for (auto &[handle, ref] : m_pedByHandle)
		{
			UObject *ped = ref.get();
			if (ped == nullptr)
				continue;
			UObject **root = m_pedRoot.in(ped);
			FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr;
			if (at == nullptr)
				continue;
			const Mob *best = nullptr;
			double bestD = 1500.0 * 1500.0;
			for (const Mob &mob : m_mobs)
			{
				if (!mob.hostile)
					continue;
				const double dx = mob.at.x - at->x, dy = mob.at.y - at->y, dz = mob.at.z + 90.0 - at->z;
				const double d = dx * dx + dy * dy + dz * dz;
				if (d < bestD)
				{
					bestD = d;
					best = &mob;
				}
			}
			if (best == nullptr)
				continue;
			// eyes to the mob's chest: anything of RoN's in between (a wall) blocks the shot
			const FVectorD eye{at->x, at->y, at->z + 60.0}, target{best->at.x, best->at.y, best->at.z + 100.0};
			FVectorD hit{};
			if (m_tracer.trace(ped, eye, target, hit, live_block_actor()))
				continue;
			// RoN's bullets spread: no shot that passes within 1.5 m of the player (it wounds them and blurs the screen)
			if (UObject **proot = m_lastPawn ? m_pedRoot.in(m_lastPawn) : nullptr; proot && *proot)
				if (FVectorD *p = m_pedLocation.in(*proot))
				{
					const double sx = target.x - eye.x, sy = target.y - eye.y, sz = target.z - eye.z;
					const double len2 = std::max(1.0, sx * sx + sy * sy + sz * sz);
					const double u = std::clamp(((p->x - eye.x) * sx + (p->y - eye.y) * sy + (p->z - eye.z) * sz) / len2, 0.0, 1.2);
					const double qx = eye.x + sx * u - p->x, qy = eye.y + sy * u - p->y, qz = eye.z + sz * u - p->z;
					if (qx * qx + qy * qy + qz * qz < 150.0 * 150.0)
						continue;
				}
			if (UObject **controller = m_pedController.in(ped); controller && *controller)
			{
				ue::Call focus(STR("/Script/AIModule.AIController:K2_SetFocalPoint"));
				focus.set(STR("FP"), ue::Vec{target.x, target.y, target.z}).run(*controller);
			}
			// turns its whole body to the mob (the gun points where the body faces; a frozen squad can't turn by itself)
			{
				const double yawDeg = std::atan2(target.y - at->y, target.x - at->x) * 180.0 / 3.14159265358979;
				ue::Call turn(STR("/Script/Engine.Actor:K2_SetActorRotation"));
				turn.set(STR("NewRotation"), FRotatorD{0.0, yawDeg, 0.0}).set(STR("bTeleportPhysics"), true).run(ped);
			}
			// and pulls the trigger: a real RoN shot (sound, muzzle flash) towards where it now looks
			ue::Call fire(STR("/Script/ReadyOrNot.CyberneticCharacter:ForceFireGun"));
			fire.set(STR("Chance"), 1.0f).run(ped);
			send("{\"t\":\"mobdmg\",\"id\":%d,\"d\":%.1f}", best->id, 8.0); // two such shots kill a zombie (Minecraft skips an equal hit inside its 10-tick hurt cooldown)
			log("%s %d shoots mob %d (%.0f m)", ped->GetClassPrivate()->GetName().find(STR("Swat")) != StringType::npos ? "officer" : "suspect", handle,
				best->id, std::sqrt(bestD) / 100.0);
		}
	}

	/// A Minecraft mob hit one of RoN's people: {"t":"mobhit","h":handle,"d":damage,"from":[...],"k":"zombie"}.
	void on_mobhit(const std::string &m)
	{
		const char *h = std::strstr(m.c_str(), "\"h\":");
		const char *d = std::strstr(m.c_str(), "\"d\":");
		if (h == nullptr || d == nullptr)
			return;
		static UObject *bullet = ue::find(STR("/Script/ReadyOrNot.BulletDamageType"));
		if (std::atoi(h + 4) == 0)
		{
			// the player's own proxy: a zombie's hit is a few points of RoN's 100 (armour takes its share)
			if (!m_mobsHuntPlayer || !player_alive() || m_god)
				return;
			const float damage = float(std::atof(d + 4) * 4.0);
			ue::Call call(STR("/Script/Engine.GameplayStatics:ApplyDamage"));
			call.set(STR("DamagedActor"), m_lastPawn).set(STR("BaseDamage"), damage).set(STR("DamageTypeClass"), bullet)
				.set(STR("EventInstigator"), m_pc).set(STR("DamageCauser"), m_lastPawn).run();
			UObject **health = m_pedHealth.in(m_lastPawn);
			float *resource = health && *health ? m_pedResource.in(*health) : nullptr;
			log("mob hit the player: %.0f damage, health now %.0f", damage, resource ? *resource : -1.0f);
			return;
		}
		auto it = m_pedByHandle.find(std::atoi(h + 4));
		UObject *victim = it != m_pedByHandle.end() ? it->second.get() : nullptr;
		if (victim == nullptr)
			return;
		const float damage = float(std::atof(d + 4) * 8.0); // Minecraft hearts are tiny next to RoN's 100-200 health
		if (!player_alive())
			return;
		ue::Call call(STR("/Script/Engine.GameplayStatics:ApplyDamage"));
		call.set(STR("DamagedActor"), victim).set(STR("BaseDamage"), damage).set(STR("DamageTypeClass"), bullet)
			.set(STR("EventInstigator"), m_pc).set(STR("DamageCauser"), m_lastPawn).run();
		UObject **health = m_pedHealth.in(victim);
		float *resource = health && *health ? m_pedResource.in(*health) : nullptr;
		log("mob hit ped %d: %.0f damage, health now %.0f", it->first, damage, resource ? *resource : -1.0f);
	}

	/// Radial damage right on character n, then its health again. ({"op":"hurt","v":n})
	void hurt_character(int n)
	{
		const std::vector<UObject *> list = characters();
		if (n >= int(list.size()))
			return;
		UObject *c = list[n];
		UObject **root = m_rootComponent.in(c);
		FVectorD *at = root && *root ? m_relativeLocation.in(*root) : nullptr;
		if (at == nullptr)
			return;
		const std::string before = health_of(c);
		const bool damaged = radial_damage(ue::Vec{at->x, at->y, at->z}, 50.0f, 80.0f);
		log("hurt [%d] %ls: %s | before: %s| after: %s", n, c->GetClassPrivate()->GetName().c_str(), damaged ? "damaged" : "not damaged",
			before.c_str(), health_of(c).c_str());
	}

	/// Damage is only dealt while the player's own character is alive and in the game: after death the pawn changes
	/// (or is being torn down) and RoN's damage code crashes on a null instigator.
	bool player_alive()
	{
		if (m_lastPawn == nullptr || m_pc == nullptr || !g_inGame)
			return false;
		if (m_lastPawn->GetClassPrivate()->GetName().find(STR("Player")) == StringType::npos)
			return false;
		UObject **health = m_pedHealth.in(m_lastPawn);
		float *resource = health && *health ? m_pedResource.in(*health) : nullptr;
		return resource == nullptr || *resource > 0.0f;
	}

	/// Radial damage in RoN (UE coordinates), sparing the player's pawn. True if it damaged anything.
	/// RoN blocks plain UDamageType on its characters: bullets (arrows, the sword) and traps (explosions) it takes.
	enum class Damage
	{
		bullet,
		trap
	};

	bool radial_damage(const ue::Vec &origin, float damage, float radiusCm, Damage type = Damage::bullet)
	{
		crash_guard::Scope scope("damage_radial");
		static UObject *bullet = ue::find(STR("/Script/ReadyOrNot.BulletDamageType"));
		static UObject *trap = ue::find(STR("/Script/ReadyOrNot.TrapDamage"));
		if (!player_alive())
			return false;
		UObject *ignore[1] = {m_lastPawn};
		const ue::PtrArray ignoreActors{ignore, 1, 1};
		ue::Call call(STR("/Script/Engine.GameplayStatics:ApplyRadialDamage"));
		call.set(STR("WorldContextObject"), m_lastPawn)
			.set(STR("BaseDamage"), damage)
			.set(STR("Origin"), origin)
			.set(STR("DamageRadius"), radiusCm)
			.set(STR("IgnoreActors"), ignoreActors)
			.set(STR("DamageTypeClass"), type == Damage::trap ? trap : bullet)
			.set(STR("DamageCauser"), m_lastPawn)
			.set(STR("InstigatedByController"), m_pc)
			.set(STR("bDoFullDamage"), false)
			.run();
		return call.get<bool>(STR("ReturnValue"));
	}

	ue::Vec to_ue(double x, double y, double z) const
	{
		return ue::Vec{uex(x), uey(z), uez(y)};
	}

	/// A Minecraft projectile moved from `from` to `to` (UE) this tick: if it crossed RoN's world, act it out there and
	/// tell Minecraft where (the firework bursts / the arrow stops there too). True when it hit.
	bool projectile_segment(int id, const Projectile &from, const FVectorD &to)
	{
		crash_guard::Scope scope("projectile_segment");
		const double dx = to.x - from.at.x, dy = to.y - from.at.y, dz = to.z - from.at.z;
		const double len = std::max(0.001, std::sqrt(dx * dx + dy * dy + dz * dz));
		FVectorD hit{};
		// the world trace stops on a person's capsule, which is wider than the body: an arrow that grazes it but
		// misses the body itself (past a shoulder, between the legs) flies on, that person ignored
		UObject *passed[3] = {};
		int passedCount = 0;
		for (;;)
		{
			if (!m_tracer.trace(m_lastPawn, from.at, to, hit, live_block_actor(), 0.0f, passed, passedCount))
				return false;
			if (from.firework || passedCount >= 3)
				break;
			UObject *victim = m_tracer.last_actor();
			if (!is_person(victim) || body_hit(victim, hit, dx / len, dy / len, dz / len))
				break;
			passed[passedCount++] = victim;
			log("arrow passes %ls (missed the body)", victim->GetClassPrivate()->GetName().c_str());
		}
		bool stick = false;
		if (from.firework)
		{
			const bool damaged = radial_damage(ue::Vec{hit.x, hit.y, hit.z}, 80.0f, 200.0f, Damage::trap);
			log("firework burst at UE (%.0f, %.0f, %.0f)%s", hit.x, hit.y, hit.z, damaged ? ": damaged something" : "");
		}
		else
		{
			// an arrow: point damage on whatever it hit (RoN applies its own hit-zone multipliers: a headshot kills),
			// or it sticks in the wall
			UObject *victim = m_tracer.last_actor();
			const bool person = is_person(victim);
			if (person && player_alive())
			{
				static UObject *bullet = ue::find(STR("/Script/ReadyOrNot.BulletDamageType"));
				static Member<UObject *> meshMember{STR("Mesh")};
				int32_t size = 0;
				const uint8_t *hitInfo = m_tracer.last_hit(size);
				// the world trace stops on the capsule; RoN scores bullets by the bone hit, so trace the body itself
				ue::Call bodyTrace(STR("/Script/Engine.PrimitiveComponent:K2_LineTraceComponent"));
				UObject **mesh = meshMember.in(victim);
				const double ext = 150.0;
				if (mesh && *mesh)
				{
					bodyTrace.set(STR("TraceStart"), ue::Vec{hit.x - dx / len * ext, hit.y - dy / len * ext, hit.z - dz / len * ext})
						.set(STR("TraceEnd"), ue::Vec{hit.x + dx / len * ext, hit.y + dy / len * ext, hit.z + dz / len * ext})
						.set(STR("bTraceComplex"), true)
						.run(*mesh);
					if (bodyTrace.get<bool>(STR("ReturnValue")))
						hitInfo = bodyTrace.bytes(STR("OutHit"), size);
					RC::Unreal::FName bone = bodyTrace.get<RC::Unreal::FName>(STR("BoneName"));
					log("arrow: body %s, bone %ls", bodyTrace.get<bool>(STR("ReturnValue")) ? "hit" : "missed", bone.ToString().c_str());
				}
				ue::Call point(STR("/Script/Engine.GameplayStatics:ApplyPointDamage"));
				point.set(STR("DamagedActor"), victim)
					.set(STR("BaseDamage"), 90.0f)
					.set(STR("HitFromDirection"), ue::Vec{dx / len, dy / len, dz / len})
					.raw(STR("HitInfo"), hitInfo, size_t(size))
					.set(STR("EventInstigator"), m_pc)
					.set(STR("DamageCauser"), m_lastPawn)
					.set(STR("DamageTypeClass"), bullet)
					.run();
			}
			stick = !person;
			log("arrow hit at UE (%.0f, %.0f, %.0f): %ls", hit.x, hit.y, hit.z,
				person ? victim->GetClassPrivate()->GetName().c_str() : STR("stuck"));
		}
		// back 15 cm along its path, in Minecraft coordinates
		const double bx = hit.x - dx / len * 15.0, by = hit.y - dy / len * 15.0, bz = hit.z - dz / len * 15.0;
		send("{\"t\":\"projhit\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"stick\":%s}", id, mcx(bx), mcy(bz), mcz(by),
			stick ? "true" : "false");
		return true;
	}

	static bool is_person(UObject *actor)
	{
		return actor != nullptr && actor->GetClassPrivate()->GetName().find(STR("Cybernetics")) != StringType::npos;
	}

	/// Whether a line through `hit` along (ux, uy, uz) touches the person's body mesh (not just its capsule).
	bool body_hit(UObject *person, const FVectorD &hit, double ux, double uy, double uz)
	{
		static Member<UObject *> meshMember{STR("Mesh")};
		UObject **mesh = meshMember.in(person);
		if (mesh == nullptr || *mesh == nullptr)
			return true; // no mesh to check: the capsule counts
		const double ext = 150.0;
		ue::Call body(STR("/Script/Engine.PrimitiveComponent:K2_LineTraceComponent"));
		body.set(STR("TraceStart"), ue::Vec{hit.x - ux * ext, hit.y - uy * ext, hit.z - uz * ext})
			.set(STR("TraceEnd"), ue::Vec{hit.x + ux * ext, hit.y + uy * ext, hit.z + uz * ext})
			.set(STR("bTraceComplex"), true)
			.run(*mesh);
		return body.get<bool>(STR("ReturnValue"));
	}

	/// {"t":"proj","p":[[id,"arrow"|"firework",x,y,z],...]} (Minecraft coordinates), every tick while any fly.
	void on_projectiles(const std::string &m)
	{
		if (m_lastPawn == nullptr || !m_haveOffset)
			return;
		for (auto &[id, pr] : m_projectiles)
			pr.seen = false;
		const char *at = std::strstr(m.c_str(), "\"p\":[");
		while (at && (at = std::strchr(at + 1, '[')) != nullptr)
		{
			int id = 0;
			char kind[16] = {};
			double x, y, z;
			if (sscanf_s(at, "[%d,\"%15[^\"]\",%lf,%lf,%lf]", &id, kind, unsigned(sizeof(kind)), &x, &y, &z) != 5)
				continue;
			const ue::Vec u = to_ue(x, y, z);
			const FVectorD now{u.x, u.y, u.z};
			auto it = m_projectiles.find(id);
			// first sighting: trace from the camera, so point-blank shots count
			const Projectile from = it != m_projectiles.end() ? it->second : Projectile{m_camLoc, std::strcmp(kind, "firework") == 0, true};
			const bool hit = projectile_segment(id, from, now);
			m_projectiles[id] = Projectile{now, from.firework, !hit};
		}
		for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
			it = it->second.seen ? std::next(it) : m_projectiles.erase(it);
	}

	/// A sword swing: what is in front of the camera (up to ~2.5 m) takes a hit.
	void on_melee()
	{
		const ue::Vec c{m_camLoc.x + m_camFwd.x * 150.0, m_camLoc.y + m_camFwd.y * 150.0, m_camLoc.z + m_camFwd.z * 150.0};
		const bool damaged = radial_damage(c, 55.0f, 110.0f);
		log("sword swing: %s", damaged ? "hit something" : "hit nothing");
	}

	/// A Minecraft explosion: radial damage in RoN around the same point (the player's own pawn is spared).
	void on_explosion(const std::string &m)
	{
		const std::vector<double> pos = json_doubles(m, "pos");
		const char *r = std::strstr(m.c_str(), "\"r\":");
		if (pos.size() < 3 || r == nullptr || m_lastPawn == nullptr || !m_haveOffset)
			return;
		const double radius = std::atof(r + 4);
		const ue::Vec origin = to_ue(pos[0], pos[1], pos[2]);
		const bool damaged = radial_damage(origin, float(60.0 * radius), float(radius * 150.0), Damage::trap);
		log("explosion r=%.1f at UE (%.0f, %.0f, %.0f): radial damage %s", radius, origin.x, origin.y, origin.z,
			damaged ? "hit something" : "hit nothing");
	}

	/// Whether a ray started at UE z `at` in column (cx, cy) starts inside something solid (a convex collision hull:
	/// a vehicle, a planter, a bush, a slab). Volumes in `volumes` don't count.
	bool inside_solid(UObject *pawn, double cx, double cy, double at, UObject *ignore, UObject *const *volumes, int volumeCount)
	{
		FVectorD hit{};
		return m_ground.trace(pawn, FVectorD{cx, cy, at}, FVectorD{cx, cy, at - 5.0}, hit, ignore, 0.0f, volumes, volumeCount) && hit.z > at - 1.0;
	}

	/// From `at` (inside a solid), the first point below where the solid ends: steps down 40, 80, 160... cm until
	/// outside, then bisects to ~3 cm. NAN if it doesn't end within kMaxSolid (or above zBottom).
	double solid_exit_below(UObject *pawn, double cx, double cy, double at, double zBottom, UObject *ignore, UObject *const *volumes, int volumeCount)
	{
		constexpr double kMaxSolid = 800.0;
		double in = at, out = NAN;
		for (double step = 40.0; at - in < kMaxSolid; step *= 2.0)
		{
			const double probe = std::max(in - step, zBottom);
			if (!inside_solid(pawn, cx, cy, probe, ignore, volumes, volumeCount))
			{
				out = probe;
				break;
			}
			in = probe;
			if (probe <= zBottom)
				return NAN;
		}
		if (std::isnan(out))
			return NAN;
		while (in - out > 3.0)
		{
			const double mid = (in + out) * 0.5;
			(inside_solid(pawn, cx, cy, mid, ignore, volumes, volumeCount) ? in : out) = mid;
		}
		return out;
	}

	/// One Minecraft column (x, z), every storey: a ray from zTop down finds the highest surface of the level (not of
	/// its characters); the next ray starts just below it and finds the one under that, and so on (up to kMaxFloors) -
	/// roofs, upper floors, landings, the ground. Each surface becomes a barrier. Where there is no room for a mob to
	/// stand on it (a 25 cm sphere swept up the column's centre from 0.7 to 1.7 m hits something: a wall, a table, a
	/// low ceiling) the barrier is 3 blocks high, so Minecraft's mobs walk every floor, take the stairs, go through
	/// doorways and don't walk through walls.
	/// Under a surface the ray may start inside the thing it belongs to (a vehicle, a planter, a bush, a thick slab):
	/// the thing goes to Minecraft as a solid pillar down to where it ends, and the probe goes on below it (before,
	/// the ground under it was never found: a hole into Minecraft's void under every parked car). An invisible
	/// blocking volume the ray starts in is skipped (a level boundary, a no-climb box: no floors inside).
	/// Appends "x,z,bottom,top" entries to columns.
	void probe_column(UObject *pawn, int x, int z, double zTop, double zBottom, std::string &columns)
	{
		UObject *ignore = live_block_actor();
		const double cx = uex(x + 0.5), cy = uey(z + 0.5);
		const int64_t key = column_key(x, z);
		double from = zTop;
		double surfaceAbove = NAN; // the last surface found: a solid right under it is that thing's body
		int surfaceTop = 0;
		UObject *volumes[3] = {};
		int volumeCount = 0;
		auto add = [&](int bottom, int top) {
			char entry[64];
			std::snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, bottom, top);
			columns += entry;
		};
		for (int found = 0, tries = 0; found < kMaxFloors && tries < 16 && from > zBottom; ++tries)
		{
			FVectorD hit{};
			const bool any = m_ground.trace(pawn, FVectorD{cx, cy, from}, FVectorD{cx, cy, zBottom}, hit, ignore, 0.0f, volumes, volumeCount);
			if (m_probeDebug)
				log("  try %d from z %.0f: %s z %.0f %ls", tries, from, any ? "hit" : "nothing", any ? hit.z : 0.0,
					any ? m_ground.last_component().c_str() : STR(""));
			if (!any)
				break;
			if (hit.z > from - 1.0)
			{
				if (volumeCount < 3 && m_ground.last_hit_volume())
				{
					volumes[volumeCount++] = m_ground.last_actor();
					continue;
				}
				// inside a solid: find its bottom and go on below it
				const double exit = solid_exit_below(pawn, cx, cy, from, zBottom, ignore, volumes, volumeCount);
				if (m_probeDebug)
					log("  inside a solid from z %.0f: it ends at z %.0f", from, exit);
				if (std::isnan(exit))
				{
					if (!std::isnan(surfaceAbove))
						break; // under a surface and solid for 8 m: the ground's mass, nothing below it matters
					from -= 800.0; // started inside a building's mass at the top: look further down
					continue;
				}
				if (!std::isnan(surfaceAbove))
				{
					// the thing under the last surface, as solid barriers from its bottom up to that surface's block
					const int bottom = int(std::floor(mcy(exit) + 0.5));
					if (bottom < surfaceTop && surfaceTop - bottom <= 12)
					{
						add(bottom, surfaceTop - 1);
						m_pillars[key].emplace_back(bottom, surfaceTop - 1);
					}
				}
				surfaceAbove = NAN;
				from = exit - 1.0;
				continue;
			}
			++found;
			const int top = int(std::floor(mcy(hit.z) + 0.5)) - 1;
			FVectorD wallHit{};
			bool wall = m_room.trace(pawn, FVectorD{cx, cy, hit.z + 70.0}, FVectorD{cx, cy, hit.z + 170.0}, wallHit, ignore, 25.0f);
			// the edges to the +x and +z neighbours: a wall between two column centres that the room test (25 cm
			// around each centre) misses would let mobs walk through it. Blocked at 1.0 and 1.6 m both: a wall, not a
			// stair riser, a table or a railing. The cell the wall stands in gets it.
			for (const auto [ex, ez] : {std::pair<int, int>{1, 0}, std::pair<int, int>{0, 1}})
			{
				const double nx = uex(x + ex + 0.5), ny = uey(z + ez + 0.5);
				FVectorD low{}, high{};
				if (!m_edge.trace(pawn, FVectorD{cx, cy, hit.z + 100.0}, FVectorD{nx, ny, hit.z + 100.0}, low, ignore) ||
					!m_edge.trace(pawn, FVectorD{cx, cy, hit.z + 160.0}, FVectorD{nx, ny, hit.z + 160.0}, high, ignore))
					continue;
				const int hx = int(std::floor(mcx(low.x))), hz = int(std::floor(mcz(low.y)));
				if (hx == x && hz == z)
					wall = true;
				else if ((hx == x + ex && hz == z + ez) && !walked_near(hx, hz, top))
				{
					std::vector<int> &tops = m_wallTops[column_key(hx, hz)];
					if (std::find(tops.begin(), tops.end(), top) == tops.end())
					{
						tops.push_back(top);
						// the wall's blocks only: the neighbour's own probe sends its floor
						char entry[64];
						std::snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", hx, hz, top + 1, top + 3);
						columns += entry;
						++m_edgeWalls;
					}
				}
			}
			if (wall && walked_near(x, z, top))
				wall = false; // someone walked here: passable whatever the coarse test says
			m_floorTops[key].push_back(top);
			if (wall)
				m_wallTops[key].push_back(top);
			add(top, wall ? top + 3 : top);
			++m_groundSent;
			if (wall)
				++m_wallColumns;
			surfaceAbove = hit.z;
			surfaceTop = top;
			from = hit.z - 2.0; // just below this surface: in the thing it belongs to, or over the storey under it
		}
	}

	double probe_ms() const
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return m_probeFrames > 0 ? double(m_probeTicks) * 1000.0 / double(f.QuadPart) / m_probeFrames : 0.0;
	}

	static int64_t column_key(int x, int z)
	{
		return (int64_t(x) << 32) ^ uint32_t(z);
	}

	bool walked_near(int x, int z, int top) const
	{
		for (int t = top - 1; t <= top + 1; ++t)
			if (m_walked.count(block_key(x, t, z)))
				return true;
		return false;
	}

	/// RoN's player or an AI character stands with its feet at (UE) `at`: its Minecraft column is passable on that
	/// storey. A wall sent there is a false one (the 1 m test caught a doorway frame or furniture edge near the column's
	/// centre) and is opened again; the column is remembered, so a later re-probe doesn't put the wall back.
	/// The character stands on something (CharacterMovement: Walking or NavWalking), not jumping or falling: only then
	/// do its feet tell where a floor is.
	bool on_ground(UObject *character)
	{
		UObject **movement = m_movement.in(character);
		uint8_t *mode = movement && *movement ? m_movementMode.in(*movement) : nullptr;
		return mode == nullptr || *mode == 1 || *mode == 2;
	}

	void walked(const FVectorD &feet, std::string &opens)
	{
		const int x = int(std::floor(mcx(feet.x))), z = int(std::floor(mcz(feet.y)));
		const int top = int(std::floor(mcy(feet.z) + 0.5)) - 1;
		if (!m_walked.insert(block_key(x, top, z)).second)
			return;
		auto it = m_wallTops.find(column_key(x, z));
		if (it == m_wallTops.end())
			return;
		std::vector<int> &tops = it->second;
		const std::vector<int> &floors = m_floorTops[column_key(x, z)];
		for (auto t = tops.begin(); t != tops.end();)
		{
			if (std::abs(*t - top) > 1)
			{
				++t;
				continue;
			}
			// the wall's blocks above this floor, but never another storey's floor among them (a low ceiling's floor)
			for (int y = *t + 1; y <= *t + 3; ++y)
			{
				if (std::find(floors.begin(), floors.end(), y) != floors.end())
					continue;
				char entry[64];
				std::snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", opens.empty() ? "" : ",", x, z, y, y);
				opens += entry;
			}
			++m_wallsOpened;
			m_openedTops[column_key(x, z)].push_back(*t);
			t = tops.erase(t);
		}
	}

	void send_opens(const std::string &opens)
	{
		if (!opens.empty())
			m_ws.send("{\"t\":\"open\",\"c\":[" + opens + "]}");
	}

	/// The player's health back to full and (v=1) unlimited, for staging shots next to the squad's gunfire.
	bool m_god = false; // god mode (op god): Minecraft's mobs don't hurt the player either

	void god_mode(bool on)
	{
		m_god = on;
		UObject **health = m_lastPawn ? m_pedHealth.in(m_lastPawn) : nullptr;
		if (health == nullptr || *health == nullptr)
			return;
		ue::Call full(STR("/Script/ReadyOrNot.ResourceComponent:SetCurrentResourceToMax"));
		full.run(*health);
		ue::Call unlimited(STR("/Script/ReadyOrNot.ResourceComponent:SetUnlimitedResource"));
		unlimited.set(STR("bEnabled"), on).run(*health);
		float *resource = m_pedResource.in(*health);
		log("player health %.0f, unlimited %s", resource ? *resource : -1.0f, on ? "on" : "off");
	}

	/// The SWAT team stops moving (CharacterMovement.DisableMovement) or walks again (SetMovementMode Walking):
	/// for staging shots, the squad stays where it stands while the player walks away. ({"op":"freeze","v":1|0})
	void freeze_squad(bool freeze)
	{
		static Member<UObject *> movement{STR("CharacterMovement")};
		int n = 0;
		for (UObject *c : characters())
		{
			if (c == m_lastPawn || c->GetClassPrivate()->GetName().find(STR("Swat_V2")) == StringType::npos)
				continue;
			UObject **mc = movement.in(c);
			if (mc == nullptr || *mc == nullptr)
				continue;
			if (freeze)
			{
				ue::Call stop(STR("/Script/Engine.CharacterMovementComponent:DisableMovement"));
				stop.run(*mc);
			}
			else
			{
				ue::Call walk(STR("/Script/Engine.CharacterMovementComponent:SetMovementMode"));
				walk.set(STR("NewMovementMode"), uint8_t(1)).set(STR("NewCustomMode"), uint8_t(0)).run(*mc);
			}
			++n;
		}
		log("squad %s (%d)", freeze ? "frozen in place" : "moving again", n);
	}

	/// The first floor below `from` at (cx, cy), like the column probes see it: invisible blocking volumes the ray
	/// starts inside are skipped (their inside is no floor). For the debug views (map, cols).
	bool floor_below(UObject *pawn, double cx, double cy, double from, double to, FVectorD &hit)
	{
		UObject *ignore = live_block_actor();
		UObject *volumes[3] = {};
		for (int n = 0; n <= 3; ++n)
		{
			if (!m_ground.trace(pawn, FVectorD{cx, cy, from}, FVectorD{cx, cy, to}, hit, ignore, 0.0f, volumes, n))
				return false;
			if (hit.z <= from - 1.0 || n == 3 || !m_ground.last_hit_volume())
				return true;
			volumes[n] = m_ground.last_actor();
		}
		return true;
	}

	/// An ASCII map of what Minecraft got around the player (1 m cells, UE axes): rows go along +x (top row = largest
	/// x), columns along +y. For the storey nearest the player's feet: '.' floor, '#' a wall (or a solid thing) over
	/// it, '+' a wall opened because someone walked there, ' ' no floor (a hole), '?' not probed yet; 'P' player,
	/// 'S' a SWAT teammate, 'X' another character. ({"op":"map","v":radius})
	void log_map(UObject *pawn, const FVectorD &cam, double feetZ, int r)
	{
		const int px = int(std::floor(cam.x / 100.0)), py = int(std::floor(cam.y / 100.0));
		const int tf = int(std::floor(mcy(feetZ) + 0.5)) - 1;
		std::vector<std::string> rows(2 * r + 1, std::string(2 * r + 1, '?'));
		auto has = [](const std::unordered_map<int64_t, std::vector<int>> &m, int64_t k, int v) {
			auto it = m.find(k);
			return it != m.end() && std::find(it->second.begin(), it->second.end(), v) != it->second.end();
		};
		for (int ix = -r; ix <= r; ++ix)
			for (int iy = -r; iy <= r; ++iy)
			{
				const int mx = int(std::floor(mcx((px + ix + 0.5) * 100.0))), mz = int(std::floor(mcz((py + iy + 0.5) * 100.0)));
				const int64_t k = column_key(mx, mz);
				char c = '?';
				if (m_sampled.count(k))
				{
					c = ' ';
					int best = INT_MIN;
					if (auto it = m_floorTops.find(k); it != m_floorTops.end())
						for (int f : it->second)
							if (std::abs(f - tf) <= 2 && (best == INT_MIN || std::abs(f - tf) < std::abs(best - tf)))
								best = f;
					bool solid = false;
					if (auto it = m_pillars.find(k); it != m_pillars.end())
						for (auto [b, t] : it->second)
							solid = solid || (b <= (best == INT_MIN ? tf + 1 : best + 1) && (best == INT_MIN ? tf + 1 : best + 1) <= t);
					if (best != INT_MIN)
						c = has(m_wallTops, k, best) || solid ? '#' : (has(m_openedTops, k, best) ? '+' : '.');
					else if (solid)
						c = '#';
				}
				rows[r - ix][iy + r] = c;
			}
		for (UObject *c : characters())
		{
			UObject **root = m_pedRoot.in(c);
			FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr;
			if (at == nullptr)
				continue;
			const int ix = int(std::floor(at->x / 100.0)) - px, iy = int(std::floor(at->y / 100.0)) - py;
			if (std::abs(ix) > r || std::abs(iy) > r)
				continue;
			const bool swat = c->GetClassPrivate()->GetName().find(STR("Swat")) != StringType::npos;
			rows[r - ix][iy + r] = c == pawn ? 'P' : (swat ? 'S' : 'X');
		}
		log("map around UE (%d, %d) m, radius %d (top row x=%d, left column y=%d):", px, py, r, px + r, py - r);
		for (const std::string &row : rows)
			log("  |%s|", row.c_str());
	}

	/// What Minecraft got for each column around the player (floors and walls sent, walked storeys) next to what the
	/// probes find there now. ({"op":"cols","v":radius})
	void log_columns(UObject *pawn, const FVectorD &cam, double feetZ, int r)
	{
		UObject *ignore = live_block_actor();
		const int px = int(std::floor(mcx(cam.x))), pz = int(std::floor(mcz(cam.y)));
		const int feetTop = int(std::floor(mcy(feetZ) + 0.5)) - 1;
		log("columns around MC (%d, %d), player's floor block y %d, sampled %d:", px, pz, feetTop, int(m_sampled.size()));
		for (int dx = -r; dx <= r; ++dx)
			for (int dz = -r; dz <= r; ++dz)
			{
				const int x = px + dx, z = pz + dz;
				auto list = [](const std::unordered_map<int64_t, std::vector<int>> &m, int64_t k) {
					std::string out;
					if (auto it = m.find(k); it != m.end())
						for (int t : it->second)
							out += std::to_string(t) + " ";
					return out.empty() ? std::string("-") : out;
				};
				const int64_t k = column_key(x, z);
				const double cx = uex(x + 0.5), cy = uey(z + 0.5);
				FVectorD hit{}, w{};
				std::string now = "no floor";
				if (floor_below(pawn, cx, cy, feetZ + 150.0, feetZ - 400.0, hit))
				{
					const bool wall = m_room.trace(pawn, FVectorD{cx, cy, hit.z + 70.0}, FVectorD{cx, cy, hit.z + 170.0}, w, ignore, 25.0f);
					now = "floor y " + std::to_string(int(std::floor(mcy(hit.z) + 0.5)) - 1) + (wall ? " +room hit" : "");
				}
				std::string pillars;
				if (auto it = m_pillars.find(k); it != m_pillars.end())
					for (auto [b, t] : it->second)
						pillars += std::to_string(b) + ".." + std::to_string(t) + " ";
				log("  (%d,%d) sampled %d floors [%s] walls [%s] solid [%s] | now %s", x, z, int(m_sampled.count((int64_t(x) << 32) ^ uint32_t(z))),
					list(m_floorTops, k).c_str(), list(m_wallTops, k).c_str(), pillars.empty() ? "-" : pillars.c_str(), now.c_str());
			}
	}

	/// Mobs next to RoN's first suspect: the floor around them is probed first (their own storey), then Minecraft
	/// spawns n zombies 2-5 blocks from them. ({"op":"mobsat","v":n})
	void mobs_at_suspect(int n)
	{
		if (m_pedByHandle.empty() || m_lastPawn == nullptr)
		{
			log("mobsat: no suspects");
			return;
		}
		UObject *ped = m_pedByHandle.begin()->second.get();
		if (ped == nullptr)
			return;
		const int handle = m_pedByHandle.begin()->first;
		UObject **root = m_pedRoot.in(ped);
		FVectorD *at = root && *root ? m_pedLocation.in(*root) : nullptr;
		if (at == nullptr)
			return;
		const double feetZ = at->z - 90.0;
		const int cx = int(std::floor(mcx(at->x))), cz = int(std::floor(mcz(at->y)));
		std::string columns;
		for (int dx = -6; dx <= 6; ++dx)
			for (int dz = -6; dz <= 6; ++dz)
				if (dx * dx + dz * dz <= 36 && m_sampled.insert((int64_t(cx + dx) << 32) ^ uint32_t(cz + dz)).second)
					probe_column(m_lastPawn, cx + dx, cz + dz, m_levelFeetZ + kFloorsAbove, m_levelFeetZ - kFloorsBelow, columns);
		if (!columns.empty())
			m_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
		// spots the zombies can walk from to the suspect, 2+ blocks away (behind a wall they'd never reach them)
		const std::vector<std::array<int, 3>> spots = reachable_spots(cx, cz, int(std::floor(mcy(feetZ) + 0.5)) - 1, 6);
		std::string list;
		for (size_t i = 0; i < spots.size() && i < 24; ++i)
			list += (list.empty() ? "" : ",") + std::string("[") + std::to_string(spots[i][0]) + "," + std::to_string(spots[i][1]) + "," +
				std::to_string(spots[i][2]) + "]";
		if (!list.empty())
			m_ws.send("{\"t\":\"spawnmobs\",\"k\":\"zombie\",\"n\":" + std::to_string(n) + ",\"spots\":[" + list + "]}");
		else
			send("{\"t\":\"spawnmobs\",\"k\":\"zombie\",\"n\":%d,\"rmin\":2,\"rmax\":5,\"at\":[%.2f,%.2f,%.2f]}", n, mcx(at->x),
				mcy(feetZ), mcz(at->y));
		log("mobsat: %d zombies at suspect %d, UE (%.0f, %.0f, %.0f), %d reachable spots", n, handle, at->x, at->y, at->z, int(spots.size()));
		log_map(m_lastPawn, FVectorD{at->x, at->y, at->z}, feetZ, 6); // what Minecraft has around them, on their storey
	}

	/// Cells reachable on foot from (sx, sz) on the storey whose floor block is near `tf`, by what Minecraft was sent:
	/// a floor within a step (+-1) of the cell before, no wall on it, nothing solid over it. Within `radius`, two or
	/// more blocks from the start, shuffled; each as [x, feet y, z].
	std::vector<std::array<int, 3>> reachable_spots(int sx, int sz, int tf, int radius)
	{
		auto floor_near = [&](int x, int z, int want) {
			int best = INT_MIN;
			if (auto it = m_floorTops.find(column_key(x, z)); it != m_floorTops.end())
				for (int f : it->second)
					if (std::abs(f - want) <= 1 && (best == INT_MIN || std::abs(f - want) < std::abs(best - want)))
						best = f;
			return best;
		};
		auto blocked = [&](int x, int z, int f) {
			// a wall on floor t fills t+1..t+3: in the way of feet and head (f+1, f+2) for t from f-2 to f+1 (an edge
			// wall carries its neighbour's floor height, a step off this column's)
			if (auto it = m_wallTops.find(column_key(x, z)); it != m_wallTops.end())
				for (int t : it->second)
					if (t >= f - 2 && t <= f + 1)
						return true;
			if (auto it = m_pillars.find(column_key(x, z)); it != m_pillars.end())
				for (auto [b, t] : it->second)
					if (b <= f + 2 && f + 1 <= t)
						return true;
			return false;
		};
		std::vector<std::array<int, 3>> out;
		std::unordered_map<int64_t, int> seen; // column -> its floor
		std::vector<std::array<int, 3>> queue{{sx, sz, tf}};
		seen[column_key(sx, sz)] = tf;
		for (size_t i = 0; i < queue.size() && queue.size() < 600; ++i)
		{
			const auto [x, z, f] = queue[i];
			for (const auto [dx, dz] : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
			{
				const int nx = x + dx, nz = z + dz;
				if ((nx - sx) * (nx - sx) + (nz - sz) * (nz - sz) > radius * radius || seen.count(column_key(nx, nz)))
					continue;
				const int nf = floor_near(nx, nz, f);
				if (nf == INT_MIN || blocked(nx, nz, nf))
					continue;
				seen[column_key(nx, nz)] = nf;
				queue.push_back({nx, nz, nf});
				if (std::abs(nx - sx) + std::abs(nz - sz) >= 2)
					out.push_back({nx, nf + 1, nz});
			}
		}
		for (size_t i = out.size(); i > 1; --i)
			std::swap(out[i - 1], out[size_t(std::rand()) % i]);
		return out;
	}

	static std::vector<std::pair<int, int>> make_spiral(int radius)
	{
		std::vector<std::pair<int, int>> spiral;
		for (int dx = -radius; dx <= radius; ++dx)
			for (int dz = -radius; dz <= radius; ++dz)
				if (dx * dx + dz * dz <= radius * radius)
					spiral.emplace_back(dx, dz);
		std::sort(spiral.begin(), spiral.end(), [](auto &a, auto &b) {
			return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
		});
		return spiral;
	}

	/// Probes up to `budget` columns not probed yet, nearest first, around (cx, cz). Returns how many it probed.
	int sample_around(UObject *pawn, int cx, int cz, const std::vector<std::pair<int, int>> &spiral, int budget, std::string &columns)
	{
		int probes = 0;
		for (const auto &[dx, dz] : spiral)
		{
			if (probes >= budget)
				break;
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if (m_probesThisTick > 0 && now.QuadPart > m_probeDeadline)
				break;
			const int x = cx + dx, z = cz + dz;
			if (!m_sampled.insert((int64_t(x) << 32) ^ uint32_t(z)).second)
				continue;
			++probes;
			++m_probesThisTick;
			probe_column(pawn, x, z, m_levelFeetZ + kFloorsAbove, m_levelFeetZ - kFloorsBelow, columns);
		}
		return probes;
	}

	/// Every tick: the floors around the player first, then around each suspect, up to kGroundProbesPerTick columns or
	/// kGroundProbeMsPerTick of the frame, whichever comes first.
	void sample_ground(UObject *pawn, double camX, double camZ)
	{
		crash_guard::Scope scope("tick_sample_ground");
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		QueryPerformanceFrequency(&freq);
		m_probeDeadline = t0.QuadPart + int64_t(double(freq.QuadPart) * kGroundProbeMsPerTick / 1000.0);
		m_probesThisTick = 0;
		static const std::vector<std::pair<int, int>> aroundPlayer = make_spiral(kGroundRadius);
		static const std::vector<std::pair<int, int>> aroundSuspect = make_spiral(kSuspectRadius);
		std::string columns;
		int budget = kGroundProbesPerTick;
		budget -= sample_around(pawn, int(std::floor(camX)), int(std::floor(camZ)), aroundPlayer, budget, columns);
		for (size_t i = 0; i < m_suspectCells.size() && budget > 0; ++i)
			budget -= sample_around(pawn, m_suspectCells[i].first, m_suspectCells[i].second, aroundSuspect, budget, columns);
		if (!columns.empty())
			m_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
		QueryPerformanceCounter(&t1);
		m_probeTicks += t1.QuadPart - t0.QuadPart;
		++m_probeFrames;
	}

	/// UGameplayStatics::IsGamePaused (game thread only).
	static bool game_paused(UObject *worldContext)
	{
		static auto *fn = UObjectGlobals::StaticFindObject<UFunction *>(nullptr, nullptr, STR("/Script/Engine.GameplayStatics:IsGamePaused"));
		static auto *cdo = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
		if (fn == nullptr || cdo == nullptr)
			return false;
		struct
		{
			UObject *worldContext;
			bool result;
		} params{worldContext, false};
		cdo->ProcessEvent(fn, &params);
		return params.result;
	}

	/// Whether the player is in the game proper: a pawn, not paused, no mouse cursor (menus, the pause menu and
	/// RoN's screens show one), and not the main menu's controller. Elsewhere Minecraft is not composited.
	bool in_game(UObject *pc, UObject *pawn)
	{
		UClass *c = pc->GetClassPrivate();
		if (c != m_pcClass)
		{
			m_pcClass = c;
			m_menuController = c->GetName().find(STR("Menu")) != StringType::npos;
			log("player controller class changed%s", m_menuController ? " (menu)" : "");
		}
		if (pawn == nullptr || m_menuController || m_showCursor.get(pc, false) || game_paused(pc))
			return false;
		// single player only: with other players in the session nothing of Minecraft is drawn, built or fought with
		// (they don't have it, and RoN's damage would be ours). RoN runs even single player as a listen server
		// (IsStandalone is false there: tried 2026-10-06), so it is "nobody else connected", not the net mode.
		const bool alone = !multiplayer();
		if (alone != m_standalone)
		{
			m_standalone = alone;
			log(alone ? "single player: BlockBreach on" : "other players in the session: BlockBreach stays off (single player only)");
		}
		return alone;
	}
	bool m_standalone = true;
	UObject *m_viewport = nullptr; // this tick's GameViewportClient (compared and read only within the tick)

	/// Other players in this session: we are connected to someone's server (NetDriver.ServerConnection), or someone is
	/// connected to ours (NetDriver.ClientConnections).
	bool multiplayer()
	{
		static Member<UObject *> world{STR("World")}, driver{STR("NetDriver")}, server{STR("ServerConnection")};
		static Member<RawPtrArray> clients{STR("ClientConnections")};
		UObject **w = m_viewport ? world.in(m_viewport) : nullptr;
		UObject **d = w && *w ? driver.in(*w) : nullptr;
		if (d == nullptr || *d == nullptr)
			return false; // no net driver: standalone
		UObject **sc = server.in(*d);
		if (sc && *sc)
			return true;
		RawPtrArray *cc = clients.in(*d);
		return cc && cc->num > 0;
	}

	/// Runs a console command through UKismetSystemLibrary::ExecuteConsoleCommand (game thread only).
	void console(UObject *pc, const TCHAR *command)
	{
		static auto *fn = UObjectGlobals::StaticFindObject<UFunction *>(nullptr, nullptr, STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
		static auto *cdo = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
		if (fn == nullptr || cdo == nullptr)
		{
			log("ExecuteConsoleCommand not found");
			return;
		}
		struct
		{
			UObject *worldContext;
			FString command;
			UObject *player;
		} params{pc, FString(command), pc};
		cdo->ProcessEvent(fn, &params);
		Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] console: {}\n"), command);
	}

	void resolve_pov(UObject *pcm)
	{
		m_pov.resolved = true;
		FProperty *cache = pcm->GetPropertyByNameInChain(STR("CameraCachePrivate"));
		auto *cacheStruct = cache ? static_cast<FStructProperty *>(cache)->GetStruct().Get() : nullptr;
		FProperty *pov = cacheStruct ? cacheStruct->FindProperty(FName(STR("POV"), FNAME_Find)) : nullptr;
		auto *povStruct = pov ? static_cast<FStructProperty *>(pov)->GetStruct().Get() : nullptr;
		if (povStruct == nullptr)
		{
			log("camera cache not found (CameraCachePrivate.POV)");
			return;
		}
		auto offset = [&](const TCHAR *name) {
			FProperty *p = povStruct->FindProperty(FName(name, FNAME_Find));
			return p ? p->GetOffset_Internal() : -1;
		};
		m_pov.cache = cache->GetOffset_Internal();
		m_pov.pov = pov->GetOffset_Internal();
		m_pov.location = offset(STR("Location"));
		m_pov.rotation = offset(STR("Rotation"));
		m_pov.fov = offset(STR("FOV"));
		m_pov.aspect = offset(STR("AspectRatio"));
		m_pov.ok = m_pov.location >= 0 && m_pov.rotation >= 0 && m_pov.fov >= 0;
		log("POV layout: cache +%d, pov +%d, location +%d, rotation +%d, fov +%d, aspect +%d", m_pov.cache, m_pov.pov,
			m_pov.location, m_pov.rotation, m_pov.fov, m_pov.aspect);
	}

	/// The local player's controller, walked from the engine: GameViewport -> GameInstance -> LocalPlayers[0].
	UObject *local_player(UEngine *engine, UObject **outPlayer)
	{
		UObject **viewport = m_gameViewport.in(engine);
		m_viewport = viewport ? *viewport : nullptr;
		UObject **instance = viewport ? m_gameInstance.in(*viewport) : nullptr;
		RawPtrArray *players = instance ? m_localPlayers.in(*instance) : nullptr;
		if (players == nullptr || players->num < 1 || players->data == nullptr || players->data[0] == nullptr)
			return nullptr;
		*outPlayer = players->data[0];
		UObject **pc = m_playerController.in(players->data[0]);
		return pc ? *pc : nullptr;
	}

	void tick(UEngine *engine)
	{
		crash_guard::Scope scope("tick");
		compositor::try_register(g_module);
		hook_window();
		++m_frame;
		if (m_toggle.exchange(false))
		{
			m_enabled = !m_enabled;
			log("passthrough %s", m_enabled ? "on" : "off");
		}
		const bool connected = m_ws.connected();
		// messages are handled further down, once this frame's player and pawn are known (handling them here used the
		// previous frame's pawn: after a death or a level change it was already destroyed)
		std::vector<std::string> messages;
		for (std::string message; m_ws.poll(message);)
		{
			// settings that touch no game object work right away, in menus too
			if (settings_op(message))
				handle_op(message);
			else
				messages.push_back(std::move(message));
		}

		UObject *player = nullptr;
		UObject *pc = local_player(engine, &player);
		UObject **pcmSlot = pc ? m_cameraManager.in(pc) : nullptr;
		UObject *pcm = pcmSlot ? *pcmSlot : nullptr;
		if (pcm == nullptr)
		{
			if (!m_loggedChain && m_frame % 600 == 0)
				log("no player camera yet (pc=%p)", static_cast<void *>(pc));
			compositor::set_active(false);
			return;
		}
		if (!m_loggedChain)
		{
			m_loggedChain = true;
			if (uint8_t *axis = m_aspectAxis.in(player))
				m_aspectConstraint = *axis;
			log("camera manager found; AspectRatioAxisConstraint = %d", m_aspectConstraint);
			// keep rendering while Minecraft (or anything else) has the focus
			console(pc, STR("t.IdleWhenNotForeground 0"));
		}
		if (!m_pov.resolved)
			resolve_pov(pcm);
		if (!m_pov.ok)
			return;

		const uint8_t *pov = reinterpret_cast<uint8_t *>(pcm) + m_pov.cache + m_pov.pov;
		const FVectorD loc = *reinterpret_cast<const FVectorD *>(pov + m_pov.location);
		const FRotatorD rot = *reinterpret_cast<const FRotatorD *>(pov + m_pov.rotation);
		const float hfov = *reinterpret_cast<const float *>(pov + m_pov.fov);
		const float povAspect = m_pov.aspect >= 0 ? *reinterpret_cast<const float *>(pov + m_pov.aspect) : 16.0f / 9.0f;

		// feet: the pawn's capsule bottom (root component = capsule, at the capsule's centre)
		double feetZ = loc.z - 170.0;
		UObject **pawn = m_pawn.in(pc);
		if (pawn && *pawn)
		{
			UObject **root = m_rootComponent.in(*pawn);
			UObject **capsule = m_capsule.in(*pawn);
			FVectorD *at = root && *root ? m_relativeLocation.in(*root) : nullptr;
			float *half = capsule && *capsule ? m_capsuleHalfHeight.in(*capsule) : nullptr;
			if (at)
				feetZ = at->z - (half ? *half : 90.0f);
		}

		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		const double aspect = bw > 0 && bh > 0 ? double(bw) / bh : 16.0 / 10.0;
		// UE's FOV is horizontal; MaintainYFOV (0) keeps the vertical fov it has at the camera's own aspect ratio
		const double d2r = 3.14159265358979 / 180.0;
		const double tanH = std::tan(hfov * 0.5 * d2r);
		const double vfov = 2.0 * std::atan(tanH / (m_aspectConstraint == 0 ? povAspect : aspect)) / d2r;

		if (!m_enabled || !connected)
		{
			// no Minecraft: no screen of its can be open (else the mouse would stay steering a cursor nobody draws)
			g_mcScreen = false;
			compositor::set_cursor(false, 0.5f, 0.5f);
			compositor::set_active(false);
			// the game state is still told (scripts wait for "in game" after loading a mission, Minecraft or not)
			const bool inGame = in_game(pc, pawn ? *pawn : nullptr);
			g_inGame = inGame;
			m_inGame = -1; // logged again once Minecraft is connected
			if (int(inGame) != m_inGameAlone)
			{
				m_inGameAlone = inGame;
				log("%s (Minecraft %s)", inGame ? "in game" : "menu, pause or no pawn", m_enabled ? "not connected" : "off");
			}
			if (m_frame % 300 == 0)
				log("cam UE (%.0f, %.0f, %.0f) passthrough %s", loc.x, loc.y, loc.z, m_enabled ? "waiting for Minecraft" : "off"); // fps.py times these
			return;
		}
		m_inGameAlone = -1; // told again if Minecraft goes away
		if (m_ws.generation() != m_generation)
		{
			// (re)connected: size Minecraft's window to RoN's picture, hide its HUD and hand, level the ground again
			m_generation = m_ws.generation();
			m_viewSent = 0;
			m_haveOffset = false;
			m_ws.send(g_build ? "{\"t\":\"hud\",\"hidden\":false}" : "{\"t\":\"hud\",\"hidden\":true}");
			log("Minecraft connected");
		}
		if (bw > 0 && bh > 0 && bw * 65536 + bh != m_viewSent)
		{
			m_viewSent = bw * 65536 + bh;
			const double scale = std::min(1.0, std::sqrt(kMaxMinecraftPixels / (double(bw) * bh))) * m_mcScale;
			send("{\"t\":\"view\",\"w\":%d,\"h\":%d}", int(bw * scale + 0.5), int(bh * scale + 0.5));
		}
		const bool placeTest = m_placeTest.exchange(false);
		// a new pawn (a level loaded) stands on a new floor: level Minecraft's ground to it again
		UObject *pawnNow = pawn ? *pawn : nullptr;
		// a new pawn: a level loaded, the mission restarted or the player respawned. No pawn (menus, loading, the death
		// screen) changes nothing: re-levelling there moved Minecraft to an empty map name's region and wiped its floors.
		if (pawnNow != nullptr && pawnNow != m_leveledPawn)
		{
			m_leveledPawn = pawnNow;
			if (g_build)
			{
				g_build = false;
				m_ws.send("{\"t\":\"key\",\"k\":\"attack\",\"down\":false}");
				m_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
				if (g_mcScreen)
					m_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
				m_ws.send("{\"t\":\"hud\",\"hidden\":true}");
				log("build mode off (a new pawn)");
			}
			const StringType before = m_mapName;
			map_offset(pawnNow);
			// the same map with the feet where they were (a restart, a respawn at the start): Minecraft's floors and
			// walls still fit, only the block collision belongs to the new level instance
			const bool same = m_haveOffset && m_mapName == before && std::abs(feetZ - m_levelFeetZ) < 30.0;
			if (same)
			{
				reset_blocks();
				log("same map, same floor: Minecraft's ground kept");
			}
			else
			{
				m_haveOffset = false;
				m_groundSent = m_wallColumns = m_wallsOpened = m_edgeWalls = 0; // the counters in the log are per level
			}
			// everything that pointed into the old level or at the old pawn
			m_pedByHandle.clear();
			m_pedHandle.clear();
			m_projectiles.clear();
			m_mobs.clear();
			m_weapon = Ref();
		}
		if (m_relevel.exchange(false))
			m_haveOffset = false;
		if (m_resyncAt != 0 && m_frame >= m_resyncAt)
		{
			m_resyncAt = 0;
			m_ws.send("{\"t\":\"blocksync\",\"r\":48}");
		}
		if ((!m_haveOffset || placeTest) && pawnNow != nullptr)
		{
			m_yOffset = kFeetY - feetZ / 100.0;
			m_haveOffset = true;
			m_sampled.clear();
			m_wallTops.clear();
			m_floorTops.clear();
			m_pillars.clear();
			m_openedTops.clear();
			m_walked.clear();
			m_levelFeetZ = feetZ;
			m_ws.send("{\"t\":\"clear\"}");
			reset_blocks();
			log("level: feet at UE z=%.1f cm -> Minecraft y=%.0f (yOffset %.3f)", feetZ, kFeetY, m_yOffset);
		}

		{
			const double d2r = 3.14159265358979 / 180.0, cp = std::cos(rot.pitch * d2r);
			m_camLoc = loc;
			m_camFwd = FVectorD{cp * std::cos(rot.yaw * d2r), cp * std::sin(rot.yaw * d2r), std::sin(rot.pitch * d2r)};
		}
		const double x = mcx(loc.x), y = mcy(loc.z), z = mcz(loc.y);
		const float yaw = wrap_degrees(float(rot.yaw) - 90.0f), pitch = float(-rot.pitch), roll = float(rot.roll) * m_rollSign;
		const double px = x, py = mcy(feetZ), pz = z;
		compositor::set_host_planes(m_nearClip, 1.0e6f);
		compositor::set_host_pose(yaw, pitch, roll, float(vfov), x, y, z);
		m_lastPawn = pawnNow;
		m_pc = pc;
		const bool inGame = in_game(pc, pawnNow);
		g_inGame = inGame;
		if (m_toggleBuild.exchange(false))
		{
			const bool build = !g_build;
			g_build = build;
			if (!build)
			{
				m_ws.send("{\"t\":\"key\",\"k\":\"attack\",\"down\":false}");
				m_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
				if (g_mcScreen)
					m_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
			}
			// Minecraft's hand, hotbar and crosshair show while building, and RoN's arms and gun don't
			send("{\"t\":\"hud\",\"hidden\":%s}", build ? "false" : "true");
			show_first_person(pawnNow, !build);
			log("build mode %s", build ? "ON: mouse, wheel and 1-9 go to Minecraft" : "off");
		}
		for (const std::string &message : messages)
			handle_op(message);
		if (int(inGame) != m_inGame)
		{
			if (inGame && m_inGame == 0)
				m_ws.send("{\"t\":\"blocksync\",\"r\":48}"); // block changes made while in a menu were dropped
			m_inGame = inGame;
			log("%s", inGame ? "in game: compositing Minecraft" : "menu, pause or no pawn: not compositing");
		}
		compositor::set_active(inGame);
		{
			const bool cursor = inGame && g_build && g_mcScreen;
			if (cursor && g_cursorDirty.exchange(false))
				send("{\"t\":\"cursor\",\"x\":%.4f,\"y\":%.4f}", g_cursorX.load(), g_cursorY.load());
			compositor::set_cursor(cursor, g_cursorX.load(), g_cursorY.load());
		}
		// Minecraft follows the camera only in the game proper: menus, loading screens, pauses and the death replay
		// have their own cameras far from the level (the main menu's is ~250 m under it), and Steve would be
		// teleported there and fall out of the world. With no pose for 2 s Minecraft lets go and Steve hovers.
		if (inGame)
			send("{\"t\":\"cam\",\"f\":%llu,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":true,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f}",
				static_cast<unsigned long long>(m_frame), x, y, z, yaw, pitch, roll, vfov, px, py, pz, yaw);

		if (inGame && pawnNow != nullptr)
		{
			// the player's own column first (its wall would be news to the probes that follow)
			std::string opens;
			UObject **root = m_rootComponent.in(pawnNow);
			FVectorD *at = root && *root ? m_relativeLocation.in(*root) : nullptr;
			if (at != nullptr && on_ground(pawnNow))
				walked(FVectorD{at->x, at->y, feetZ}, opens);
			send_opens(opens);
			sample_ground(pawnNow, x, z);
		}
		if (inGame && pawnNow != nullptr && !g_build && player_alive())
			player_shots(pawnNow);
		if (inGame && m_frame % 20 == 0)
			send_peds();
		if (inGame && m_frame % 50 == 25)
			suspects_fight_back();
		if (const int r = m_mapRequest.exchange(0); r > 0 && pawnNow != nullptr)
			log_map(pawnNow, loc, feetZ, r);
		if (const int r = m_colsRequest.exchange(0); r > 0 && pawnNow != nullptr)
			log_columns(pawnNow, loc, feetZ, r);
		if (const int pc = m_probeColRequest.exchange(-1); pc >= 0 && pawnNow != nullptr)
		{
			const int cx = int(std::floor(mcx(loc.x))) + pc / 100 - 50, cz = int(std::floor(mcz(loc.y))) + pc % 100 - 50;
			std::string out;
			m_probeDebug = true;
			log("probe column (%d, %d) from UE z %.0f down to %.0f:", cx, cz, m_levelFeetZ + kFloorsAbove, m_levelFeetZ - kFloorsBelow);
			probe_column(pawnNow, cx, cz, m_levelFeetZ + kFloorsAbove, m_levelFeetZ - kFloorsBelow, out);
			m_probeDebug = false;
			log("  -> %s", out.c_str());
		}
		{
			std::vector<std::wstring> commands;
			{
				std::lock_guard<std::mutex> lock(m_consoleLock);
				commands.swap(m_consoleQueue);
			}
			for (const std::wstring &c : commands)
				console(pc, c.c_str());
		}
		if (m_probe.exchange(false) && pawnNow != nullptr)
		{
			// what the wall test hits: across the 5 columns ahead, 1 m above the feet
			UObject *ignore = live_block_actor();
			for (int d = 1; d <= 5; ++d)
			{
				const double cx = loc.x + m_camFwd.x * 100.0 * d, cy = loc.y + m_camFwd.y * 100.0 * d, wz = feetZ + 100.0;
				FVectorD hit{};
				const bool h1 = m_tracer.trace(pawnNow, FVectorD{cx - 48, cy, wz}, FVectorD{cx + 48, cy, wz}, hit, ignore);
				const StringType c1 = h1 ? m_tracer.last_component() : STR("-");
				const bool h2 = m_tracer.trace(pawnNow, FVectorD{cx, cy - 48, wz}, FVectorD{cx, cy + 48, wz}, hit, ignore);
				const StringType c2 = h2 ? m_tracer.last_component() : STR("-");
				Output::send<LogLevel::Verbose>(STR("[RoNPassthrough] probe {} m: x-trace {} | y-trace {}\n"), d, c1, c2);
			}
		}
		if (placeTest)
		{
			place_test_blocks(x, py, z, rot.yaw);
			compositor::request_trace();
		}
		if (++m_camFrames == 1000)
			compositor::request_trace(); // one frame's render target binds in ReShade.log, ~10 s in
		if (m_frame % 300 == 0)
		{
			int frames = 0, early = 0;
			compositor::stats(frames, early);
			log("cam UE (%.0f, %.0f, %.0f) yaw %.1f pitch %.1f roll %.1f hfov %.1f -> MC (%.2f, %.2f, %.2f) vfov %.1f; bb %dx%d; frames %d, before-UI %d; ground columns %d (walls %d + %d between centres, %d opened where people walked), probes %.2f ms/frame; player shots %d, on mobs %d",
				loc.x, loc.y, loc.z, rot.yaw, rot.pitch, rot.roll, hfov, x, y, z, vfov, bw, bh, frames, early, m_groundSent, m_wallColumns, m_edgeWalls, m_wallsOpened, probe_ms(),
				m_playerShots, m_playerMobHits);
			m_probeTicks = 0;
			m_probeFrames = 0;
		}
	}

	/// Test geometry in front of the player, standing on the floor (feet level = y 64):
	/// a gold block 3 m ahead, a 3-high diamond pillar 6 m ahead and 1.5 m right, an emerald block 2 m left.
	void place_test_blocks(double x, double feetY, double z, double ueYaw)
	{
		const double r = ueYaw * 3.14159265358979 / 180.0;
		const double fx = std::cos(r), fz = std::sin(r); // forward in Minecraft x/z (UE x/y)
		const double rx = -fz, rz = fx;                   // right
		const int fy = int(std::floor(feetY + 0.01));
		auto at = [&](double ahead, double right, int &bx, int &bz) {
			bx = int(std::floor(x + fx * ahead + rx * right));
			bz = int(std::floor(z + fz * ahead + rz * right));
		};
		int bx, bz;
		at(3.0, 0.0, bx, bz);
		send("{\"t\":\"cmd\",\"c\":\"setblock %d %d %d minecraft:gold_block\"}", bx, fy, bz);
		at(6.0, 1.5, bx, bz);
		send("{\"t\":\"cmd\",\"c\":\"fill %d %d %d %d %d %d minecraft:diamond_block\"}", bx, fy, bz, bx, fy + 2, bz);
		at(2.0, -2.0, bx, bz);
		send("{\"t\":\"cmd\",\"c\":\"setblock %d %d %d minecraft:emerald_block\"}", bx, fy, bz);
		if (m_testFar)
		{
			// occlusion test: 3-high iron pillars every 4 m from 8 to 32 m ahead, 3 m left and 3 m right
			for (int d = 8; d <= 32; d += 4)
				for (double side : {-3.0, 3.0})
				{
					at(double(d), side, bx, bz);
					send("{\"t\":\"cmd\",\"c\":\"fill %d %d %d %d %d %d minecraft:iron_block\"}", bx, fy, bz, bx, fy + 2, bz);
				}
		}
		log("test blocks placed around (%.1f, %d, %.1f)%s", x, fy, z, m_testFar ? " + far pillars" : "");
	}
};

extern "C"
{
	__declspec(dllexport) RC::CppUserModBase *start_mod()
	{
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&start_mod), &g_module);
		crash_guard::install(g_module);
		return new RoNPassthrough();
	}

	__declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod)
	{
		delete mod;
		crash_guard::uninstall();
	}
}
