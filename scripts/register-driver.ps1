param([ValidateSet('Register','Unregister')][string]$Action='Register',[switch]$Preview)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$dll = [System.IO.Path]::GetFullPath((Join-Path $project 'build/audio_aggregate.dll'))
$clsid = '{A51448C1-33A9-44D4-9173-6784E1920D63}'
$asio = 'SOFTWARE\ASIO\Audio Aggregate Prototype'
$com = "SOFTWARE\Classes\CLSID\$clsid"
if ($Preview) {
    Write-Output "$Action : HKLM(64bit)\$asio"
    Write-Output "$Action : HKLM(64bit)\$com"
    Write-Output "DLL : $dll"
    return
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'DAWへの登録・解除は管理者PowerShellで実行してください。Previewは管理者権限不要です' }
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
try {
    $currentCom = $registry.OpenSubKey("$com\InprocServer32")
    $currentAsio = $registry.OpenSubKey($asio)
    try {
        if ($currentCom -and $currentCom.GetValue('') -ne $dll) { throw '同じCLSIDに別DLLが登録済みのため変更しません' }
        if ($currentAsio -and $currentAsio.GetValue('CLSID') -ne $clsid) { throw '同じASIO名に別CLSIDが登録済みのため変更しません' }
        if ($Action -eq 'Unregister' -and $currentAsio -and !$currentCom) { throw 'COM登録との対応を確認できないため解除しません' }
    } finally {
        if ($currentCom) { $currentCom.Dispose() }
        if ($currentAsio) { $currentAsio.Dispose() }
    }
    if ($Action -eq 'Register') {
        if (!(Test-Path $dll)) { throw 'ビルド済みDLLがありません' }
        if (!(Test-Path (Join-Path $project 'config/aggregate.ini'))) { throw 'config/aggregate.iniに利用する設定を用意してください' }
        $key = $registry.CreateSubKey("$com\InprocServer32")
        try { $key.SetValue('', $dll); $key.SetValue('ThreadingModel','Apartment') } finally { $key.Dispose() }
        $key = $registry.CreateSubKey($asio)
        try { $key.SetValue('CLSID',$clsid); $key.SetValue('Description','AASAPI') } finally { $key.Dispose() }
    } else {
        # Delete only the two fixed keys owned by this prototype, after ownership checks above.
        $registry.DeleteSubKeyTree($asio,$false)
        $registry.DeleteSubKeyTree($com,$false)
    }
} finally { $registry.Dispose() }
Write-Output "$Action 完了: AASAPI（互換登録キー: Audio Aggregate Prototype）"
