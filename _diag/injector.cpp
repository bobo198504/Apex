// ⚠️ A MINIMAL INJECTOR, AND ITS ONLY JOB IS TO ANSWER "CAN THIS BE DELIVERED AT ALL?"
//
// The payload's EFFECT is already settled (_diag/menudark.cpp, with pictures): ForceDark turns an old program's
// popup menu dark when the program loads it itself. What is NOT settled is whether a separate process can put
// that payload into another program -- a different question with different failure modes (privilege level,
// protected processes, antivirus, and the plain fact that CreateRemoteThread is the single most recognisable
// injection signature there is).
//
// ⚠️ TWO ROUTES, BECAUSE THE FIRST ONE WAS MEASURED TO BE UNRELIABLE HERE. CreateRemoteThread worked on the
// first injection and then began failing with ERROR_ACCESS_DENIED on the second (stably, twice -- see
// docs/rules/features.md). Whatever is refusing it, the lesson is the one that was already true before the
// measurement: CreateRemoteThread is the most recognisable injection there is.
//
//   injector.exe <pid> <absolute dll>            -> CreateRemoteThread(LoadLibraryA)
//   injector.exe <pid> <absolute dll> --hook     -> SetWindowsHookEx(WH_GETMESSAGE) on one of its threads
//
// ⚠️ THE HOOK ROUTE SPECIFIES A THREAD, NEVER ZERO. dwThreadId = 0 means "every GUI process on the machine",
// which would load this payload into the user's applications -- exactly the accident this project has already
// had once with a process filter that was too loose. One thread, in one process, chosen by pid.
// ⚠️ windows.h FIRST: tlhelp32.h uses HANDLE/DWORD/WINBOOL and does not include windows.h itself, so the other
// order produces a screenful of "does not name a type" errors that have nothing to do with the code.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The thread the hook is installed on: any thread of the target works, because what we want is for the SYSTEM
// to load our DLL into that process at all.
// ⚠️ A WINDOW'S THREAD IS BETTER THAN ANY THREAD, FOR TWO SEPARATE REASONS.
//
// First, correctness: WH_GETMESSAGE is delivered along with messages, so a thread with no message queue would
// never receive the hook at all -- and the system only maps the DLL in when the hook actually fires. Picking a
// window's thread guarantees there is a queue to fire from.
//
// Second, and this is the one that was measured: the thread-snapshot route DOES NOT LIST EVERY PROCESS.
// Against this project's own test program the snapshot listed 887 threads and found 4 belonging to it; against
// a plain System32 program (winver.exe) it listed 884 threads and found NONE of its. That is not a permission
// error and not a UWP sandbox -- it is simply not a reliable way to reach a process, and the failure looked
// exactly like "the target has no threads" until the three outcomes were printed separately.
// ⚠️ A STATIC CALLBACK RATHER THAN A LAMBDA, AND IT IS A 32-BIT CONSTRAINT RATHER THAN A STYLE CHOICE. On x64
// there is one calling convention, so a lambda converts to WNDENUMPROC without complaint -- and it did, for the
// whole life of the 64-bit injector. On x86 the callback is __stdcall and a default (__cdecl) lambda does not
// convert at all, which is exactly how this failed the moment a 32-bit compiler was pointed at it.
struct FindWin
{
  DWORD pid;
  HWND hwnd;
};

static BOOL CALLBACK FindWinProc(HWND h, LPARAM p)
{
  FindWin *f = (FindWin *)p;
  DWORD owner = 0;
  GetWindowThreadProcessId(h, &owner);
  if (owner == f->pid)
  {
    f->hwnd = h;
    return FALSE; // stop at the first one
  }
  return TRUE;
}

