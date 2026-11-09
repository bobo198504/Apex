#ifndef APEX_APPPROXY_SERVICE_STATUS_H
#define APEX_APPPROXY_SERVICE_STATUS_H

// The service writes a tiny key=value file every second. The settings page reads it to answer the question
// the user actually asks: "is this connected, has it forwarded anything, and if not, why?" Keeping the parser
// here means the feature and its probe read exactly the same grammar; the page never has to interpret raw
// status lines.
#include "proxy_common.h"

namespace appproxy {

struct ServiceStatus
{
  char state[32];
  char detail[160];
  char lastResult[16];
  char lastApp[64];
  char lastTarget[96];
  char lastError[160];
  int relayPort;
  int packets;
  int matches;
  int rewrites;
  int relayOk;
  int relayFail;
  int ipv6Blocked;
  int quicBlocked;
  int any;
};

inline void Defaults(ServiceStatus *s)
{
  if (!s)
    return;
  char *p = (char *)s;
  for (unsigned i = 0; i < sizeof(ServiceStatus); ++i)
    p[i] = 0;
}

inline void ParseServiceStatus(const char *text, int textLen, ServiceStatus *out)
{
  if (!out)
    return;
  Defaults(out);
  if (!text || textLen <= 0)
    return;
  out->any = 1;

  int pos = 0;
  while (pos < textLen)
  {
    int end = pos;
    while (end < textLen && text[end] != '\n')
      ++end;
    char line[512];
    int n = end - pos;
    if (n > (int)sizeof(line) - 1)
      n = (int)sizeof(line) - 1;
    CopyN(line, (int)sizeof(line), text + pos, n);
    pos = end + 1;

    char *s = Trim(line);
    if (!s[0] || s[0] == '#')
      continue;
    char *eq = s;
    while (*eq && *eq != '=')
      ++eq;
    if (*eq != '=')
      continue;
    *eq++ = 0;
    s = Trim(s);
    char *v = Trim(eq);

    if (EqNoCase(s, "state"))
      Copy(out->state, (int)sizeof(out->state), v);
    else if (EqNoCase(s, "detail"))
      Copy(out->detail, (int)sizeof(out->detail), v);
    else if (EqNoCase(s, "relay_port"))
      ParseInt(v, &out->relayPort);
    else if (EqNoCase(s, "packets"))
      ParseInt(v, &out->packets);
    else if (EqNoCase(s, "matches"))
      ParseInt(v, &out->matches);
    else if (EqNoCase(s, "rewrites"))
      ParseInt(v, &out->rewrites);
    else if (EqNoCase(s, "relay_ok"))
      ParseInt(v, &out->relayOk);
    else if (EqNoCase(s, "relay_fail"))
      ParseInt(v, &out->relayFail);
    else if (EqNoCase(s, "ipv6_blocked"))
      ParseInt(v, &out->ipv6Blocked);
    else if (EqNoCase(s, "quic_blocked"))
      ParseInt(v, &out->quicBlocked);
    else if (EqNoCase(s, "last_result"))
      Copy(out->lastResult, (int)sizeof(out->lastResult), v);
    else if (EqNoCase(s, "last_app"))
      Copy(out->lastApp, (int)sizeof(out->lastApp), v);
    else if (EqNoCase(s, "last_target"))
      Copy(out->lastTarget, (int)sizeof(out->lastTarget), v);
    else if (EqNoCase(s, "last_error"))
      Copy(out->lastError, (int)sizeof(out->lastError), v);
  }
}

} // namespace appproxy

#endif
