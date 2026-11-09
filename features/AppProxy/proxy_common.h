#ifndef APEX_APPPROXY_COMMON_H
#define APEX_APPPROXY_COMMON_H

// ---------------------------------------------------------------------------
// AppProxy -- the ONE copy of its settings grammar, process matcher and proxy
// handshakes.
//
// This header is used in two very different places:
//   * feature_appproxy.cpp, which owns the page, file and process injection;
//   * AppProxyHook.cpp, the tiny payload loaded INSIDE a target program.
//
// The second one deliberately avoids the C/C++ runtime. Everything here is
// therefore fixed-size buffers and plain loops, with no allocation, no std
// strings and no file or socket calls. The payload supplies those.
// ---------------------------------------------------------------------------

#include "../../common/match.h"

namespace appproxy {

enum ProxyKind
{
  kSocks5 = 0,
  kHttpConnect = 1
};

static const int kMaxRules = 32;
static const int kMaxProfiles = 8;
static const int kMaxPattern = 64;
static const int kProfileNameCap = 64;
static const int kServerCap = 256;
static const int kUserCap = 128;
static const int kPasswordCap = 256;
static const int kSessionCap = 128;
static const int kPathCap = 512;
static const int kConfigCap = 16 * 1024;

struct Profile
{
  char name[kProfileNameCap];
  int kind;
  char server[kServerCap];
  int port;
  char username[kUserCap];
  char password[kPasswordCap];
};

struct Rule
{
  char pattern[kMaxPattern];
  int enabled;
  char profile[kProfileNameCap];
};

struct Config
{
  char session[kSessionCap];
  char windivertDir[kPathCap];
  int bypassLocal;
  int blockQuic;
  Profile profiles[kMaxProfiles];
  int profileCount;
  Rule rules[kMaxRules];
  int ruleCount;
};

inline int Len(const char *s)
{
  int n = 0;
  while (s && s[n])
    ++n;
  return n;
}

inline void Copy(char *dst, int cap, const char *src)
{
  if (!dst || cap <= 0)
    return;
  int n = 0;
  if (src)
  {
    while (src[n] && n < cap - 1)
    {
      dst[n] = src[n];
      ++n;
    }
  }
  dst[n] = 0;
}

inline void CopyN(char *dst, int cap, const char *src, int srcLen)
{
  if (!dst || cap <= 0)
    return;
  int n = 0;
  while (src && n < srcLen && n < cap - 1)
  {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
}

inline bool EqNoCase(const char *a, const char *b)
{
  if (!a || !b)
    return false;
  while (*a && *b)
  {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
    if (ca != cb)
      return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

inline bool ParseInt(const char *s, int *out)
{
  if (!s || !*s || !out)
    return false;
  int v = 0;
  int digits = 0;
  while (*s)
  {
    if (*s < '0' || *s > '9')
      return false;
    if (v > 1000000)
      return false;
    v = v * 10 + (*s - '0');
    ++digits;
    ++s;
  }
  if (!digits)
    return false;
  *out = v;
  return true;
}

inline bool ParseBool(const char *s, int *out)
{
  if (!s || !out)
    return false;
  if (s[0] == '1' && s[1] == 0)
  {
    *out = 1;
    return true;
  }
  if (s[0] == '0' && s[1] == 0)
  {
    *out = 0;
    return true;
  }
  if (EqNoCase(s, "true") || EqNoCase(s, "yes") || EqNoCase(s, "on"))
  {
    *out = 1;
    return true;
  }
  if (EqNoCase(s, "false") || EqNoCase(s, "no") || EqNoCase(s, "off"))
  {
    *out = 0;
    return true;
  }
  return false;
}

inline char *Trim(char *s)
{
  if (!s)
    return s;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    ++s;
  int n = Len(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                   s[n - 1] == '\n'))
    s[--n] = 0;
  return s;
}

inline void Defaults(Config *c)
{
  if (!c)
    return;
  char *p = (char *)c;
  for (unsigned i = 0; i < sizeof(Config); ++i)
    p[i] = 0;
  c->bypassLocal = 1;
  c->blockQuic = 1;
  c->profileCount = 1;
  Copy(c->profiles[0].name, (int)sizeof(c->profiles[0].name), "Default");
  c->profiles[0].kind = kSocks5;
  c->profiles[0].port = 1080;
}

inline int FindProfile(const Config *c, const char *name)
{
  if (!c || !name)
    return -1;
  for (int i = 0; i < c->profileCount; ++i)
    if (EqNoCase(c->profiles[i].name, name))
      return i;
  return -1;
}

inline bool AddProfile(Config *c, const char *name)
{
  if (!c || !name)
    return false;
  char trimmed[kProfileNameCap] = {0};
  int n = 0;
  while (*name == ' ' || *name == '\t')
    ++name;
  while (name[n] && name[n] != '|' && name[n] != '\r' && name[n] != '\n' &&
         n < (int)sizeof(trimmed) - 1)
  {
    trimmed[n] = name[n];
    ++n;
  }
  while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t'))
    trimmed[--n] = 0;
  if (!trimmed[0])
    return false;
  if (FindProfile(c, trimmed) >= 0)
    return true;
  if (c->profileCount >= kMaxProfiles)
    return false;
  Profile &p = c->profiles[c->profileCount++];
  char *pbytes = (char *)&p;
  for (unsigned i = 0; i < sizeof(Profile); ++i)
    pbytes[i] = 0;
  Copy(p.name, (int)sizeof(p.name), trimmed);
  p.kind = kSocks5;
  p.port = 1080;
  return true;
}

inline bool RemoveProfile(Config *c, int index)
{
  if (!c || index < 0 || index >= c->profileCount || c->profileCount <= 1)
    return false;
  const Profile &gone = c->profiles[index];
  for (int i = 0; i < c->ruleCount; ++i)
    if (c->rules[i].enabled && EqNoCase(c->rules[i].profile, gone.name))
      return false;
  for (int i = index; i < c->profileCount - 1; ++i)
    c->profiles[i] = c->profiles[i + 1];
  --c->profileCount;
  return true;
}

inline bool ProfileValid(const Profile *p)
{
  return p && p->name[0] && p->server[0] && p->port >= 1 && p->port <= 65535 &&
         (p->kind == kSocks5 || p->kind == kHttpConnect);
}

inline int FindRule(const Config *c, const char *pattern)
{
  if (!c || !pattern)
    return -1;
  for (int i = 0; i < c->ruleCount; ++i)
    if (EqNoCase(c->rules[i].pattern, pattern))
      return i;
  return -1;
}

inline bool AddRule(Config *c, const char *raw, int enabled, const char *profile = nullptr)
{
  if (!c || !raw)
    return false;
  char normal[kMaxPattern] = {0};
  apex::match::NormaliseName(raw, normal, (int)sizeof(normal));
  if (!normal[0])
    return false;
  const int old = FindRule(c, normal);
  if (old >= 0)
  {
    c->rules[old].enabled = enabled ? 1 : 0;
    if (profile && profile[0])
      Copy(c->rules[old].profile, (int)sizeof(c->rules[old].profile), profile);
    return true;
  }
  if (c->ruleCount >= kMaxRules)
    return false;
  Rule &r = c->rules[c->ruleCount++];
  Copy(r.pattern, (int)sizeof(r.pattern), normal);
  r.enabled = enabled ? 1 : 0;
  Copy(r.profile, (int)sizeof(r.profile),
       profile && profile[0] ? profile : (c->profileCount > 0 ? c->profiles[0].name : ""));
  return true;
}

inline bool RemoveRule(Config *c, int index)
{
  if (!c || index < 0 || index >= c->ruleCount)
    return false;
  for (int i = index; i < c->ruleCount - 1; ++i)
    c->rules[i] = c->rules[i + 1];
  --c->ruleCount;
  return true;
}

inline int RuleForExe(const Config *c, const char *exe)
{
  if (!c || !exe)
    return -1;
  char normal[kMaxPattern] = {0};
  apex::match::NormaliseName(exe, normal, (int)sizeof(normal));
  for (int i = 0; i < c->ruleCount; ++i)
    if (c->rules[i].enabled && apex::match::Pattern(c->rules[i].pattern, normal) &&
        FindProfile(c, c->rules[i].profile) >= 0)
      return i;
  return -1;
}

inline const Profile *ProfileForExe(const Config *c, const char *exe)
{
  const int rule = RuleForExe(c, exe);
  if (rule < 0)
    return nullptr;
  const int profile = FindProfile(c, c->rules[rule].profile);
  return profile >= 0 ? &c->profiles[profile] : nullptr;
}

inline bool Valid(const Config *c)
{
  if (!c || c->profileCount <= 0 || c->ruleCount <= 0)
    return false;
  bool hasEnabled = false;
  for (int i = 0; i < c->ruleCount; ++i)
  {
    if (!c->rules[i].enabled)
      continue;
    hasEnabled = true;
    const int profile = FindProfile(c, c->rules[i].profile);
    if (profile < 0 || !ProfileValid(&c->profiles[profile]))
      return false;
  }
  return hasEnabled;
}

// Parse the tiny INI the feature owns. It is deliberately a flat list:
//
//   bypass_local=1
//   block_quic=1
//   profile=Name|socks5|host|1080|user|pass
//   app=chrome.exe|1|Name
//
// The password is stored in this file in plain text. Apex is portable and has
// no credential store; the settings page says so next to the field instead of
// pretending otherwise.
inline void ParseConfig(const char *text, int textLen, Config *out)
{
  if (!out)
    return;
  Defaults(out);
  if (!text || textLen <= 0)
    return;

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
    if (!s[0] || s[0] == '#' || s[0] == ';')
      continue;
    char *eq = s;
    while (*eq && *eq != '=')
      ++eq;
    if (*eq != '=')
      continue;
    *eq++ = 0;
    s = Trim(s);
    char *v = Trim(eq);

    if (EqNoCase(s, "profile"))
    {
      char *parts[6] = {v, nullptr, nullptr, nullptr, nullptr, nullptr};
      int count = 1;
      for (char *p = v; *p && count < 6; ++p)
        if (*p == '|')
        {
          *p = 0;
          parts[count++] = p + 1;
        }
      if (count >= 4 && AddProfile(out, Trim(parts[0])))
      {
        Profile &profile = out->profiles[FindProfile(out, Trim(parts[0]))];
        if (EqNoCase(parts[1], "http") || EqNoCase(parts[1], "http-connect") ||
            EqNoCase(parts[1], "https"))
          profile.kind = kHttpConnect;
        else if (EqNoCase(parts[1], "socks5") || EqNoCase(parts[1], "socks"))
          profile.kind = kSocks5;
        Copy(profile.server, (int)sizeof(profile.server), Trim(parts[2]));
        int port = 0;
        if (ParseInt(Trim(parts[3]), &port) && port >= 1 && port <= 65535)
          profile.port = port;
        if (count > 4)
          Copy(profile.username, (int)sizeof(profile.username), parts[4]);
        if (count > 5)
          Copy(profile.password, (int)sizeof(profile.password), parts[5]);
      }
    }
    // Legacy single-proxy keys are read into the first profile, so an existing AppProxy.ini upgrades
    // without losing its server or needing the user to recreate it.
    else if (EqNoCase(s, "kind"))
    {
      if (EqNoCase(v, "http") || EqNoCase(v, "http-connect") || EqNoCase(v, "https"))
        out->profiles[0].kind = kHttpConnect;
      else if (EqNoCase(v, "socks5") || EqNoCase(v, "socks"))
        out->profiles[0].kind = kSocks5;
    }
    else if (EqNoCase(s, "server"))
    {
      Copy(out->profiles[0].server, (int)sizeof(out->profiles[0].server), v);
    }
    else if (EqNoCase(s, "port"))
    {
      int value = 0;
      if (ParseInt(v, &value) && value >= 1 && value <= 65535)
        out->profiles[0].port = value;
    }
    else if (EqNoCase(s, "username"))
    {
      Copy(out->profiles[0].username, (int)sizeof(out->profiles[0].username), v);
    }
    else if (EqNoCase(s, "password"))
    {
      Copy(out->profiles[0].password, (int)sizeof(out->profiles[0].password), v);
    }
    else if (EqNoCase(s, "session"))
    {
      Copy(out->session, (int)sizeof(out->session), v);
    }
    else if (EqNoCase(s, "windivert_dir"))
    {
      Copy(out->windivertDir, (int)sizeof(out->windivertDir), v);
    }
    else if (EqNoCase(s, "bypass_local"))
    {
      int value = 0;
      if (ParseBool(v, &value))
        out->bypassLocal = value;
    }
    else if (EqNoCase(s, "block_quic"))
    {
      int value = 0;
      if (ParseBool(v, &value))
        out->blockQuic = value;
    }
    else if (EqNoCase(s, "app"))
    {
      int enabled = 1;
      char profile[kProfileNameCap] = {0};
      char *bar = v;
      while (*bar && *bar != '|')
        ++bar;
      if (*bar == '|')
      {
        *bar++ = 0;
        char *second = bar;
        while (*second && *second != '|')
          ++second;
        if (*second == '|')
        {
          *second++ = 0;
          Copy(profile, (int)sizeof(profile), Trim(second));
        }
        int parsed = 0;
        if (ParseBool(Trim(bar), &parsed))
          enabled = parsed;
      }
      AddRule(out, Trim(v), enabled, profile);
    }
  }
}

inline bool EnabledFor(const Config *c, const char *exe)
{
  return RuleForExe(c, exe) >= 0;
}

inline int Base64(const char *src, char *out, int cap)
{
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!src || !out || cap <= 0)
    return 0;
  int n = Len(src);
  int o = 0;
  for (int i = 0; i < n; i += 3)
  {
    unsigned v = (unsigned char)src[i] << 16;
    const int remain = n - i;
    if (remain > 1)
      v |= (unsigned char)src[i + 1] << 8;
    if (remain > 2)
      v |= (unsigned char)src[i + 2];
    const char c0 = table[(v >> 18) & 63];
    const char c1 = table[(v >> 12) & 63];
    const char c2 = remain > 1 ? table[(v >> 6) & 63] : '=';
    const char c3 = remain > 2 ? table[v & 63] : '=';
    if (o + 4 >= cap)
      break;
    out[o++] = c0;
    out[o++] = c1;
    out[o++] = c2;
    out[o++] = c3;
  }
  out[o] = 0;
  return o;
}

inline int Append(char *out, int cap, int off, const char *s)
{
  while (s && *s && off < cap - 1)
    out[off++] = *s++;
  out[off] = 0;
  return off;
}

inline int BuildHttpConnect(const char *authority, const char *user, const char *password,
                            char *out, int cap)
{
  if (!authority || !out || cap <= 0)
    return 0;
  out[0] = 0;
  int o = 0;
  o = Append(out, cap, o, "CONNECT ");
  o = Append(out, cap, o, authority);
  o = Append(out, cap, o, " HTTP/1.1\r\nHost: ");
  o = Append(out, cap, o, authority);
  o = Append(out, cap, o, "\r\nProxy-Connection: Keep-Alive\r\n");
  if (user && user[0])
  {
    char joined[kUserCap + kPasswordCap + 2] = {0};
    int n = 0;
    for (const char *p = user; *p && n < (int)sizeof(joined) - 1; ++p)
      joined[n++] = *p;
    joined[n++] = ':';
    for (const char *p = password ? password : ""; *p && n < (int)sizeof(joined) - 1; ++p)
      joined[n++] = *p;
    joined[n] = 0;
    char token[((kUserCap + kPasswordCap + 2) * 4) / 3 + 8] = {0};
    Base64(joined, token, (int)sizeof(token));
    o = Append(out, cap, o, "Proxy-Authorization: Basic ");
    o = Append(out, cap, o, token);
    o = Append(out, cap, o, "\r\n");
  }
  o = Append(out, cap, o, "\r\n");
  return o;
}

inline int BuildSocks5Greeting(int useAuth, unsigned char *out, int cap)
{
  if (!out || cap < 4)
    return 0;
  out[0] = 0x05;
  out[1] = useAuth ? 2 : 1;
  out[2] = 0x00;
  int n = 3;
  if (useAuth)
    out[n++] = 0x02;
  return n;
}

inline int BuildSocks5Auth(const char *user, const char *password, unsigned char *out, int cap)
{
  if (!user || !password || !out)
    return 0;
  const int un = Len(user);
  const int pn = Len(password);
  if (un > 255 || pn > 255 || cap < un + pn + 3)
    return 0;
  int o = 0;
  out[o++] = 0x01;
  out[o++] = (unsigned char)un;
  for (int i = 0; i < un; ++i)
    out[o++] = (unsigned char)user[i];
  out[o++] = (unsigned char)pn;
  for (int i = 0; i < pn; ++i)
    out[o++] = (unsigned char)password[i];
  return o;
}

inline int BuildSocks5Connect(int atyp, const unsigned char *addr, int addrLen, int port,
                              unsigned char *out, int cap)
{
  if (!addr || !out || cap < addrLen + 7)
    return 0;
  int o = 0;
  out[o++] = 0x05;
  out[o++] = 0x01;
  out[o++] = 0x00;
  out[o++] = (unsigned char)atyp;
  for (int i = 0; i < addrLen; ++i)
    out[o++] = addr[i];
  out[o++] = (unsigned char)((port >> 8) & 0xFF);
  out[o++] = (unsigned char)(port & 0xFF);
  return o;
}

} // namespace appproxy

#endif // APEX_APPPROXY_COMMON_H
