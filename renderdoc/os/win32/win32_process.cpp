/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "os/win32/sr44_diagnostics.h"
#include "strings/string_utils.h"

#include <fstream>
#include <string>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

static bool IsSR44LaunchDiagnosticsEnabled()
{
  char value[16] = {};
  DWORD len = GetEnvironmentVariableA("RENDERTEST_SR44_LAUNCH_DIAGNOSTICS", value,
                                     sizeof(value));

  if(len == 0 || len >= sizeof(value))
    return false;

  return value[0] == '1' || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' ||
         value[0] == 'T';
}

static DWORD GetSR44LaunchObservationMilliseconds()
{
  char value[32] = {};
  DWORD len = GetEnvironmentVariableA("RENDERTEST_SR44_LAUNCH_OBSERVE_MS", value,
                                     sizeof(value));
  if(len == 0 || len >= sizeof(value))
    return 10000;

  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if(end == value || *end != 0)
    return 10000;

  return (DWORD)parsed;
}

static std::string JsonEscape(const char *value)
{
  std::string escaped;

  if(value == NULL)
    return escaped;

  for(const unsigned char *p = (const unsigned char *)value; *p != 0; ++p)
  {
    switch(*p)
    {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if(*p < 0x20)
        {
          char control[7] = {};
          snprintf(control, sizeof(control), "\\u%04x", (unsigned int)*p);
          escaped += control;
        }
        else
        {
          escaped += (char)*p;
        }
        break;
    }
  }

  return escaped;
}

static DWORD g_SR44DiagnosticTargetPid = 0;

static void LogSR44LaunchJson(const char *stage, const char *details, DWORD targetPid = 0,
                              DWORD processExitCode = STILL_ACTIVE)
{
  rdcstr jsonfile = Process::GetEnvVariable("RENDERTEST_SR44_LAUNCH_JSONL");
  if(jsonfile.empty())
    return;

  SYSTEMTIME now = {};
  GetSystemTime(&now);

  std::ofstream output(jsonfile.c_str(), std::ios::out | std::ios::app);
  if(!output)
    return;

  output << "{\"timestamp_utc\":\"" << StringFormat::Fmt(
                "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay,
                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds)
                .c_str()
         << "\",\"logger_pid\":" << GetCurrentProcessId() << ",\"target_pid\":"
         << (targetPid != 0 ? targetPid : g_SR44DiagnosticTargetPid) << ",\"tick\":"
         << GetTickCount64() << ",\"stage\":\""
         << JsonEscape(stage) << "\",\"details\":\"" << JsonEscape(details)
         << "\",\"process_exit_code\":";

  if(processExitCode == STILL_ACTIVE)
    output << "null";
  else
    output << processExitCode;

  output << "}\n";
}

void LogSR44LaunchStage(const char *stage, const char *details)
{
  if(!IsSR44LaunchDiagnosticsEnabled())
    return;

  RDCLOG("[SR-4.4 launch] %s: %s (tick=%llu)", stage, details,
         (unsigned long long)GetTickCount64());
  LogSR44LaunchJson(stage, details);
}

static void ConfigureSR44LaunchDiagnostics()
{
  if(!IsSR44LaunchDiagnosticsEnabled())
    return;

  rdcstr logfile = Process::GetEnvVariable("RENDERTEST_SR44_LAUNCH_LOG");
  if(!logfile.empty())
    RDCLOGFILE(logfile.c_str());

  RDCLOGOUTPUT();

  LogSR44LaunchStage("diagnostics", logfile.empty() ? "enabled without explicit log file"
                                                      : logfile.c_str());
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

// Forward declaration (used by SetThreadContext path inside InjectDLL)
uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName);

