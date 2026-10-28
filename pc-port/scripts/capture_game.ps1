param([string]$OutDir = ".", [string]$AtSeconds = "", [string]$Name = "game")
# Captures ONLY the game's window (iss-probe.exe), even behind other windows or in the
# background: the window's own content through PrintWindow(PW_RENDERFULLCONTENT), never a copy of
# the screen (another window on top is never in the image). Not while minimized.
#   powershell -File capture_game.ps1 -OutDir shots                     one capture now
#   powershell -File capture_game.ps1 -OutDir shots -AtSeconds 40,55,70  at these seconds of the game
# For scripted runs the renderer's own capture is exact: ISS_SHOT_AT=40,55,70 (scripts/auto_porte.py).
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GameWindow {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@
New-Item -ItemType Directory -Force $OutDir | Out-Null
function Find-Game {
  Get-Process iss-probe -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
}
function Capture([System.Diagnostics.Process]$p, [string]$file) {
  $h = $p.MainWindowHandle
  if ([GameWindow]::IsIconic($h)) { "minimized: not captured"; return }
  $r = New-Object GameWindow+RECT
  [GameWindow]::GetClientRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -le 0 -or $hh -le 0) { "no client area"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  # 1 = PW_CLIENTONLY, 2 = PW_RENDERFULLCONTENT (DirectX/Vulkan content through DWM)
  $ok = [GameWindow]::PrintWindow($h, $dc, 3)
  $g.ReleaseHdc($dc)
  $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  if ($ok) { "saved $file" } else { "PrintWindow failed" }
}
$p = $null
while (-not $p) { $p = Find-Game; if (-not $p) { Start-Sleep -Milliseconds 500 } }
if (-not $AtSeconds) {
  Capture $p (Join-Path $OutDir ("{0}-{1:yyyyMMdd-HHmmss}.png" -f $Name, (Get-Date)))
  exit
}
$start = $p.StartTime
foreach ($s in ($AtSeconds -split "," | ForEach-Object { [int]$_ })) {
  while (((Get-Date) - $start).TotalSeconds -lt $s) {
    if ($p.HasExited) { "exited"; exit }
    Start-Sleep -Milliseconds 200
  }
  $p.Refresh()
  Capture $p (Join-Path $OutDir ("{0}-t{1:D3}.png" -f $Name, $s))
}
