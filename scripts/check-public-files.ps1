param([switch]$TrackedOnly)
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$arguments=@('-C',$project,'ls-files','--cached')
if(!$TrackedOnly){$arguments+=@('--others','--exclude-standard')}
$files=@(& git @arguments | Sort-Object -Unique)
if($LASTEXITCODE){throw 'Gitの公開予定ファイルを取得できません'}
if(!$files.Count){throw '公開予定ファイルがありません'}
$rejected=[Collections.Generic.List[string]]::new()
foreach($file in $files){
    $allowed=$file -match '^(src/[^/]+\.(h|cpp|def)|tests/[^/]+\.cpp|scripts/[^/]+\.ps1|docs/portable-usage\.md|vendor/(asio/[^/]+\.(h|txt)|miniaudio/(miniaudio\.h|LICENSE)|sources\.json|README\.md|toolchain-LICENSE\.txt)|config/(simulation|aggregate\.example)\.ini|README\.md|THIRD_PARTY_NOTICES\.md|LICENSE|\.gitignore)$'
    if(!$allowed){$rejected.Add($file);continue}
    $text=[IO.File]::ReadAllText((Join-Path $project $file))
    # Do not print matching values. Preserve upstream copyright/contact notices.
    if($text -match '(?i)([A-Z]:[/\\]Users[/\\][^/\\\s]+|[/]Users[/][^/\s]+|\{0\.0\.[01]\.00000000\}\.\{[a-f0-9-]{36}\}|gh[pousr]_[a-zA-Z0-9]{20,}|github_pat_[a-zA-Z0-9_]{20,}|AKIA[A-Z0-9]{16}|-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----)'){$rejected.Add($file)}
}
if($rejected.Count){throw ('公開検査で要確認: '+($rejected -join ', '))}
Write-Output "PASS: $($files.Count) public source files; local settings, endpoint IDs, user paths and common secrets excluded"
