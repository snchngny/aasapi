param([string]$Compiler,[switch]$SkipBackend,[string]$BuildDirectory="build")
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (!$Compiler) {
    $candidate = Get-ChildItem (Join-Path $env:TEMP 'audio-aggregate-toolchain') -Filter clang++.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$candidate) { throw 'LLVM/MinGW clang++.exe のパスを -Compiler で指定してください' }
    $Compiler = $candidate.FullName
}
$build = Join-Path $project $BuildDirectory
New-Item -ItemType Directory -Force $build | Out-Null
$common = @('-std=c++20','-O2','-g','-DNOMINMAX','-DUNICODE','-D_UNICODE','-static','-I',"$project/src",'-I',"$project/vendor/asio",'-I',"$project/vendor/miniaudio")
if (!$SkipBackend -or !(Test-Path "$build/audio_backend.o")) {
    & $Compiler @common '-c' "$project/src/audio_backend.cpp" '-o' "$build/audio_backend.o"
    if ($LASTEXITCODE) { throw '音声バックエンドのビルドに失敗' }
}
& $Compiler @common '-shared' "$project/src/driver.cpp" "$project/src/driver.def" "$build/audio_backend.o" '-lole32' '-luuid' '-o' "$build/audio_aggregate.dll"
if ($LASTEXITCODE) { throw 'ASIO DLLのビルドに失敗' }
& $Compiler @common "$project/src/host.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-o' "$build/aggregate_host.exe"
if ($LASTEXITCODE) { throw '検証ホストのビルドに失敗' }
& $Compiler @common '-municode' '-mwindows' "$project/src/gui.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-lcomctl32' '-ladvapi32' '-lgdi32' '-o' "$build/aggregate_gui.exe"
if ($LASTEXITCODE) { throw 'GUIのビルドに失敗' }
& $Compiler @common '-municode' '-mwindows' "$project/src/router_gui.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-lcomctl32' '-lgdi32' '-lshell32' '-ladvapi32' '-o' "$build/aggregate_router.exe"
if ($LASTEXITCODE) { throw 'ルーティングGUIのビルドに失敗' }
Copy-Item -LiteralPath "$build/aggregate_router.exe" -Destination "$build/aasapi.exe" -Force
& $Compiler @common "$project/tests/core_test.cpp" '-lole32' '-luuid' '-o' "$build/core_test.exe"
if ($LASTEXITCODE) { throw '同期テストのビルドに失敗' }
& $Compiler @common "$project/tests/routing_test.cpp" '-o' "$build/routing_test.exe"
if ($LASTEXITCODE) { throw 'ルーティングテストのビルドに失敗' }
& $Compiler @common "$project/tests/router_integration_test.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-o' "$build/router_integration_test.exe"
if ($LASTEXITCODE) { throw 'ルーティング結合テストのビルドに失敗' }
& $Compiler @common "$project/src/calibrate.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-o' "$build/aggregate_calibrate.exe"
if ($LASTEXITCODE) { throw '遅延計測のビルドに失敗' }
& $Compiler @common "$project/tests/delay_test.cpp" "$build/audio_backend.o" '-lole32' '-luuid' '-o' "$build/delay_test.exe"
if ($LASTEXITCODE) { throw '遅延補正テストのビルドに失敗' }
Write-Output "Build complete: $build"
