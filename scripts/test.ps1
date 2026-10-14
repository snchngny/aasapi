param([string]$HardwareConfig,[int]$Seconds=30,[int]$Cycles=2,[string]$BuildDirectory="build")
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $project $BuildDirectory
$results = Join-Path $project 'build/results'
New-Item -ItemType Directory -Force $results | Out-Null
& "$binary/delay_test.exe" | Tee-Object "$results/delay.txt"
if ($LASTEXITCODE) { throw '出力別遅延と校正の検証に失敗' }
$sources = Get-Content (Join-Path $project 'vendor/sources.json') -Raw | ConvertFrom-Json
foreach ($item in $sources.files.PSObject.Properties) {
    if ((Get-FileHash (Join-Path "$project/vendor" $item.Name) -Algorithm SHA256).Hash.ToLower() -ne $item.Value) { throw "依存ファイルのSHA256不一致: $($item.Name)" }
}
& "$binary/core_test.exe" | Tee-Object "$results/core.txt"
if ($LASTEXITCODE) { throw '同期エンジン検証に失敗' }
& "$binary/routing_test.exe" | Tee-Object "$results/routing.txt"
if ($LASTEXITCODE) { throw 'ルーティング検証に失敗' }
& "$binary/router_integration_test.exe" | Tee-Object "$results/router-integration.txt"
if ($LASTEXITCODE) { throw 'ルーティング結合検証に失敗' }
& "$binary/aggregate_host.exe" probe "$project/config/simulation.ini" 2 2 | Tee-Object "$results/asio-simulation.txt"
if ($LASTEXITCODE) { throw 'ASIO入出力・再起動検証に失敗' }
if ($HardwareConfig) {
    $config = if ([System.IO.Path]::IsPathRooted($HardwareConfig)) { $HardwareConfig } else { Join-Path $project $HardwareConfig }
    & "$binary/aggregate_host.exe" probe $config $Seconds $Cycles | Tee-Object "$results/hardware.txt"
    if ($LASTEXITCODE) { throw '実機検証に失敗。build/results/hardware.txtを確認してください' }
}

& "$binary/aggregate_calibrate.exe" --self-test | Tee-Object "$results/calibration.txt"
if ($LASTEXITCODE) { throw '遅延計測検証に失敗' }
$temporary=Join-Path ([IO.Path]::GetTempPath()) ('audio-remap-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$temporary/config" | Out-Null
[IO.File]::WriteAllText("$temporary/config/aggregate.ini","[engine]`nnative_wasapi_endpoint=old,other`n[playback.0]`nid=old`nchannels=2`n")
[IO.File]::WriteAllText("$temporary/config/router.txt",'version 1' + "`n" + 'source "old"' + "`n" + 'route "pc:L" "wasapi:old:0"' + "`n" + 'route "loopback:old:1" "wasapi:old:1"' + "`n" + 'port output "old" 2' + "`n")
& "$project/scripts/remap-device.ps1" -ProjectDirectory $temporary -OldId old -NewId new | Out-Null
if ([IO.File]::ReadAllText("$temporary/config/aggregate.ini") -notmatch 'native_wasapi_endpoint=new,other' -or [IO.File]::ReadAllText("$temporary/config/router.txt") -notmatch 'wasapi:new:0' -or [IO.File]::ReadAllText("$temporary/config/router.txt") -notmatch 'loopback:new:1' -or [IO.File]::ReadAllText("$temporary/config/router.txt") -notmatch 'port output "new" 2') {throw '端子割り当て移行に失敗'}
'PASS: endpoint ID remapping, source and route preservation, multi-endpoint native mapping' | Tee-Object "$results/remapping.txt"
