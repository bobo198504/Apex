#ifndef APEX_APPPROXY_PROCESS_TREE_H
#define APEX_APPPROXY_PROCESS_TREE_H

#include "proxy_common.h"

namespace appproxy {

struct ProcessNode
{
  unsigned long pid;
  unsigned long parent;
  char exe[kMaxPattern];
};

// A selected application is a PROCESS TREE, not merely one image name. Chromium and WebView2 routinely put
// DNS/network work in children with other executable names (msedgewebview2.exe, a utility process, ...), so a
// rule such as `chatgpt*` would otherwise attach to the visible process and miss the one doing the I/O.
inline bool MayInheritAppRule(const char *exe)
{
  // Only browser network helpers may inherit from a visible application. Inheriting through EVERY child would
  // turn `chatgpt*` into "the whole desktop": packaged launchers often start shells, editors and terminals,
  // and those are not part of the application's own networking.
  return EqNoCase(exe, "msedgewebview2.exe");
}

inline int RuleForSelectedTree(const Config &cfg, const ProcessNode *nodes, int count, int index)
{
  if (!nodes || count <= 0 || index < 0 || index >= count)
    return -1;
  int current = index;
  for (int depth = 0; depth < 20; ++depth)
  {
    const int rule = RuleForExe(&cfg, nodes[current].exe);
    if (rule >= 0)
      return rule;
    if (!MayInheritAppRule(nodes[current].exe))
      return -1;
    const unsigned long parentPid = nodes[current].parent;
    if (!parentPid || parentPid == nodes[current].pid)
      return -1;
    int parent = -1;
    for (int i = 0; i < count; ++i)
      if (nodes[i].pid == parentPid)
      {
        parent = i;
        break;
      }
    if (parent < 0)
      return -1;
    current = parent;
  }
  return -1;
}

inline bool InSelectedTree(const Config &cfg, const ProcessNode *nodes, int count, int index)
{
  return RuleForSelectedTree(cfg, nodes, count, index) >= 0;
}

} // namespace appproxy

#endif // APEX_APPPROXY_PROCESS_TREE_H