static DWORD FirstThreadOf(DWORD pid, HWND *outWindow)
{
  *outWindow = nullptr;
  // Route 1: a window of that process. Preferred, for both reasons above.
  {
    FindWin f;
    f.pid = pid;
    f.hwnd = nullptr;
    EnumWindows(FindWinProc, (LPARAM)&f);
    if (f.hwnd)
    {
      const DWORD tid = GetWindowThreadProcessId(f.hwnd, nullptr);
      printf("injector: pid %lu owns window %p; its thread is %lu\n", (unsigned long)pid, (void *)f.hwnd,
             (unsigned long)tid);
      *outWindow = f.hwnd;
      return tid;
    }
    printf("injector: pid %lu owns no top-level window -- falling back to a thread snapshot\n",
           (unsigned long)pid);
  }

  // Route 2: the snapshot, which is kept only as a fallback and reports each outcome separately.
  DWORD tid = 0;
  int seen = 0, matched = 0;
  SetLastError(0);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
  {
    printf("injector: CreateToolhelp32Snapshot(SNAPTHREAD) FAILED (GetLastError=%lu)\n",
           (unsigned long)GetLastError());
    return 0;
  }
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  SetLastError(0);
  if (Thread32First(snap, &te))
  {
    do
    {
      ++seen;
      if (te.th32OwnerProcessID == pid)
      {
        ++matched;
        if (!tid)
          tid = te.th32ThreadID;
      }
    } while (Thread32Next(snap, &te));
  }
  else
  {
    printf("injector: Thread32First FAILED (GetLastError=%lu)\n", (unsigned long)GetLastError());
  }
  CloseHandle(snap);
  printf("injector: the snapshot listed %d thread(s); %d of them belong to pid %lu\n", seen, matched,
         (unsigned long)pid);
  return tid;
}

