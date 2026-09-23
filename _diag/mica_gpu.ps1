# _diag/mica_gpu.ps1 -- WHAT DOES THE MATERIAL COST, on the DWM/GPU side?
#
# The probe reports its OWN cpu time over the hold (the `perf:` line in its log). DWM's CPU time is NOT readable
# on this machine: `(Get-Process dwm).TotalProcessorTime` comes back empty and the probe's OpenProcess on dwm
# fails, both of which are access denials rather than zeroes. So the DWM side is measured through the GPU
# performance counters, which do work here: \GPU Engine(*)\Utilization Percentage carries one instance per
# (process, engine), so summing the instances of a pid says how much GPU work that process asked for.
#
# ⚠️ READ THE NUMBERS AS "SAME ORDER OF MAGNITUDE", NOT AS PRECISE. Samples are ~1.4 s apart (a Get-Counter call
# over 300 instances is not free), the ambient baseline is measured once and not interleaved, and the desktop is
# live while this runs. The noise floor is visible in the output itself: the OPAQUE window measures BELOW the
# "no window" baseline, i.e. the run-to-run scatter is around +-1 unit.
#
# usage: powershell -NoProfile -ExecutionPolicy Bypass -File _diag/mica_gpu.ps1
$ErrorActionPreference = 'Continue'
$R = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\_mica_run'
$Exe = Join-Path $R 'mica_probe.exe'
if (-not (Test-Path $Exe)) { Write-Host "missing $Exe -- build it first: bash _diag/mica_probe.sh"; exit 1 }
$dwmPid = (Get-Process dwm).Id
Write-Host "dwm pid = $dwmPid"
Write-Host "probe:   $Exe"

function SampleGpu([int]$a, [int]$b) {
  $res = @{ a = 0.0; b = 0.0 }
  $s = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue
  if (-not $s) { return $res }
  foreach ($c in $s.CounterSamples) {
    $inst = [string]$c.InstanceName
    if ($inst.StartsWith("pid_$a`_")) { $res.a += [double]$c.CookedValue }
    if ($inst.StartsWith("pid_$b`_")) { $res.b += [double]$c.CookedValue }
  }
  return $res
}

function Baseline([int]$seconds) {
  $vals = @()
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt $seconds) {
    Start-Sleep -Milliseconds 400
    $vals += (SampleGpu $dwmPid 0).a
  }
  $m = ($vals | Measure-Object -Average).Average
  Write-Host ("baseline: dwm gpu mean = {0:N2}   ({1} samples, no probe window up)" -f $m, $vals.Count)
  return $m
}

function Mode([string]$tag, [string]$backdrop, [int]$ck) {
  $log = Join-Path $R "perf-$tag.log"
  $bmp = Join-Path $R "perf-$tag.bmp"
  Remove-Item $log, $bmp -ErrorAction SilentlyContinue
  $pr = Start-Process -FilePath $Exe -PassThru -ArgumentList `
    "--backdrop=$backdrop", "--dark=1", "--colorkey=$ck", "--gdi=1", "--hold=8000", `
    "--out=$bmp", "--log=$log"
  $dwmVals = @(); $meVals = @()
  $t0 = Get-Date
  while (-not $pr.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 40) {
    Start-Sleep -Milliseconds 400
    if ($pr.HasExited) { break }
    $r = SampleGpu $dwmPid $pr.Id
    $dwmVals += $r.a
    if ($r.b -gt 0) { $meVals += $r.b }
  }
  $pr.WaitForExit(5000) | Out-Null
  $perf = Select-String -Path $log -Pattern '^perf:' -ErrorAction SilentlyContinue
  $cpuLine = if ($perf) { $perf[0].Line } else { '(no perf line)' }
  $dwmMean = if ($dwmVals.Count) { ($dwmVals | Measure-Object -Average).Average } else { 0 }
  $meMean = if ($meVals.Count) { ($meVals | Measure-Object -Average).Average } else { 0 }
  # ⚠️ Write-Host, NOT Write-Output: a function's Write-Output is part of its RETURN value, and the first version
  # of this script returned the report lines mixed in with the numbers, so every delta line failed on an array
  # subtraction while the numbers themselves were fine.
  Write-Host ("{0,-9} samples={1,2}  dwm gpu mean = {2,6:N2}   probe gpu mean = {3,6:N2}   {4}" -f `
    $tag, $dwmVals.Count, $dwmMean, $meMean, $cpuLine)
  return [pscustomobject]@{ mode = $tag; dwm = [math]::Round($dwmMean, 2); me = [math]::Round($meMean, 2) }
}

Write-Host ''
Write-Host 'WARNING: five test windows will appear, one after another, ~8 s each. Do not click them.'
Write-Host ''
$base = Baseline 6
Write-Host ''
$results = @()
$results += Mode 'plain'   'none'    0   # opaque window, no material  -- closest to the panel as it is today
$results += Mode 'hole'    'none'    1   # layered + colour key only    -- what the transparency mechanism costs
$results += Mode 'mica'    'mica'    1
$results += Mode 'micaalt' 'micaalt' 1
$results += Mode 'acrylic' 'acrylic' 1
Write-Host ''
Write-Host ('ambient dwm gpu baseline (no window): {0:N2}' -f $base)
foreach ($r in $results) {
  Write-Host ('  {0,-9} dwm delta vs baseline = {1,6:N2}' -f $r.mode, ($r.dwm - $base))
}
