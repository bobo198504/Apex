// ---------------------------------------------------------------------------
// THE WINDOWS SIDE OF FEATURE LOADING. Only this file touches the OS loader.
//
// Everything that decides WHETHER a feature is acceptable is here, in one place, because this is the
// boundary where a wrong DLL in Plugins\ must not be able to hurt the host (see loader.h for the
// three guarantees this implements).
// ---------------------------------------------------------------------------

#include "loader.h"
#include "host.h"
#include "paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace apex {

// The exported symbol. C linkage, so the name is not decorated and can be found by a plain string.
typedef const ApexFeature *(__cdecl *EntryFn)(void);
static const char *kEntryName = "ApexFeatureEntry";

void Loader::Record(const LoadedFeature &f)
{
  if (seen_ < (int)(sizeof(seenItems_) / sizeof(seenItems_[0])))
    seenItems_[seen_++] = f;
}

int Loader::LoadAll(const ApexHost *host)
{
  count_ = 0;
  seen_ = 0;

  char plugins[512] = {0};
  if (!PluginsDir(plugins, (int)sizeof(plugins)))
    return 0;

  char pattern[560] = {0};
  if (_snprintf(pattern, sizeof(pattern), "%s*", plugins) <= 0)
    return 0;

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return 0;

  do
  {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      continue;
    if (fd.cFileName[0] == '.')
      continue;
    if (count_ >= kMaxFeatures)
      break;

    LoadedFeature f;
    if (!FeatureDir(fd.cFileName, f.dir, (int)sizeof(f.dir)))
      continue;

    // THE DLL IN THE FOLDER. The convention is <id>.dll, but any single DLL is accepted: a feature
    // whose author named it differently should load rather than need a rename, and the id that matters
    // is the one the feature itself reports (it is what the settings file and the skip rules use).
    char dll[560] = {0};
    if (!FeatureDllPath(fd.cFileName, dll, (int)sizeof(dll)))
      continue;

    // If the conventional name is not there, take the first .dll in the folder instead.
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES)
    {
      char any[560] = {0};
      if (_snprintf(any, sizeof(any), "%s*.dll", f.dir) <= 0)
        continue;
      WIN32_FIND_DATAA df;
      HANDLE dh = FindFirstFileA(any, &df);
      if (dh == INVALID_HANDLE_VALUE)
      {
        _snprintf(f.why, sizeof(f.why), "no dll in this folder");
        Record(f);
        continue;
      }
      JoinPath(f.dir, df.cFileName, dll, (int)sizeof(dll));
      FindClose(dh);
    }

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: the feature's own folder is searched for anything it depends
    // on, so a feature that ships a helper library next to itself works without touching the host's
    // search path. Without this, LoadLibrary would search the app directory and then PATH.
    HMODULE mod = LoadLibraryExA(dll, nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod)
    {
      mod = LoadLibraryA(dll); // older systems without the flag support
      if (!mod)
      {
        _snprintf(f.why, sizeof(f.why), "LoadLibrary failed (error %lu)", GetLastError());
        Record(f);
        continue;
      }
    }

    EntryFn entry = (EntryFn)(void *)GetProcAddress(mod, kEntryName);
    if (!entry)
    {
      _snprintf(f.why, sizeof(f.why), "no %s export", kEntryName);
      FreeLibrary(mod);
      Record(f);
      continue;
    }

    const ApexFeature *api = entry();
    if (!api)
    {
      _snprintf(f.why, sizeof(f.why), "%s returned null", kEntryName);
      FreeLibrary(mod);
      Record(f);
      continue;
    }

    // ---- THE TWO CHECKS THAT MUST COME FIRST ----
    //
    // Called out separately because they are the difference between "a stale DLL is rejected" and "a
    // stale DLL has its struct read past the end". NOTHING in `api` may be touched before both pass,
    // including its id for a log line -- that is why the message names the folder instead.
    if (api->abiVersion != APEX_ABI_VERSION)
    {
      _snprintf(f.why, sizeof(f.why), "ABI %u, this host speaks %u -- rebuild the feature",
                api->abiVersion, APEX_ABI_VERSION);
      FreeLibrary(mod);
      Record(f);
      continue;
    }
    if (api->structSize < sizeof(ApexFeature))
    {
      _snprintf(f.why, sizeof(f.why), "struct is %u bytes, the host expects %u -- too old",
                api->structSize, (unsigned)sizeof(ApexFeature));
      FreeLibrary(mod);
      Record(f);
      continue;
    }

    f.api = api;
    f.handle = mod;

    if (host && api->init)
    {
      // The feature's folder must be answerable BEFORE init() runs: init is where a feature reads its own
      // settings, and that is the first thing it asks. Getting this wrong is not a subtle bug -- the
      // feature is told "(unknown)" and cannot find its settings at all.
      host::SetCurrentFeature(api->id);
      const int rc = api->init(host);
      host::SetCurrentFeature(nullptr);
      if (rc != 0)
      {
        _snprintf(f.why, sizeof(f.why), "init() refused (returned %d)", rc);
        // ⚠️ AND IT IS SAID IN THE LOG, not only in the panel's reason field. The ABI promises "the host
        // unloads it and logs it"; only the first half was true, and the silence is expensive: a refused
        // feature simply never appears in the "feature: ... enabled" list below, which looks exactly like a
        // feature that was never found on disk. (This is how a whole afternoon went missing -- the crash it
        // caused had no line anywhere pointing at the cause.)
        //
        // (`host::SettingsLog` is the host's own log line, declared in host.h; the name comes from its first
        // caller, the settings protocol, and it is nothing more than LogLine with a buffer.)
        host::SettingsLog(
            "feature %s: init() refused (returned %d) -- the DLL is unloaded and the feature is not running",
            api->id ? api->id : "(no id)", rc);
        f.api = nullptr;
        FreeLibrary(mod);
        f.handle = nullptr;
        Record(f);
        continue;
      }
    }

    f.ok = true;
    // ⚠️ THE SLOT IS FILLED, THEN THE COUNT IS BUMPED -- never `items_[count_++] = f`.
    //
    // That form stores the count and the entry in an order the language does not fix, and a FEATURE'S OWN
    // THREAD reads this list while it is being built: `featureEnabled` may be asked from any thread (see
    // abi.h), KeepAwake's worker asks once a second from the moment init() returns, and load order decides
    // whether that lands inside this loop. A reader that sees the count first reads a half-written entry --
    // `api` still the previous slot's value, or garbage -- and `f.api->id` on a wild pointer takes the host
    // down with it. It did: the host died within a second of startup, and stayed up for ever under a debugger.
    items_[count_] = f;
    MemoryBarrier(); // the entry is complete before anybody is told that it exists
    ++count_;
    Record(f);
  } while (FindNextFileA(h, &fd));

  FindClose(h);
  return count_;
}

void Loader::UnloadAll()
{
  // REVERSE ORDER: a feature loaded later may be the reason an earlier one started doing something, so
  // it is stopped first.
  for (int i = count_ - 1; i >= 0; --i)
  {
    if (items_[i].api && items_[i].api->shutdown)
      items_[i].api->shutdown();
    items_[i].api = nullptr;
    if (items_[i].handle)
    {
      FreeLibrary((HMODULE)items_[i].handle);
      items_[i].handle = nullptr;
    }
  }
  count_ = 0;
}

} // namespace apex
