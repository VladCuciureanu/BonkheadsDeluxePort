# Debug helper: run the port headless and save a screenshot after N seconds.
# usage: tools\debug\shot.ps1 <seconds> <outname> [extra args...]
# Each run uses a private copy of the game data (build/testdata/<name>), so your
# own prefs, saves and high scores are never touched. Set BONK_TEST_NETCFG to a
# line such as "host_address=127.0.0.1:24732" to seed the copy's network.cfg.
param([double]$Seconds, [string]$Name, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra)
$build = Join-Path $PSScriptRoot "..\..\build"
$logs = Join-Path $build "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$scratch = $env:BONK_SHOT_DIR; if (-not $scratch) { $scratch = $logs }
$bmp = Join-Path $logs "$Name.bmp"
$log = Join-Path $logs "$Name.log"
$src = Join-Path $build "data\Bonkheads Deluxe"
$data = Join-Path $build "testdata\$Name"
if (Test-Path $data) { Remove-Item $data -Recurse -Force }
New-Item -ItemType Directory -Force $data | Out-Null
Get-ChildItem $src -File | Where-Object { $_.Name -notlike "Bonkheads Deluxe Prefs*" -and $_.Name -ne "network.cfg" } |
    ForEach-Object { Copy-Item $_.FullName $data }
if ($env:BONK_TEST_NETCFG) { Set-Content -Path (Join-Path $data "network.cfg") -Value $env:BONK_TEST_NETCFG }
$runArgs = New-Object System.Collections.Generic.List[string]
$runArgs.Add("--headless"); $runArgs.Add("--data"); $runArgs.Add("`"$data`"")
if (-not $env:BONK_SOUND) { $runArgs.Add("--nosound") }
$runArgs.Add("--shot"); $runArgs.Add("$Seconds"); $runArgs.Add("`"$bmp`"")
if ($Extra) { foreach ($e in $Extra) { $runArgs.Add($e) } }
$p = Start-Process -FilePath (Join-Path $build "bonkheads.exe") -ArgumentList $runArgs.ToArray() -WorkingDirectory $build -RedirectStandardError $log -WindowStyle Hidden -PassThru
if (-not $p.WaitForExit([int](($Seconds + 15) * 1000))) { Stop-Process -Id $p.Id -Force; Write-Output "TIMEOUT" }
Get-Content $log -Tail 6
if (Test-Path $bmp) { python -I (Join-Path $PSScriptRoot "bmp2png.py") $bmp (Join-Path $scratch "$Name.png") }
