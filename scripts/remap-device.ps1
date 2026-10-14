param(
    [Parameter(Mandatory)][string]$OldId,
    [Parameter(Mandatory)][string]$NewId,
    [string]$ProjectDirectory=(Split-Path $PSScriptRoot -Parent),
    [switch]$Preview
)
$ErrorActionPreference='Stop'
if (!$OldId -or !$NewId -or $OldId -match '["\r\n]' -or $NewId -match '["\r\n]') { throw '有効な端子IDを指定してください' }
$configPath=Join-Path $ProjectDirectory 'config/aggregate.ini'
$routesPath=Join-Path $ProjectDirectory 'config/router.txt'
$originalConfig=[IO.File]::ReadAllText($configPath)
$originalRoutes=if (Test-Path $routesPath) {[IO.File]::ReadAllText($routesPath)} else {''}
$configText=$originalConfig -replace ('(?m)^(id|native_wasapi_endpoint)='+[regex]::Escape($OldId)+'\r?$'),('${1}='+$NewId)
$configText=[regex]::Replace($configText,'(?m)^native_wasapi_endpoint=([^\r\n]*)',{param($match) 'native_wasapi_endpoint='+(($match.Groups[1].Value.Split(',') | ForEach-Object {if($_ -eq $OldId) {$NewId} else {$_}}) -join ',')})
$direction='';$seen=@{}
foreach($line in ($configText -split '\r?\n')) {if($line -match '^\[(capture|playback)\.') {$direction=$Matches[1]} elseif($line -match '^\[') {$direction=''} elseif($direction -and $line -match '^id=(.+)$') {$key=$direction+':'+$Matches[1];if($seen.ContainsKey($key)) {throw '置換先が同じ方向の別端子と重複します'};$seen[$key]=$true}}
$routeText=$originalRoutes.Replace(('"loopback:'+$OldId+':'),('"loopback:'+$NewId+':')).Replace(('port input "'+$OldId+'"'),('port input "'+$NewId+'"')).Replace(('port output "'+$OldId+'"'),('port output "'+$NewId+'"')).Replace(('"wasapi:'+$OldId+':'),('"wasapi:'+$NewId+':')).Replace(('source "'+$OldId+'"'),('source "'+$NewId+'"'))
if ($configText -eq $originalConfig -and $routeText -eq $originalRoutes) { throw '置換元IDが保存設定にありません' }
Write-Output "端子割り当て変更: $OldId -> $NewId"
if ($Preview) { return }
$backup=Join-Path $ProjectDirectory ('build/backups/remap-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $backup | Out-Null
[IO.File]::WriteAllText((Join-Path $backup 'aggregate.ini'),$originalConfig)
if (Test-Path $routesPath) {[IO.File]::WriteAllText((Join-Path $backup 'router.txt'),$originalRoutes)}
try {
    [IO.File]::WriteAllText($configPath,$configText)
    if (Test-Path $routesPath) {[IO.File]::WriteAllText($routesPath,$routeText)}
} catch {
    [IO.File]::WriteAllText($configPath,$originalConfig)
    if (Test-Path $routesPath) {[IO.File]::WriteAllText($routesPath,$originalRoutes)}
    throw
}
Write-Output "保存完了。復元元: $backup / GUIを開き直してください。同じチャンネル構成の機器への割り当て変更用"
