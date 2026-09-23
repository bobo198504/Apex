#ifndef APEX_LOADER_H
#define APEX_LOADER_H

// ---------------------------------------------------------------------------
// LOADING FEATURES out of Plugins\.
//
// THE SECURITY POSTURE, STATED PLAINLY: this loads whatever DLL it finds. There is no signature, no
// allowlist, no sandbox. That is the same posture as any plugin host (REAPER, Winamp, Photoshop) and it
// is stated here rather than implied, because the alternative -- "it only loads ours" -- would be a lie.
// A user who drops a stranger's DLL into Plugins\ is running that stranger's code.
//
// WHAT THE HOST DOES GUARANTEE: a feature that is broken, stale, or built for another ABI fails in ONE
// place, is named in the log, and does not take the host with it. Three things make that true:
//
//   1. BOTH version AND size are compared BEFORE any field of the struct is read. A DLL built against a
//      struct with fewer members would otherwise have the host reading past its end.
//   2. The entry point is looked up by NAME, so a DLL without it (or with it exported under the wrong
//      decoration) is rejected with a reason rather than a crash.
//   3. A feature that fails init() is unloaded, and the host keeps running -- the rest of the features
//      and the host's own hook are unaffected.
//
// The loaded handle is kept, not just the function pointers: the DLL must stay mapped for as long as its
// code can be called, and it has to be unloaded explicitly at shutdown (Windows does not do it for us in
// a way that lets the feature run its own cleanup).
// ---------------------------------------------------------------------------

#include "abi.h"

namespace apex {

static const int kMaxFeatures = 16;

struct LoadedFeature
{
  const ApexFeature *api = nullptr; // points INTO the DLL
  void *handle = nullptr;           // HMODULE, kept mapped for the feature's lifetime
  char dir[512] = {0};              // Plugins\<id>\, so its own files have a home
  bool ok = false;                  // loaded AND init() succeeded
  char why[160] = {0};              // why not, for the log and the panel
};

class Loader
{
public:
  // Scan Plugins\, load each folder's DLL, and call init() on each accepted one.
  //
  // `host` is handed to every feature. Returns how many features are live.
  int LoadAll(const ApexHost *host);

  // Call shutdown() on each live feature, in REVERSE load order (a feature loaded later may have been
  // the reason an earlier one did something), then unload them.
  void UnloadAll();

  int Count() const { return count_; }
  const LoadedFeature &At(int i) const { return items_[i]; }

  // One line per accepted feature, whether or not it is currently enabled -- the panel lists all of
  // them, and a user needs to see a feature that failed to load rather than wonder where it went.
  int CountAll() const { return seen_; }
  const LoadedFeature &Seen(int i) const { return seenItems_[i]; }

private:
  LoadedFeature items_[kMaxFeatures];
  int count_ = 0;
  LoadedFeature seenItems_[kMaxFeatures + 8];
  int seen_ = 0;

  void Record(const LoadedFeature &f);
};

} // namespace apex

#endif // APEX_LOADER_H
