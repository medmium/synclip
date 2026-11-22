[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$WindowsIp,
    [string]$PhoneSerial,
    [string]$ConfigPath = (Join-Path $env:LOCALAPPDATA 'SynClip\config.ini')
)

$ErrorActionPreference = 'Stop'
$ip = $null
if (-not [System.Net.IPAddress]::TryParse($WindowsIp, [ref]$ip) -or
    $ip.Equals([System.Net.IPAddress]::Any) -or $ip.Equals([System.Net.IPAddress]::IPv6Any)) {
    throw 'Specify the Windows IP address the phone can reach (for example 192.168.1.230).'
}
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
    throw 'Start SynClip.exe once to generate config.ini, or specify -ConfigPath.'
}

$key = $null
$port = 45873
foreach ($line in Get-Content -LiteralPath $ConfigPath) {
    if ($line -cmatch '^\s*key\s*=\s*([0-9A-Fa-f]{64})\s*$') {
        $key = $Matches[1]
    } elseif ($line -cmatch '^\s*port\s*=\s*([0-9]{1,5})\s*$') {
        $port = [int]$Matches[1]
    }
}
if (-not $key -or $key -match '^0+$' -or $port -lt 1 -or $port -gt 65535) {
    throw 'config.ini must contain a valid pairing key and TCP port.'
}

$adb = (Get-Command adb -CommandType Application -ErrorAction Stop).Source
$target = @()
if ($PhoneSerial) {
    $target = @('-s', $PhoneSerial)
}

$directory = '/data/adb/synclip'
$temporary = "$directory/.config-$([Guid]::NewGuid().ToString('N'))"
$command = "umask 077; mkdir -p $directory && chmod 0700 $directory && cat > $temporary && chmod 0600 $temporary && mv -f $temporary $directory/config; result=`$?; rm -f $temporary; exit `$result"
@("host=$ip", "port=$port", "key=$key") | & $adb @target shell -T "su -c '$command'"
if ($LASTEXITCODE -ne 0) {
    throw 'ADB could not save the config. Verify the selected phone is connected and adb shell has root authorization.'
}

Write-Output 'Pairing saved. Reboot the phone if SynClip was already running with a different config.'
