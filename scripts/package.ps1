param([string]$BuildDirectory='build')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$binary=Join-Path $project $BuildDirectory
$destination=Join-Path $project ('build/packages/aasapi-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force "$destination/build","$destination/config","$destination/scripts","$destination/docs","$destination/vendor/miniaudio","$destination/vendor/asio" | Out-Null
foreach ($file in 'aasapi.exe','aggregate_gui.exe','aggregate_router.exe','aggregate_host.exe','aggregate_calibrate.exe','audio_aggregate.dll') {Copy-Item -LiteralPath (Join-Path $binary $file) -Destination "$destination/build"}
foreach ($file in 'LICENSE','THIRD_PARTY_NOTICES.md') {Copy-Item -LiteralPath (Join-Path $project $file) -Destination $destination}
foreach ($file in 'register-driver.ps1','remap-device.ps1') {Copy-Item -LiteralPath (Join-Path "$project/scripts" $file) -Destination "$destination/scripts"}
Copy-Item -LiteralPath "$project/docs/portable-usage.md" -Destination "$destination/docs"
Copy-Item -LiteralPath "$project/docs/portable-usage.md" -Destination "$destination/README.md"
Copy-Item -LiteralPath "$project/vendor/miniaudio/LICENSE" -Destination "$destination/vendor/miniaudio"
Copy-Item -LiteralPath "$project/vendor/asio/LICENSE.txt" -Destination "$destination/vendor/asio"
# Include the source used to build these binaries; never include local device
# configuration, captured diagnostics, caches, or the compiler archive.
foreach ($folder in 'src','tests') {Copy-Item -LiteralPath (Join-Path $project $folder) -Destination $destination -Recurse}
Copy-Item -Path "$project/scripts/*.ps1" -Destination "$destination/scripts"
Copy-Item -Path "$project/docs/*.md" -Destination "$destination/docs"
Copy-Item -Path "$project/vendor/asio/*.h" -Destination "$destination/vendor/asio"
Copy-Item -LiteralPath "$project/vendor/miniaudio/miniaudio.h" -Destination "$destination/vendor/miniaudio"
Copy-Item -LiteralPath "$project/vendor/sources.json" -Destination "$destination/vendor"
Copy-Item -LiteralPath "$project/vendor/README.md","$project/vendor/toolchain-LICENSE.txt" -Destination "$destination/vendor"
Copy-Item -LiteralPath "$project/config/simulation.ini" -Destination "$destination/config"
Copy-Item -LiteralPath "$project/README.md" -Destination "$destination/SOURCE_README.md"
$initialConfig=@'
# 配布先で設定GUIから主ASIOと追加機器を選び直して保存
[engine]
sample_rate=48000
block_frames=256
queue_blocks=2
exclusive=false
simulation=false
'@
[IO.File]::WriteAllText("$destination/config/aggregate.ini",$initialConfig)
$archive="$destination.zip"
Compress-Archive -LiteralPath $destination -DestinationPath $archive
Write-Output $archive