rdcstr InjectDLL(HANDLE hProcess, rdcwstr libName, HANDLE hPrimaryThread = NULL)
{
  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    DWORD err = GetLastError();
    return StringFormat::Fmt("Couldn't get handle for kernel32.dll: %u", err);
  }

  FARPROC pLoadLibraryW = GetProcAddress(kernel32, "LoadLibraryW");
  if(pLoadLibraryW == NULL)
  {
    DWORD err = GetLastError();
    return StringFormat::Fmt("Couldn't get LoadLibraryW address: %u", err);
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT, PAGE_READWRITE);
  if(!remoteMem)
  {
    DWORD err = GetLastError();
    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("VirtualAllocEx failed",
                         StringFormat::Fmt("pid=%lu dll='%ls' size=%zu error=%lu",
                                            (unsigned long)GetProcessId(hProcess), libName.c_str(),
                                            sizeof(dllPath), (unsigned long)err)
                             .c_str());

    return StringFormat::Fmt("Couldn't allocate remote memory for DLL '%ls': %u", libName.c_str(),
                             err);
  }

  BOOL success = WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, sizeof(dllPath), NULL);
  if(!success)
  {
    DWORD err = GetLastError();
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return StringFormat::Fmt("Couldn't write remote memory %p with dllPath '%ls': %u", remoteMem,
                             dllPath, err);
  }

  // Scheme A: when we have the primary thread from CREATE_SUSPENDED, use SetThreadContext
  // with a stub + dedicated remote stack so the real thread stack is never touched.
  if(hPrimaryThread != NULL)
  {
#if ENABLED(RDOC_X64)
    CONTEXT saved = {};
    saved.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(hPrimaryThread, &saved))
    {
      DWORD err = GetLastError();
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("GetThreadContext failed: %u", err);
    }

    // Remote block: [done:4][pad:4][stub]
    // Stub calls LoadLibraryW(path), sets done=1, spins.
    // sub rsp, 0x20 (not 0x28): with RSP 16-byte aligned, after `call` the callee
    // sees RSP ≡ 8 (mod 16) as required by the Windows x64 ABI.
    unsigned char stub[] = {
        0x48, 0x83, 0xEC, 0x20,                             // sub rsp, 0x20
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov rcx, path
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov rax, LoadLibraryW
        0xFF, 0xD0,                                         // call rax
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov rax, done
        0xC7, 0x00, 0x01, 0x00, 0x00, 0x00,                 // mov dword [rax], 1
        0xEB, 0xFE,                                         // jmp $
    };

    const size_t codeSize = sizeof(stub);
    const size_t blockSize = 8 + codeSize;
    void *codeMem =
        VirtualAllocEx(hProcess, NULL, blockSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if(!codeMem)
    {
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("Couldn't allocate LoadLibrary stub: %u", GetLastError());
    }

    const size_t remoteStackSize = 64 * 1024;
    void *remoteStack =
        VirtualAllocEx(hProcess, NULL, remoteStackSize, MEM_COMMIT, PAGE_READWRITE);
    if(!remoteStack)
    {
      VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("Couldn't allocate LoadLibrary remote stack: %u", GetLastError());
    }

    const uint64_t pathAddr = (uint64_t)(uintptr_t)remoteMem;
    const uint64_t doneAddr = (uint64_t)(uintptr_t)codeMem;
    const uint64_t stubAddr = doneAddr + 8;
    const uint64_t loadAddr = (uint64_t)(uintptr_t)pLoadLibraryW;

    memcpy(stub + 6, &pathAddr, 8);
    memcpy(stub + 16, &loadAddr, 8);
    memcpy(stub + 28, &doneAddr, 8);

    unsigned char block[8 + sizeof(stub)] = {};
    memcpy(block + 8, stub, sizeof(stub));
    if(!WriteProcessMemory(hProcess, codeMem, block, sizeof(block), NULL))
    {
      DWORD err = GetLastError();
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("Couldn't write LoadLibrary stub: %u", err);
    }

    CONTEXT ctx = saved;
    ctx.Rip = stubAddr;
    ctx.Rsp = ((uint64_t)(uintptr_t)remoteStack + remoteStackSize) & ~0xFull;

    if(!SetThreadContext(hPrimaryThread, &ctx))
    {
      DWORD err = GetLastError();
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("SetThreadContext failed: %u", err);
    }

    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("SetThreadContext", "LoadLibraryW stub + remote stack dispatched");

    SetLastError(ERROR_SUCCESS);
    DWORD resumeRet = ResumeThread(hPrimaryThread);
    DWORD resumeErr = GetLastError();
    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("SetThreadContext ResumeThread",
                         StringFormat::Fmt("return=%lu lastError=%lu", (unsigned long)resumeRet,
                                            (unsigned long)resumeErr)
                             .c_str());

    if(resumeRet == (DWORD)-1)
    {
      SetThreadContext(hPrimaryThread, &saved);
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("ResumeThread after SetThreadContext failed: %u", resumeErr);
    }

    // Prefer polling the done flag; also accept module appearance as success.
    DWORD pid = GetProcessId(hProcess);
    const char *rdoc_dll_name = STRINGIZE(RDOC_BASE_NAME) ".dll";
    uintptr_t loc = 0;
    const DWORD pollTimeoutMs = 10000;
    const DWORD pollIntervalMs = 20;
    DWORD waited = 0;
    uint32_t done = 0;

    // MUST wait for done==1. Breaking on module presence alone can suspend the thread while
    // still inside LoadLibrary/DllMain (loader lock held) → process deadlocks after final Resume
    // and never shows a window.
    while(waited < pollTimeoutMs)
    {
      SIZE_T nr = 0;
      if(ReadProcessMemory(hProcess, codeMem, &done, sizeof(done), &nr) && nr == sizeof(done) &&
         done == 1)
      {
        loc = FindRemoteDLL(pid, rdoc_dll_name);
        break;
      }

      DWORD exitCode = 0;
      if(GetExitCodeProcess(hProcess, &exitCode))
      {
        if(IsSR44LaunchDiagnosticsEnabled())
          LogSR44LaunchStage("poll exit code",
                             StringFormat::Fmt("pid=%lu exit=%lu waited=%u done=%u",
                                                (unsigned long)pid, (unsigned long)exitCode,
                                                (unsigned)waited, (unsigned)done)
                                 .c_str());

        if(exitCode != STILL_ACTIVE)
        {
          VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
          VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
          VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
          return StringFormat::Fmt("Target process exited during SetThreadContext LoadLibrary "
                                   "(code=%lu) before stub completed",
                                   (unsigned long)exitCode);
        }
      }

      Sleep(pollIntervalMs);
      waited += pollIntervalMs;
    }

    // Re-suspend and fully restore original context (no stack damage)
    SuspendThread(hPrimaryThread);
    SetThreadContext(hPrimaryThread, &saved);

    if(loc == 0)
      loc = FindRemoteDLL(pid, rdoc_dll_name);

    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("SetThreadContext re-SuspendThread",
                         StringFormat::Fmt("loc=0x%llx waited=%lu done=%u",
                                            (unsigned long long)loc, (unsigned long)waited,
                                            (unsigned)done)
                             .c_str());

    VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, codeMem, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);

    if(done != 1)
    {
      return StringFormat::Fmt("SetThreadContext LoadLibraryW stub did not signal done after %lu ms "
                               "(loc=0x%llx). Refusing to continue to avoid loader-lock deadlock.",
                               (unsigned long)pollTimeoutMs, (unsigned long long)loc);
    }

    if(loc == 0)
    {
      return StringFormat::Fmt("SetThreadContext LoadLibraryW completed but module '%s' not found",
                               rdoc_dll_name);
    }

    return {};