static int RouteHook(DWORD pid, const char *dll)
{
  HWND target = nullptr;
  const DWORD tid = FirstThreadOf(pid, &target);
  if (!tid)
  {
    printf("injector: no thread could be reached in pid %lu\n", (unsigned long)pid);
    return 1;
  }
  // ⚠️ THE DLL IS LOADED LOCALLY FIRST, and that is how SetWindowsHookEx is meant to be used: the module
  // handle identifies which DLL the system should map into the target. It is not loaded into the target by us.
  HMODULE mine = LoadLibraryA(dll);
  if (!mine)
  {
    printf("injector: local LoadLibrary(\"%s\") FAILED (GetLastError=%lu)\n", dll,
           (unsigned long)GetLastError());
    return 1;
  }
  // ⚠️ TWO NAMES, BECAUSE x86 DECORATES __stdcall EXPORTS AND x64 DOES NOT. On x64 the export is plain
  // "ApexHookProc"; on x86 the same source produces "ApexHookProc@12", where 12 is the byte count of the
  // arguments (three four-byte parameters). This is not a bug in either build -- it is what the two
  // architectures do -- and looking up both is cheaper and clearer than a .def file that would have to be kept
  // in step with the signature.
  HOOKPROC proc = (HOOKPROC)GetProcAddress(mine, "ApexHookProc");
  if (!proc)
    proc = (HOOKPROC)GetProcAddress(mine, "ApexHookProc@12");
  if (!proc)
  {
    printf("injector: the dll exports neither ApexHookProc nor ApexHookProc@12\n");
    return 1;
  }

  SetLastError(0);
  HHOOK hk = SetWindowsHookExA(WH_GETMESSAGE, proc, mine, tid);
  printf("injector: SetWindowsHookEx(WH_GETMESSAGE, tid=%lu) -> %p (GetLastError=%lu)\n", (unsigned long)tid,
         (void *)hk, (unsigned long)GetLastError());
  if (!hk)
    return 1;

  // ⚠️⚠️ THE SYSTEM LOADS THE DLL WHEN THE THREAD NEXT TAKES A MESSAGE, SO AN IDLE TARGET NEVER LOADS IT.
  // That is the whole reason this nudge exists: SetWindowsHookEx returned success against winver.exe and
  // charmap.exe and the payload still never arrived, because both are static dialogs sitting blocked in
  // GetMessage with nothing to hand over. WM_NULL is a defined no-op message -- it cannot change anything the
  // target is doing -- so posting it only gives the queue something to deliver, which is what fires the hook.
  // (It is harmless to the target by construction, and the target here is a program this command started.)
  printf("injector: waiting for the target to pick the hook up...\n");
  bool arrived = false;
  const char *leaf = strrchr(dll, '\\');
  leaf = leaf ? leaf + 1 : dll;
  for (int i = 0; i < 100 && !arrived; ++i)
  {
    if (target && (i % 3) == 0)
    {
      // ⚠️ THE RETURN VALUE IS PRINTED, AND ITS ABSENCE WAS A REAL BLIND SPOT. "The nudge was refused" and "the
      // nudge arrived and the hook still did not load" are different facts with different consequences, and the
      // first version could not tell them apart -- it ignored this result and then reported only "did NOT
      // arrive", which reads as a fact about the mechanism when it may be a fact about the delivery.
      SetLastError(0);
      const BOOL posted = PostMessageA(target, WM_NULL, 0, 0);
      if (i == 0)
        printf("injector: PostMessage(WM_NULL) to the target's window -> %d (GetLastError=%lu)\n", (int)posted,
               (unsigned long)GetLastError());
    }
    Sleep(100);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
      continue;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me))
    {
      do
      {
        if (_stricmp(me.szModule, leaf) == 0)
        {
          arrived = true;
          break;
        }
      } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
  }
  printf("injector: the payload %s in the target (checked by reading its module list)\n",
         arrived ? "IS LOADED" : "did NOT arrive");
  // The hook object belongs to this process; letting it exit drops the hook. The DLL stays mapped in the target
  // and whatever it did in DllMain is done, which is all this payload needs.
  return arrived ? 0 : 1;
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("usage: injector.exe <pid> <absolute path to dll> [--hook]\n");
    return 2;
  }
  const DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
  const char *dll = argv[2];
  if (!(dll[0] && dll[1] == ':'))
  {
    printf("injector: the dll path must be absolute (%s)\n", dll);
    return 2;
  }
  const bool useHook = (argc >= 4 && strcmp(argv[3], "--hook") == 0);
  const bool useProbe = (argc >= 4 && strcmp(argv[3], "--probe") == 0);
  printf("injector: pid=%lu dll=%s route=%s\n", (unsigned long)pid, dll,
         useProbe ? "probe-only" : (useHook ? "hook" : "remote-thread"));

  // ⚠️ A LOOK BEFORE A LEAP, AND IT ANSWERS A QUESTION BIGGER THAN THIS RUN. "Can a thread in that process be
  // reached at all" is the difference between the environment refusing cross-process work and the target being
  // genuinely unreachable -- and those two have opposite meanings for whether this route is viable. Nothing is
  // written, mapped or hooked in this mode.
  if (useProbe)
  {
    HWND w = nullptr;
    const DWORD tid = FirstThreadOf(pid, &w);
    if (tid)
    {
      printf("injector: probe only -- thread %lu (of window %p) is reachable; injection looks possible\n",
             (unsigned long)tid, (void *)w);
      return 0;
    }
    printf("injector: probe only -- NO thread could be reached; injection cannot proceed here\n");
    return 1;
  }

  if (useHook)
    return RouteHook(pid, dll);

  HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
  if (!proc)
  {
    printf("injector: OpenProcess FAILED (GetLastError=%lu)\n", (unsigned long)GetLastError());
    return 1;
  }

  const SIZE_T len = strlen(dll) + 1;
  void *remote = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote)
  {
    printf("injector: VirtualAllocEx FAILED (GetLastError=%lu)\n", (unsigned long)GetLastError());
    CloseHandle(proc);
    return 1;
  }
  SIZE_T written = 0;
  if (!WriteProcessMemory(proc, remote, dll, len, &written) || written != len)
  {
    printf("injector: WriteProcessMemory FAILED (GetLastError=%lu)\n", (unsigned long)GetLastError());
    CloseHandle(proc);
    return 1;
  }

  // LoadLibraryA sits at the same address in every process of the same bitness -- documented, and the whole
  // reason this classic technique works.
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
  printf("injector: LoadLibraryA is at %p here\n", (void *)loadLib);

  HANDLE th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, nullptr);
  if (!th)
  {
    printf("injector: CreateRemoteThread FAILED (GetLastError=%lu)\n", (unsigned long)GetLastError());
    CloseHandle(proc);
    return 1;
  }
  const DWORD w = WaitForSingleObject(th, 10000);
  DWORD code = 0;
  GetExitCodeThread(th, &code);
  printf("injector: thread finished (wait=%lu), LoadLibraryA returned 0x%08lX\n", (unsigned long)w,
         (unsigned long)code);
  if (code == 0)
    printf("injector: the payload did NOT load in the target\n");

  CloseHandle(th);
  // The remote buffer is left allocated on purpose: freeing it would race with a thread that has just started
  // executing the code it points at. The target is this project's own short-lived test program.
  CloseHandle(proc);
  return code ? 0 : 1;
}
