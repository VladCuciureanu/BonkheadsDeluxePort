# Debug helper: run the port headless-ish and save a screenshot after N seconds.
# usage: tools\shot.ps1 <seconds> <outname> [extra args...]
param([double]$Seconds, [string]$Name, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra)
$build = Join-Path $PSScriptRoot "..\build"
$logs = Join-Path $build "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$scratch = $env:BONK_SHOT_DIR; if (-not $scratch) { $scratch = $logs }
$bmp = Join-Path $logs "$Name.bmp"
$log = Join-Path $logs "$Name.log"
$runArgs = New-Object System.Collections.Generic.List[string]
if (-not $env:BONK_SOUND) { $runArgs.Add("--nosound") }
$runArgs.Add("--shot"); $runArgs.Add("$Seconds"); $runArgs.Add("`"$bmp`"")
if ($Extra) { foreach ($e in $Extra) { $runArgs.Add($e) } }
$p = Start-Process -FilePath (Join-Path $build "bonkheads.exe") -ArgumentList $runArgs.ToArray() -WorkingDirectory $build -RedirectStandardError $log -PassThru
if (-not $p.WaitForExit([int](($Seconds + 15) * 1000))) { Stop-Process -Id $p.Id -Force; Write-Output "TIMEOUT" }
Get-Content $log -Tail 6
if (Test-Path $bmp) { python -I (Join-Path $PSScriptRoot "bmp2png.py") $bmp (Join-Path $scratch "$Name.png") }