#else
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return rdcstr("SetThreadContext LoadLibrary path not implemented for 32-bit");
#endif
  }

  // Fallback / Attach path: classic CreateRemoteThread
  {
    HANDLE hThread = CreateRemoteThread(
        hProcess, NULL, 1024 * 1024U, (LPTHREAD_START_ROUTINE)pLoadLibraryW, remoteMem, 0, NULL);
    if(hThread)
    {
      DWORD waitResult = WaitForSingleObject(hThread, INFINITE);
      DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
      DWORD loadResult = 0;
      BOOL gotExitCode = FALSE;
      DWORD exitCodeError = ERROR_SUCCESS;

      if(waitResult == WAIT_OBJECT_0)
      {
        gotExitCode = GetExitCodeThread(hThread, &loadResult);
        if(!gotExitCode)
          exitCodeError = GetLastError();
      }

      CloseHandle(hThread);

      if(waitResult != WAIT_OBJECT_0)
      {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return StringFormat::Fmt("Waiting for remote LoadLibraryW failed: wait=0x%08x error=%u",
                                 waitResult, waitError);
      }

      if(!gotExitCode)
      {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return StringFormat::Fmt("Couldn't read remote LoadLibraryW result: %u", exitCodeError);
      }

      if(loadResult == 0)
      {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return StringFormat::Fmt("Remote LoadLibraryW returned NULL for '%ls'", dllPath);
      }
    }
    else
    {
      DWORD err = GetLastError();
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return StringFormat::Fmt("Couldn't create remote thread for LoadLibraryW: %u", err);
    }

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  }

  return {};
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

