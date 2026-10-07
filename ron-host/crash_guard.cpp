#include "crash_guard.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	thread_local const char *t_stage = nullptr;
	HANDLE g_file = INVALID_HANDLE_VALUE;
	PVOID g_handler = nullptr;
	std::atomic<int> g_written{0}, g_writtenOurs{0};
	std::wstring g_path;

	// last operations, any thread
	constexpr int kNotes = 16;
	char g_notes[kNotes][96];
	std::atomic<unsigned> g_noteCount{0};

	// stages of the two threads that matter, readable from the handler of any thread
	std::atomic<const char *> g_gameStage{nullptr}, g_renderStage{nullptr};

	void write_line(const char *text)
	{
		if (g_file == INVALID_HANDLE_VALUE)
			return;
		DWORD written = 0;
		WriteFile(g_file, text, DWORD(std::strlen(text)), &written, nullptr);
	}

	bool fatal(DWORD code)
	{
		switch (code)
		{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_PRIV_INSTRUCTION:
			return true;
		default:
			return false;
		}
	}

	LONG CALLBACK handler(EXCEPTION_POINTERS *info)
	{
		const DWORD code = info->ExceptionRecord->ExceptionCode;
		if (!fatal(code))
			return EXCEPTION_CONTINUE_SEARCH;
		// exceptions while the mod is at work are always recorded (up to 64); others (drivers, the game's own first-chance
		// ones) only the first 8, so they can't push the mod's record out
		const bool ours = t_stage != nullptr || g_gameStage.load() != nullptr || g_renderStage.load() != nullptr;
		if ((ours ? g_writtenOurs.fetch_add(1) >= 64 : g_written.fetch_add(1) >= 8))
			return EXCEPTION_CONTINUE_SEARCH;
		void *address = info->ExceptionRecord->ExceptionAddress;
		char module[MAX_PATH] = "?";
		HMODULE owner = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(address), &owner))
		{
			GetModuleFileNameA(owner, module, MAX_PATH);
		}
		const char *name = std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module;
		SYSTEMTIME now;
		GetLocalTime(&now);
		char line[512];
		std::snprintf(line, sizeof(line),
			"%04d-%02d-%02d %02d:%02d:%02d.%03d EXCEPTION 0x%08lX at %s+0x%llx (read/write 0x%llx) thread=%s game=%s render=%s\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, code, name,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(owner)),
			static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0),
			t_stage ? t_stage : "-", g_gameStage.load() ? g_gameStage.load() : "-", g_renderStage.load() ? g_renderStage.load() : "-");
		write_line(line);
		const unsigned n = g_noteCount.load();
		for (unsigned i = n > kNotes ? n - kNotes : 0; i < n; ++i)
		{
			char note[128];
			std::snprintf(note, sizeof(note), "    last op: %s\n", g_notes[i % kNotes]);
			write_line(note);
		}
		FlushFileBuffers(g_file);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void set_stage(const char *name)
	{
		t_stage = name;
		// the render thread's stages start with "render"; everything else is the game thread's
		if (name && std::strncmp(name, "render", 6) == 0)
			g_renderStage = name;
		else if (name)
			g_gameStage = name;
	}
}

namespace crash_guard
{
	void install(void *module)
	{
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH);
		g_path = std::filesystem::path(path).parent_path().parent_path() / L"crash_breadcrumbs.log";
		// keep the previous run's records (read at start), then append this run's
		g_file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		SYSTEMTIME now;
		GetLocalTime(&now);
		char line[128];
		std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d START pid=%lu\n", now.wYear, now.wMonth, now.wDay,
			now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
		write_line(line);
		g_handler = AddVectoredExceptionHandler(1, handler);
	}

	void uninstall()
	{
		if (g_handler)
			RemoveVectoredExceptionHandler(g_handler);
		g_handler = nullptr;
		if (g_file != INVALID_HANDLE_VALUE)
			CloseHandle(g_file);
		g_file = INVALID_HANDLE_VALUE;
	}

	void stage(const char *name)
	{
		set_stage(name);
	}

	void note(const char *format, ...)
	{
		const unsigned i = g_noteCount.fetch_add(1) % kNotes;
		va_list args;
		va_start(args, format);
		std::vsnprintf(g_notes[i], sizeof(g_notes[i]), format, args);
		va_end(args);
	}

	bool previous_run_crashed_in_early_render()
	{
		// the log's runs are separated by START lines; the last one is this run, the one before it the previous run
		std::ifstream in(g_path);
		std::string line, previousRun, currentRun;
		while (std::getline(in, line))
		{
			if (line.find(" START ") != std::string::npos)
			{
				previousRun = currentRun;
				currentRun.clear();
			}
			else
				currentRun += line + "\n";
		}
		// a fatal exception of the previous run that happened in the mod's early (before-UI) rendering
		for (size_t at = previousRun.find(" EXCEPTION "); at != std::string::npos; at = previousRun.find(" EXCEPTION ", at + 1))
		{
			const size_t end = previousRun.find('\n', at);
			if (previousRun.substr(at, end - at).find("render=render_early") != std::string::npos ||
				previousRun.substr(at, end - at).find("thread=render_early") != std::string::npos)
				return true;
		}
		return false;
	}

	Scope::Scope(const char *name) : previous(t_stage)
	{
		set_stage(name);
	}

	Scope::~Scope()
	{
		// back to the enclosing stage; leaving the outermost one clears this thread's slot
		const char *leaving = t_stage;
		t_stage = previous;
		const bool render = leaving && std::strncmp(leaving, "render", 6) == 0;
		(render ? g_renderStage : g_gameStage) = previous;
	}
}
