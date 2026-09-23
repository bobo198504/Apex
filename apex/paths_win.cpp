// ---------------------------------------------------------------------------
// THE WINDOWS SIDE OF THE HOST'S PATHS. The one place the layout touches the OS.
//
// The folder is found from the MODULE PATH, not the current directory: a shortcut, a launch from a
// terminal, or a file dialog can all set a working directory that is somewhere else entirely, and the
// settings would then be written there. That distinction is the whole reason this is a separate file
// from paths.h -- the rules above it are pure and testable, and only this needs Windows.
// ---------------------------------------------------------------------------

#include "paths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace apex {

bool ApexModuleDir(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return false;
  out[0] = 0;
  char path[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return false;
  DirOf(path, out, outSize);
  return out[0] != 0; // DirOf leaves it empty when there is no separator at all
}

} // namespace apex