bool InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen, HANDLE hPrimaryThread = NULL)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return false;
  }

  if(hProcess == NULL)
  {
    RDCERR("Invalid process handle for injected call to %s", funcName);
    return false;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  if(func_local == 0 || renderdoc_remote == 0)
  {
    RDCERR("Couldn't resolve injected function %s (local=%p remote=%p)", funcName,
           (void *)func_local, (void *)renderdoc_remote);
    return false;
  }

  // we've found the export in our local instance of the module, now calculate the offset and
  // so get the function in the remote module (which might be loaded at a different base address)
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  // Scheme A path: hijack the suspended primary thread with a small stub that calls the target
  // function, writes a completion flag, then spins. We poll the flag, re-suspend, and restore
  // the original context so subsequent calls / final Resume still work.
  if(hPrimaryThread != NULL)
  {
#if ENABLED(RDOC_X64)
    // Layout: [dataLen bytes payload] [8-byte aligned done flag] [shellcode]
    const size_t doneOffset = (dataLen + 7) & ~(size_t)7;
    const size_t codeOffset = doneOffset + 8;

    // x64 stub:
    //   sub rsp, 0x20   (shadow space; keeps 16-byte alignment for call)
    //   mov rcx, <data>
    //   mov rax, <func>
    //   call rax
    //   mov rax, <done>
    //   mov dword [rax], 1
    //   jmp $
    unsigned char stub[] = {
        0x48, 0x83, 0xEC, 0x20,                                     // sub rsp, 0x20
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,                         // mov rcx, imm64
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                         // mov rax, imm64
        0xFF, 0xD0,                                                 // call rax
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                         // mov rax, imm64
        0xC7, 0x00, 0x01, 0x00, 0x00, 0x00,                         // mov dword [rax], 1
        0xEB, 0xFE,                                                 // jmp $
    };
    const size_t totalSize = codeOffset + sizeof(stub);

    void *remoteMem =
        VirtualAllocEx(hProcess, NULL, totalSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if(remoteMem == NULL)
    {
      RDCERR("Couldn't allocate remote memory for SetThreadContext call %s: %lu", funcName,
             (unsigned long)GetLastError());
      return false;
    }

    // Dedicated remote stack so we never clobber the real thread stack (avoids 0xC0000409 /GS).
    const size_t remoteStackSize = 64 * 1024;
    void *remoteStack =
        VirtualAllocEx(hProcess, NULL, remoteStackSize, MEM_COMMIT, PAGE_READWRITE);
    if(remoteStack == NULL)
    {
      RDCERR("Couldn't allocate remote stack for %s: %lu", funcName,
             (unsigned long)GetLastError());
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    // Patch absolute addresses into the stub
    const uint64_t remoteData = (uint64_t)(uintptr_t)remoteMem;
    const uint64_t remoteDone = remoteData + doneOffset;
    const uint64_t remoteCode = remoteData + codeOffset;
    const uint64_t remoteFunc = (uint64_t)func_remote;

    memcpy(stub + 6, &remoteData, 8);
    memcpy(stub + 16, &remoteFunc, 8);
    memcpy(stub + 28, &remoteDone, 8);

    // Write payload (zero-fill the gap + done flag), then stub
    rdcarray<byte> block;
    block.resize(totalSize);
    memset(block.data(), 0, totalSize);
    memcpy(block.data(), data, dataLen);
    memcpy(block.data() + codeOffset, stub, sizeof(stub));

    SIZE_T numWritten = 0;
    if(!WriteProcessMemory(hProcess, remoteMem, block.data(), totalSize, &numWritten) ||
       numWritten != totalSize)
    {
      DWORD err = GetLastError();
      RDCERR("Couldn't write remote stub for %s: %lu", funcName, (unsigned long)err);
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    CONTEXT saved = {};
    saved.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(hPrimaryThread, &saved))
    {
      RDCERR("GetThreadContext failed for %s: %lu", funcName, (unsigned long)GetLastError());
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    CONTEXT ctx = saved;
    ctx.Rip = remoteCode;
    // Point RSP at the top of the dedicated remote stack (16-byte aligned).
    // Stub does sub rsp, 0x20 for shadow space; original thread stack is untouched.
    ctx.Rsp = ((uint64_t)(uintptr_t)remoteStack + remoteStackSize) & ~0xFull;

    if(!SetThreadContext(hPrimaryThread, &ctx))
    {
      RDCERR("SetThreadContext failed for %s: %lu", funcName, (unsigned long)GetLastError());
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage(funcName, "SetThreadContext stub dispatched (remote stack)");

    SetLastError(ERROR_SUCCESS);
    DWORD resumeRet = ResumeThread(hPrimaryThread);
    if(resumeRet == (DWORD)-1)
    {
      RDCERR("ResumeThread failed for %s: %lu", funcName, (unsigned long)GetLastError());
      SetThreadContext(hPrimaryThread, &saved);
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    // Poll completion flag
    const DWORD pollTimeoutMs = 10000;
    const DWORD pollIntervalMs = 10;
    DWORD waited = 0;
    uint32_t done = 0;
    bool completed = false;

    while(waited < pollTimeoutMs)
    {
      SIZE_T numRead = 0;
      if(ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)remoteDone, &done, sizeof(done),
                           &numRead) &&
         numRead == sizeof(done) && done == 1)
      {
        completed = true;
        break;
      }

      DWORD exitCode = 0;
      if(GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE)
      {
        RDCERR("Target exited during injected call %s (code=%lu)", funcName,
               (unsigned long)exitCode);
        VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
      }

      Sleep(pollIntervalMs);
      waited += pollIntervalMs;
    }

    // Re-suspend and restore original context (registers + original stack pointer)
    SuspendThread(hPrimaryThread);
    SetThreadContext(hPrimaryThread, &saved);

    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage(funcName, completed ? StringFormat::Fmt("completed waited=%lu",
                                                                   (unsigned long)waited)
                                                   .c_str()
                                             : StringFormat::Fmt("TIMEOUT waited=%lu",
                                                                   (unsigned long)waited)
                                                   .c_str());

    if(!completed)
    {
      RDCERR("SetThreadContext call %s timed out after %lu ms", funcName,
             (unsigned long)pollTimeoutMs);
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    // Read back possibly-modified payload
    SIZE_T numRead = 0;
    if(!ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numRead) || numRead != dataLen)
    {
      RDCERR("Couldn't read remote memory after %s: %lu", funcName,
             (unsigned long)GetLastError());
      VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
      VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
      return false;
    }

    VirtualFreeEx(hProcess, remoteStack, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    RDCDEBUG("Injected call %s completed via SetThreadContext", funcName);
    return true;
#else
    // 32-bit SetThreadContext path not implemented here; fall through to CreateRemoteThread
    RDCWARN("SetThreadContext InjectFunctionCall not implemented for 32-bit, falling back");
#endif
  }

  // Fallback / Attach path: classic CreateRemoteThread
  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(remoteMem == NULL)
  {
    RDCERR("Couldn't allocate remote memory for injected call %s: %lu", funcName,
           (unsigned long)GetLastError());
    return false;
  }

  SIZE_T numWritten = 0;
  if(!WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten) || numWritten != dataLen)
  {
    DWORD err = GetLastError();
    RDCERR("Couldn't write remote memory for injected call %s: %lu (%llu/%llu bytes)", funcName,
           (unsigned long)err, (unsigned long long)numWritten, (unsigned long long)dataLen);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  HANDLE hThread =
      CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
  if(hThread == NULL)
  {
    RDCERR("Couldn't create remote thread for injected call %s: %lu", funcName,
           (unsigned long)GetLastError());
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  DWORD waitResult = WaitForSingleObject(hThread, INFINITE);
  if(waitResult != WAIT_OBJECT_0)
  {
    RDCERR("Couldn't wait for injected call %s: result %lu error %lu", funcName,
           (unsigned long)waitResult, (unsigned long)GetLastError());
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  DWORD threadExitCode = 0;
  if(!GetExitCodeThread(hThread, &threadExitCode))
  {
    RDCERR("Couldn't get exit code for injected call %s: %lu", funcName,
           (unsigned long)GetLastError());
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  SIZE_T numRead = 0;
  if(!ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numRead) || numRead != dataLen)
  {
    DWORD err = GetLastError();
    RDCERR("Couldn't read remote memory for injected call %s: %lu (%llu/%llu bytes)", funcName,
           (unsigned long)err, (unsigned long long)numRead, (unsigned long long)dataLen);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return false;
  }

  CloseHandle(hThread);
  if(!VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE))
  {
    RDCERR("Couldn't free remote memory for injected call %s: %lu", funcName,
           (unsigned long)GetLastError());
    return false;
  }

  RDCDEBUG("Injected call %s completed with remote thread exit code %lu", funcName,
           (unsigned long)threadExitCode);
  return true;
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("CreateProcessW failed", StringFormat::Fmt("app='%s' error=%lu", app.c_str(),
                                                                       (unsigned long)err)
                             .c_str());
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }
  else if(IsSR44LaunchDiagnosticsEnabled())
  {
    LogSR44LaunchStage("CreateProcessW succeeded",
                       StringFormat::Fmt("app='%s' pid=%lu tid=%lu", app.c_str(),
                                          (unsigned long)pi.dwProcessId,
                                          (unsigned long)pi.dwThreadId)
                           .c_str());
  }

  return pi;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit,
                                                       void *hPrimaryThread)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(IsSR44LaunchDiagnosticsEnabled())
  {
    if(hProcess)
      LogSR44LaunchStage("OpenProcess succeeded",
                         StringFormat::Fmt("pid=%lu handle=%p", (unsigned long)pid, hProcess)
                             .c_str());
    else
      LogSR44LaunchStage("OpenProcess failed",
                         StringFormat::Fmt("pid=%lu error=%lu", (unsigned long)pid,
                                            (unsigned long)GetLastError())
                             .c_str());
  }

  if(hProcess == NULL)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Couldn't open target process %u: %lu", pid,
                     GetLastError());
    return {result, 0};
  }

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    if(IsSR44LaunchDiagnosticsEnabled())
      LogSR44LaunchStage("delayForDebugger",
                         StringFormat::Fmt("pid=%lu seconds=%u", (unsigned long)pid,
                                            (unsigned)opts.delayForDebugger)
                             .c_str());
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit renderdoccmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build of RenderDoc. Please run a "
                       "64-bit build of RenderDoc");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness rendertestcmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\rendertestcmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\rendertestcmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --capfile=\"%ls\" --debuglog=\"%ls\" --capopts=\"%hs\"",
        renderdocPath, pid, wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false,
                                   CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit renderdoccmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit renderdoccmd to capture 32-bit program."
          "If this is a locally built RenderDoc you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit renderdoccmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  rdcstr injectError = InjectDLL(hProcess, renderdocPath, (HANDLE)hPrimaryThread);

  if(IsSR44LaunchDiagnosticsEnabled())
    LogSR44LaunchStage("remote DLL load", injectError.empty() ? "InjectDLL returned success"
                                                               : injectError.c_str());

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);

  uintptr_t loc = FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");

  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  if(loc == 0)
  {
    if(!injectError.empty())
    {
      SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed, "Failed to inject %s.dll: %s",
                       rdoc_dll, injectError.c_str());
    }
    else
    {
      SET_ERROR_RESULT(
          result.first, ResultCode::InjectionFailed,
          "LoadLibraryW succeeded but %s.dll was not visible in the target module list.", rdoc_dll);
    }
  }
  else
  {
    // safe to cast away the const as we know these functions don't modify the parameters

    bool configOk = true;
    HANDLE primaryThread = (HANDLE)hPrimaryThread;

    auto callInjected = [&](const char *name, void *data, size_t dataLen) {
      bool ok = InjectFunctionCall(hProcess, loc, name, data, dataLen, primaryThread);
      if(IsSR44LaunchDiagnosticsEnabled())
        LogSR44LaunchStage(name, ok ? "completed" : "failed");
      if(!ok && configOk)
      {
        SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed,
                         "Injected call %s failed", name);
        configOk = false;
      }
      return ok;
    };

    if(!capturefile.empty())
      callInjected("INTERNAL_SetCaptureFile", (void *)capturefile.c_str(), capturefile.size() + 1);

    rdcstr debugLogfile = RDCGETLOGFILE();

    callInjected("INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(), debugLogfile.size() + 1);

    callInjected("INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts, sizeof(CaptureOptions));

    callInjected("INTERNAL_GetTargetControlIdent", &result.second, sizeof(result.second));

    if(!env.empty())
    {
      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;
        EnvMod mod = e.mod;
        EnvSep sep = e.sep;

        if(name == "")
          break;

        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModName", (void *)name.c_str(),
                           name.size() + 1, primaryThread);
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModValue", (void *)value.c_str(),
                           value.size() + 1, primaryThread);
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvSep", &sep, sizeof(sep), primaryThread);
        InjectFunctionCall(hProcess, loc, "INTERNAL_EnvMod", &mod, sizeof(mod), primaryThread);
      }

      // parameter is unused
      void *dummy = NULL;
      InjectFunctionCall(hProcess, loc, "INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy),
                         primaryThread);
    }
  }

  if(waitForExit)
    WaitForSingleObject(hProcess, INFINITE);

  CloseHandle(hProcess);

  return result;
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  ConfigureSR44LaunchDiagnostics();

  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons RenderDoc does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Failed to launch process.");
    return {result, 0};
  }

  const bool sr44Diagnostics = IsSR44LaunchDiagnosticsEnabled();
  g_SR44DiagnosticTargetPid = pi.dwProcessId;
  if(sr44Diagnostics)
    LogSR44LaunchStage("launch process handle", StringFormat::Fmt("pid=%lu tid=%lu", (unsigned long)pi.dwProcessId,
                                                                    (unsigned long)pi.dwThreadId)
                           .c_str());

  rdcpair<RDResult, uint32_t> ret =
      InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false, pi.hThread);

  if(sr44Diagnostics)
    LogSR44LaunchStage("InjectIntoProcess",
                       ret.first.message.empty()
                           ? (ret.first == ResultCode::Succeeded ? "succeeded" : "failed")
                           : ret.first.message.c_str());

  if(!sr44Diagnostics)
    CloseHandle(pi.hProcess);

  if(sr44Diagnostics)
  {
    DWORD beforeExitCode = 0;
    BOOL beforeExitOk = GetExitCodeProcess(pi.hProcess, &beforeExitCode);
    LogSR44LaunchStage("pre-resume target state",
                       beforeExitOk
                           ? StringFormat::Fmt("code=%lu state=%s", (unsigned long)beforeExitCode,
                                               beforeExitCode == STILL_ACTIVE ? "STILL_ACTIVE" : "exited")
                                 .c_str()
                           : StringFormat::Fmt("GetExitCodeProcess failed error=%lu",
                                               (unsigned long)GetLastError())
                                 .c_str());
  }

  SetLastError(ERROR_SUCCESS);
  DWORD resumeFirst = ResumeThread(pi.hThread);
  DWORD resumeFirstError = GetLastError();
  if(sr44Diagnostics)
    LogSR44LaunchStage("ResumeThread #1",
                       StringFormat::Fmt("return=%lu lastError=%lu", (unsigned long)resumeFirst,
                                          (unsigned long)resumeFirstError)
                           .c_str());

  SetLastError(ERROR_SUCCESS);
  DWORD resumeSecond = ResumeThread(pi.hThread);
  DWORD resumeSecondError = GetLastError();
  if(sr44Diagnostics)
    LogSR44LaunchStage("ResumeThread #2",
                       StringFormat::Fmt("return=%lu lastError=%lu", (unsigned long)resumeSecond,
                                          (unsigned long)resumeSecondError)
                           .c_str());

  if(sr44Diagnostics)
  {
    const uint64_t observeStart = GetTickCount64();
    const DWORD observeWindowMs = GetSR44LaunchObservationMilliseconds();
    DWORD waitResult = WaitForSingleObject(pi.hProcess, observeWindowMs);
    DWORD exitCode = 0;
    BOOL exitCodeOk = GetExitCodeProcess(pi.hProcess, &exitCode);

    LogSR44LaunchStage(
        "post-resume observation",
        waitResult == WAIT_OBJECT_0
            ? StringFormat::Fmt("exited after %llu ms code=%lu", (unsigned long long)(GetTickCount64() - observeStart),
                                (unsigned long)(exitCodeOk ? exitCode : 0))
                  .c_str()
            : waitResult == WAIT_TIMEOUT
                  ? StringFormat::Fmt("still running after %lu ms state=%s", (unsigned long)observeWindowMs,
                                      exitCodeOk && exitCode == STILL_ACTIVE ? "STILL_ACTIVE" : "unknown")
                        .c_str()
                  : StringFormat::Fmt("wait failed result=%lu error=%lu", (unsigned long)waitResult,
                                      (unsigned long)GetLastError())
                        .c_str());

    if(waitResult == WAIT_TIMEOUT)
    {
      DWORD laterExitCode = 0;
      BOOL laterExitCodeOk = GetExitCodeProcess(pi.hProcess, &laterExitCode);
      LogSR44LaunchStage(
          "observation timeout state",
          laterExitCodeOk
              ? StringFormat::Fmt("code=%lu state=%s window_ms=%lu", (unsigned long)laterExitCode,
                                  laterExitCode == STILL_ACTIVE ? "STILL_ACTIVE" : "exited",
                                  (unsigned long)observeWindowMs)
                    .c_str()
              : StringFormat::Fmt("GetExitCodeProcess failed error=%lu window_ms=%lu",
                                  (unsigned long)GetLastError(), (unsigned long)observeWindowMs)
                    .c_str());
    }

    if(waitResult == WAIT_OBJECT_0 && exitCodeOk)
      LogSR44LaunchJson("target_exit", "target process exited during observation", pi.dwProcessId,
                        exitCode);
  }

  if(sr44Diagnostics)
    CloseHandle(pi.hProcess);

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
      "Check that RenderTest is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "RenderTest is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where RenderDoc is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "RenderTest is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where RenderDoc is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"RenderDoc_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native rendertestcmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\rendertestcmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just rendertestshim64.dll
  shimpathNative = renderdocPath + "\\rendertestshim64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestcmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestshim32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestcmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\rendertestcmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\rendertestshim32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
