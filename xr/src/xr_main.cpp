// EchoXR runtime entry point -- replaces Revive's injection glue (ReviveXR/main.cpp).
//
// Revive normally INJECTS itself: it hooks LoadLibrary so a game asking for the
// Oculus runtime gets Revive instead. Echo doesn't need that. Echo's own LibOVR
// loader already checks LIBOVR_DLL_DIR for LibOVRRT64_1.dll, so this DLL is
// simply built under that name and loaded like the real Oculus runtime.
//
// Revive's core still calls these around instance creation (to un-hook its
// LoadLibrary detours while the OpenXR loader runs). There are no loader hooks
// here, so they are no-ops.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "echoxr_diag.h"

void AttachDetours() {}
void DetachDetours() {}

// ---------------------------------------------------------------------------
// runtime.log -- written next to this DLL. Every failed OpenXR call (CHK_XR in
// ReviveXR/Common.h) lands here with the call text and source line, so a
// runtime rejecting something names the exact call instead of a bare error code.
// Past echoxr::kLogCapBytes only failed calls and a removal's report get in, so the file stays
// small enough for the launcher's log upload.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static char g_logPath[MAX_PATH];
static uint64_t g_logBytes = 0;
static bool g_logCapped = false;

static void LogLine(bool important, const char* fmt, va_list ap)
{
	if (!g_logPath[0]) return;
	EnterCriticalSection(&g_logLock);
	important = important || EchoXR_LogImportant();
	FILE* f = nullptr;
	if (echoxr::KeepLogLine(g_logBytes, important) && fopen_s(&f, g_logPath, "a") == 0 && f) {
		if (!g_logCapped && !echoxr::KeepLogLine(g_logBytes, false)) {
			g_logCapped = true;
			fprintf(f, "runtime.log is %llu MB now: from here on only failed calls and a device removal's report\n",
				(unsigned long long)(g_logBytes >> 20));
		}
		SYSTEMTIME t; GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		vfprintf(f, fmt, ap);
		fputc('\n', f);
		g_logBytes = (uint64_t)_ftelli64(f);
		fclose(f);
	}
	LeaveCriticalSection(&g_logLock);
}

void EchoXR_Log(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	LogLine(false, fmt, ap);
	va_end(ap);
}

static void LogImportant(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	LogLine(true, fmt, ap);
	va_end(ap);
}

void EchoXR_LogFail(const char* call, int result, const char* file, int line)
{
	const char* base = strrchr(file, '\\');
	LogImportant("FAILED %d at %s:%d  %s", result, base ? base + 1 : file, line, call);
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_logLock);
		GetModuleFileNameA((HMODULE)hModule, g_logPath, MAX_PATH);
		char* slash = strrchr(g_logPath, '\\');
		if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), "runtime.log");
		FILE* f = nullptr;
		if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);     // fresh log per launch
		EchoXR_Log("EchoXR %s runtime loaded (LibOVRRT64_1 = ReviveXR over OpenXR)", ECHOXR_VERSION);
		EchoXR_Log(EchoXR_Max() ? "Logging everything (ECHOXR_VERBOSE=max)"
			: "Logging the default lines (ECHOXR_VERBOSE=max, or EchoXR.exe --verbose max, logs everything)");
	}
	return TRUE;
}
