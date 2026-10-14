$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$sources = Get-Content (Join-Path $project 'vendor/sources.json') -Raw | ConvertFrom-Json
$target = Join-Path $env:TEMP 'audio-aggregate-toolchain'
New-Item -ItemType Directory -Force $target | Out-Null
$archive = Join-Path $target 'compiler.zip'
if (!(Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $sources.compiler_sha256) {
    Invoke-WebRequest $sources.compiler_url -OutFile $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $sources.compiler_sha256) { throw 'コンパイラのSHA256が一致しません' }
$sevenZip = 'C:/Program Files/7-Zip/7z.exe'
if (Test-Path $sevenZip) {
    & $sevenZip x $archive "-o$target" -y -bso0 -bsp0
    if ($LASTEXITCODE) { throw 'コンパイラの展開に失敗' }
} else {
    Expand-Archive -LiteralPath $archive -DestinationPath $target -Force
}
Get-ChildItem $target -Filter clang++.exe -Recurse | Select-Object -First 1 -ExpandProperty FullName
