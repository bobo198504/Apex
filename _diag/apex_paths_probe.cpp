// WHERE APEX's FILES LAND, checked against the code that actually decides it.
//
// WHAT THIS REPLACES, and why the replacement is not just a rename: the old gate (check_app_configfile.sh)
// tested `config_win.cpp`, the standalone app's "settings beside the exe" rule. That file is DEAD CODE in
// Apex -- the host keeps its settings through apex/paths_win.cpp and the feature keeps its own through the
// host's featureDir() -- so testing it would have meant a green gate over code nothing runs, which is the
// worst kind of test there is.
//
// The REQUIREMENT did not change, so it is checked against the real thing instead:
//
//   * the host's settings are `apex.ini` BESIDE apex.exe, never in %APPDATA% or the registry;
//   * a feature's DLL and its settings are both in `Plugins\<id>\`, so moving that folder moves the feature;
//   * a path is built from the MODULE PATH, not the current directory -- a shortcut or a terminal can set a
//     working directory somewhere else entirely, and settings would then be written there.
//
// The last one is the reason this cannot be judged by reading the source: it is a property of what the OS
// resolves at run time. So the probe is run from a DIFFERENT working directory, and the paths must still
// come back beside the probe's own exe.
//
// g++ -std=c++17 -O2 -Iapex _diag/apex_paths_probe.cpp apex/paths_win.cpp -o <dir>/paths_probe
#include "paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void Ok(const char *what, bool pass, const char *detail = nullptr)
{
  printf("  %-64s %s%s%s\n", what, pass ? "ok" : "FAIL", detail ? "  " : "", detail ? detail : "");
  if (!pass)
    ++failures;
}

// Case-insensitive prefix test: Windows paths are case-insensitive, and the module path's case comes from
// the OS rather than from anything we control.
static bool StartsWithI(const char *s, const char *prefix)
{
  return _strnicmp(s, prefix, (int)strlen(prefix)) == 0;
}

int main(int argc, char **argv)
{
  const char *argv0 = (argc > 0 && argv[0]) ? argv[0] : "";
  char dir[512] = {0};
  apex::DirOf(argv0, dir, (int)sizeof(dir));

  printf("probe exe : %s\n", argv0);
  printf("probe dir : %s\n\n", dir[0] ? dir : "(none)");

  char hostCfg[512] = {0};
  char plugins[512] = {0};
  char featDir[512] = {0};
  char featDll[512] = {0};
  char featCfg[512] = {0};

  Ok("the module directory resolves", dir[0] != 0);
  Ok("the host's settings path resolves", apex::HostConfigPath(hostCfg, (int)sizeof(hostCfg)));
  Ok("the plugins directory resolves", apex::PluginsDir(plugins, (int)sizeof(plugins)));
  Ok("a feature directory resolves", apex::FeatureDir("SmoothWheel", featDir, (int)sizeof(featDir)));
  Ok("a feature dll path resolves", apex::FeatureDllPath("SmoothWheel", featDll, (int)sizeof(featDll)));
  Ok("a feature settings path resolves",
     apex::FeatureConfigPath("SmoothWheel", featCfg, (int)sizeof(featCfg)));

  printf("\nresolved:\n  host    %s\n  plugins %s\n  feature %s\n  dll     %s\n  config  %s\n\n",
         hostCfg, plugins, featDir, featDll, featCfg);

  // ---- THE LAYOUT THE USER ASKED FOR ----
  Ok("apex.ini is BESIDE the exe", StartsWithI(hostCfg, dir));
  Ok("  and is named apex.ini", strstr(hostCfg, "apex.ini") != nullptr);
  Ok("Plugins is a folder beside the exe", StartsWithI(plugins, dir));
  Ok("  with a trailing separator (so joining never needs one added)",
     plugins[0] && plugins[strlen(plugins) - 1] == '\\');
  Ok("the feature's folder is under Plugins", strstr(featDir, "Plugins\\SmoothWheel") != nullptr);
  Ok("  and also ends with a separator", featDir[0] && featDir[strlen(featDir) - 1] == '\\');
  Ok("the feature's dll is inside its own folder",
     StartsWithI(featDll, featDir) && strstr(featDll, "SmoothWheel.dll") != nullptr);
  Ok("the feature's settings sit beside its dll, not in a shared file",
     StartsWithI(featCfg, featDir) && strstr(featCfg, "SmoothWheel.ini") != nullptr);

  // ---- NOT ANYWHERE ELSE ----
  //
  // The portable promise, asserted rather than assumed. A path that quietly fell back to a profile directory
  // would still WORK, which is exactly why it needs a test: nothing would look wrong until someone copied
  // the folder to another machine and found their settings gone.
  const char *paths[] = {hostCfg, plugins, featDir, featDll, featCfg};
  bool clean = true;
  for (int i = 0; i < 5; ++i)
    if (strstr(paths[i], "AppData") || strstr(paths[i], "appdata") || strstr(paths[i], "APPDATA"))
      clean = false;
  Ok("nothing lands in %APPDATA%", clean);

  clean = true;
  for (int i = 0; i < 5; ++i)
    if (strstr(paths[i], "REAPER") || strstr(paths[i], "reaper.ini"))
      clean = false;
  Ok("nothing lands in a REAPER folder or in reaper.ini", clean);

  // ---- A BAD ID MUST NOT PRODUCE A PATH ----
  //
  // An empty or missing id would otherwise build "\" or "Plugins\\" and a feature could write its settings
  // into the Plugins root instead of its own folder.
  char bad[512] = {0};
  Ok("an empty feature id is refused", !apex::FeatureDir("", bad, (int)sizeof(bad)));
  Ok("a null feature id is refused", !apex::FeatureDir(nullptr, bad, (int)sizeof(bad)));
  Ok("an empty id leaves the buffer empty", bad[0] == 0);

  // ---- A SHORT BUFFER MUST FAIL, NOT TRUNCATE ----
  //
  // A truncated path is worse than a refusal: it names a DIFFERENT file, and the caller has no way to tell.
  char tiny[8] = {0};
  Ok("a buffer too small to hold the path is refused", !apex::HostConfigPath(tiny, (int)sizeof(tiny)));

  printf("\n");
  if (failures == 0)
    printf("OK: apex.ini and every feature's files land beside the exe, and nowhere else\n");
  else
    printf("FAILED: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
