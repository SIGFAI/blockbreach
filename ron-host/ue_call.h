// Calling a UFunction through ProcessEvent with its parameters laid out by reflection (offsets from the function's
// own properties), so no engine headers or hard-coded layouts are needed. Game thread only.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <type_traits>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace ue
{
	struct Vec
	{
		double x, y, z;
	};

	/// FTransform (UE5, doubles): rotation quaternion, translation, scale, each 16-byte aligned (32 bytes apiece).
	struct alignas(16) Transform
	{
		double qx = 0, qy = 0, qz = 0, qw = 1;
		double tx = 0, ty = 0, tz = 0, pad0 = 0;
		double sx = 1, sy = 1, sz = 1, pad1 = 0;
	};
	static_assert(sizeof(Transform) == 96);

	/// TArray<T*> as UE lays it out (the data is ours: only pass it to parameters the callee doesn't resize).
	struct PtrArray
	{
		RC::Unreal::UObject **data;
		int32_t num, max;
	};

	inline RC::Unreal::UObject *find(const TCHAR *path)
	{
		return RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject *>(nullptr, nullptr, path);
	}

	class Call
	{
	public:
		/// fn: "/Script/Module.Class:Function". The target defaults to the class's default object (static functions).
		explicit Call(const TCHAR *fn)
		{
			m_fn = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction *>(nullptr, nullptr, fn);
			if (m_fn == nullptr)
			{
				RC::Output::send<RC::LogLevel::Warning>(STR("[RoNPassthrough] no UFunction {}\n"), fn);
				return;
			}
			m_size = m_fn->GetParmsSize();
			if (m_size > int32_t(sizeof(m_params)))
			{
				RC::Output::send<RC::LogLevel::Warning>(STR("[RoNPassthrough] {}: {} bytes of parameters\n"), fn, m_size);
				m_fn = nullptr;
				return;
			}
			std::memset(m_params, 0, sizeof(m_params));
		}

		bool ok() const
		{
			return m_fn != nullptr;
		}

		template <typename T>
		Call &set(const TCHAR *name, const T &value)
		{
			if (RC::Unreal::FProperty *p = prop(name))
			{
				auto *b = RC::Unreal::CastField<RC::Unreal::FBoolProperty>(p);
				if constexpr (std::is_arithmetic_v<T>)
				{
					if (b != nullptr)
					{
						b->SetPropertyValueInContainer(m_params, bool(value));
						return *this;
					}
				}
				std::memcpy(m_params + p->GetOffset_Internal(), &value, sizeof(T));
			}
			return *this;
		}

		/// Raw bytes into a parameter (a struct copied from another call's result), at most the parameter's size.
		Call &raw(const TCHAR *name, const void *data, size_t size)
		{
			if (RC::Unreal::FProperty *p = prop(name))
				std::memcpy(m_params + p->GetOffset_Internal(), data, std::min<size_t>(size, size_t(p->GetSize())));
			return *this;
		}

		/// A parameter's bytes after the call (an out struct, e.g. an FHitResult) and its size.
		const uint8_t *bytes(const TCHAR *name, int32_t &size)
		{
			RC::Unreal::FProperty *p = prop(name);
			size = p ? p->GetSize() : 0;
			return p ? m_params + p->GetOffset_Internal() : nullptr;
		}

		Call &name(const TCHAR *param, const TCHAR *value)
		{
			const RC::Unreal::FName n(value, RC::Unreal::FNAME_Add);
			return set(param, n);
		}

		template <typename T>
		T get(const TCHAR *name)
		{
			T value{};
			if (RC::Unreal::FProperty *p = prop(name))
			{
				auto *b = RC::Unreal::CastField<RC::Unreal::FBoolProperty>(p);
				if constexpr (std::is_arithmetic_v<T>)
				{
					if (b != nullptr)
						return T(b->GetPropertyValueInContainer(m_params));
				}
				std::memcpy(&value, m_params + p->GetOffset_Internal(), sizeof(T));
			}
			return value;
		}

		/// Calls it on target (or on the function's class default object when target is null).
		bool run(RC::Unreal::UObject *target = nullptr)
		{
			if (m_fn == nullptr)
				return false;
			if (target == nullptr)
			{
				if (m_cdo == nullptr)
					m_cdo = static_cast<RC::Unreal::UClass *>(m_fn->GetOuterPrivate())->GetClassDefaultObject().Get();
				target = m_cdo;
			}
			if (target == nullptr)
				return false;
			target->ProcessEvent(m_fn, m_params);
			return true;
		}

	private:
		RC::Unreal::FProperty *prop(const TCHAR *name)
		{
			if (m_fn == nullptr)
				return nullptr;
			RC::Unreal::FProperty *p = m_fn->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
			if (p == nullptr)
				RC::Output::send<RC::LogLevel::Warning>(STR("[RoNPassthrough] no parameter {}\n"), name);
			return p;
		}

		RC::Unreal::UFunction *m_fn = nullptr;
		RC::Unreal::UObject *m_cdo = nullptr;
		int32_t m_size = 0;
		alignas(16) uint8_t m_params[1024];
	};
}
