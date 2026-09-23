# Measure a window's CAPTION brightness, in numbers rather than by eye.
#
# A screenshot of the whole window was tried first and was not trustworthy: the capture lands on
# whatever window the shell considers "main", and a caption one shade different is hard to judge by eye
# anyway. This samples a horizontal strip inside the caption and reports the MEAN luminance -- a light
# caption is around 240, a dark one around 30.
#
# The window is found by PROCESS, walking the top-level windows, because FindWindow-by-class was tried
# and did not match this application's window in this environment while it happily matched the shell's
# (the shell's window is what proved the call itself works, so the class string was the problem, not the
# API). Enumerating and filtering on the process id needs no name to be right.
#
# usage: caption_brightness.ps1 <target process id>
#
# ⚠️ THE CLASS NAME BELOW IS "ApexSettingsWnd" (APEX_SETTINGS_WND_CLASS in apex/settings_ipc.h). It used to
# be "ApexNoDarkWnd", a window from the spike that came before the panel existed -- so this script kept
# answering "no-window" about a window that was on screen, which reads as "the panel is not running" rather
# than as "this probe is looking for the wrong thing".
#
# (`$Pid` is PowerShell's own automatic variable -- the shell's process id -- so the parameter
# cannot be called that; assigning it fails before any of this runs.)
param([int]$TargetPid = 0)

if ($TargetPid -le 0) { Write-Output "RESULT=no-pid"; exit 2 }

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class CapB {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
  public struct R { public int L, T, Rt, B; }

  public static IntPtr Find(int want) {
    IntPtr result = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid != (uint)want) return true;
      var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
      if (sb.ToString() != "ApexSettingsWnd") return true;
      if (!IsWindowVisible(h)) return true;
      result = h;
      return false; // stop at the first match
    }, IntPtr.Zero);
    return result;
  }
}
"@

$h = [CapB]::Find($TargetPid)
if ($h -eq [IntPtr]::Zero) { Write-Output "RESULT=no-window"; exit 1 }

$r = New-Object CapB+R
[CapB]::GetWindowRect($h, [ref]$r) | Out-Null

# A few pixels down from the top edge, across the middle of the caption: clear of the icon at the left
# and the buttons at the right, and above the frame.
$dc = [CapB]::GetDC([IntPtr]::Zero)
$y = $r.T + 9
$x0 = $r.L + [int](($r.Rt - $r.L) * 0.30)
$x1 = $r.L + [int](($r.Rt - $r.L) * 0.70)

$sum = 0.0
$n = 0
$sample = ""
for ($x = $x0; $x -lt $x1; $x += 4) {
  $p = [CapB]::GetPixel($dc, $x, $y)
  $rr = $p -band 0xFF
  $gg = ($p -shr 8) -band 0xFF
  $bb = ($p -shr 16) -band 0xFF
  # Rec.601 luma, the same weighting the plugin uses for its own light/dark decision.
  $sum += (0.299 * $rr + 0.587 * $gg + 0.114 * $bb)
  ++$n
  if ($n -le 3) { $sample += ("({0},{1},{2}) " -f $rr, $gg, $bb) }
}
[CapB]::ReleaseDC([IntPtr]::Zero, $dc) | Out-Null

$mean = if ($n -gt 0) { $sum / $n } else { -1 }
$verdict = if ($mean -lt 90) { "DARK" } elseif ($mean -gt 180) { "LIGHT" } else { "MID (accent?)" }
Write-Output ("rect={0},{1} {2}x{3}  samples={4}  first={5}" -f $r.L, $r.T, ($r.Rt - $r.L), ($r.B - $r.T), $n, $sample)
Write-Output ("caption mean luma = {0:N1}  ->  {1}" -f $mean, $verdict)
Write-Output ("RESULT={0}" -f $verdict)
