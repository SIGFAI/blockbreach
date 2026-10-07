// A flight recorder for crashes: what the mod was doing (a stage name per thread, and the last few operations), written
// to crash_breadcrumbs.log next to the mod's DLL by a vectored exception handler the moment an access violation or
// another fatal exception happens anywhere in the process. scripts\crash_check.py matches it to RoN's crash report.
//
// At start the mod reads that log: if the previous run crashed inside the mod's early (before-UI) rendering, this run
// composites at present instead (safe mode), so one bad combination of settings can't crash every session.
#pragma once

namespace crash_guard
{
	/// Installs the handler and opens the log (call once, from start_mod).
	void install(void *module);
	void uninstall();

	/// What the calling thread is doing now ("tick", "render_early", ...); a string literal, nullptr when idle.
	void stage(const char *name);
	/// A short note on the last operations (ring of 16): "op proj", "damage radial", ...
	void note(const char *format, ...);

	/// True if the previous run's last record was a fatal exception during early rendering, and RoN's newest crash
	/// report is from then: the caller should composite at present this time.
	bool previous_run_crashed_in_early_render();

	/// RAII: sets the thread's stage for a scope.
	struct Scope
	{
		const char *previous;
		explicit Scope(const char *name);
		~Scope();
	};
}
