// A THROWAWAY SECOND FEATURE, written only to answer one question: does adding a feature really cost one
// folder and nothing else? Deleted as soon as the audit is done.
//
// It is deliberately unlike SmoothWheel: it owns a top-level wheel with Ctrl held (which SmoothWheel
// refuses), it has a DIFFERENT set of controls, and it sends NO curve -- three things a panel that secretly
// knew about its one feature would trip over.
#include "abi.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace {

const ApexHost *g_host = nullptr;
double g_amount = 3.0;
int g_on = 1;
char g_own[64] = {0};

void Log(const char *fmt, ...)
{
  if (!g_host || !g_host->logLine)
    return;
  char buf[256] = {0};
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_host->logLine(buf);
}

int StInit(const ApexHost *host)
{
  g_host = host;
  char dir[512] = {0};
  if (host->featureDir)
    host->featureDir(dir, (int)sizeof(dir));
  // Its OWN settings file, in its OWN folder -- the thing that proves the folder is the unit.
  char path[600] = {0};
  _snprintf(path, sizeof(path), "%sauditstub.ini", dir);
  if (FILE *f = fopen(path, "rb"))
  {
    char line[128] = {0};
    while (fgets(line, sizeof(line), f))
    {
      int v = 0;
      if (sscanf(line, "amount=%d", &v) == 1)
        g_amount = (double)v;
      if (sscanf(line, "own=%63s", g_own) == 1)
        ; // its own private key
    }
    fclose(f);
  }
  Log("AuditStub: folder %s (amount %.0f)", dir[0] ? dir : "(unknown)", g_amount);
  return 0;
}

int StReload(void) { return 1; }

void StShutdown(void) {}

int StListOp(const char *id, const char *op, const char *value, int index) { (void)id; (void)op; (void)value; (void)index; return 0; }

int StSave(void)
{
  if (!g_host || !g_host->featureDir)
    return 0;
  char dir[512] = {0};
  g_host->featureDir(dir, (int)sizeof(dir));
  char path[600] = {0};
  _snprintf(path, sizeof(path), "%sauditstub.ini", dir);
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  fprintf(f, "amount=%.0f\n", g_amount);
  fclose(f);
  return 1;
}

// A CONTROL SET THAT SHARES NOTHING WITH SMOOTHWHEEL: different ids, different types, a bool and a range.
int StJson(char *out, int outSize)
{
  if (!out || outSize <= 0)
    return 0;
  _snprintf(out, outSize,
            "{\"params\":["
            "{\"id\":\"on\",\"type\":\"bool\",\"labelZh\":\"\xe5\xbc\x80\xe5\x90\xaf\",\"labelEn\":\"On\","
            "\"value\":%d},"
            "{\"id\":\"amount\",\"type\":\"range\",\"labelZh\":\"\xe6\x95\xb0\xe9\x87\x8f\",\"labelEn\":\"Amount\","
            "\"min\":1,\"max\":10,\"step\":1,\"value\":%.0f,\"def\":3,\"unit\":\"d\",\"hue\":\"#78BEFF\"}"
            "]}",
            g_on, g_amount);
  return (int)strlen(out);
}

int StApply(const char *id, const char *text)
{
  if (!id || !text)
    return 0;
  const double v = atof(text); // ABI 6: the value is text; this feature's controls are numbers
  if (strcmp(id, "on") == 0)
    g_on = (v != 0.0);
  else if (strcmp(id, "amount") == 0)
    g_amount = v;
  else
    return 0;
  return 1;
}

unsigned StFlags(void)
{
  unsigned f = APEX_FEATURE_ENABLED;
  if (g_on)
    f |= APEX_FEATURE_ACTIVE;
  return f;
}

// TAKES Ctrl+WHEEL: the gesture SmoothWheel explicitly declines, so both features can be live at once and
// the decision layer has to route between them.
int StOnWheel(const ApexWheelEvent *ev)
{
  if (!ev || !g_on)
    return 0;
  if (ev->key != 2) // Ctrl only
    return 0;
  if (ev->delta == 0)
    return 0;
  ApexTarget t;
  if (!g_host->targetAt || !g_host->targetAt(ev->x, ev->y, &t))
    return 0;
  return 1; // swallow; nothing is injected, which the invariant allows because a feature claimed it
}

double StTick(double dtSec)
{
  (void)dtSec;
  return 0.0;
}

} // namespace

extern "C" __declspec(dllexport) const ApexFeature *__cdecl ApexFeatureEntry(void)
{
  static const ApexFeature k = {
      APEX_ABI_VERSION, sizeof(ApexFeature),
      "AuditStub", "\xe5\xae\xa1\xe8\xae\xa1\xe6\xa1\xa9", "Audit Stub", "0.0.1",
      StInit,          // int (*)(const ApexHost*)
      StShutdown,      // void (*)()
      StReload,        // int (*)()
      StJson,          // int (*)(char*, int)
      StApply,         // int (*)(const char*, double)
      StListOp,        // int (*)(const char*, const char*, const char*, int)
      nullptr,         // int (*)(ApexQuickItem*, int) -- this stub puts nothing in the quick panel
      StSave,          // int (*)()
      StOnWheel,       // int (*)(const ApexWheelEvent*)
      StTick,          // double (*)(double)
      StFlags,         // unsigned (*)()
  };
  return &k;
}
