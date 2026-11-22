[CmdletBinding()]
param(
    [string] $SdkRoot,
    [string] $NativeRoot,
    [string] $MbedTlsLicense,
    [string] $Output,
    [ValidateSet('arm64-v8a', 'x86_64')]
    [string[]] $Abis = @('arm64-v8a', 'x86_64')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$project = Split-Path -Parent $PSScriptRoot
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
if (-not $NativeRoot) { $NativeRoot = Join-Path $project 'build/android' }
if (-not $Output) { $Output = Join-Path $project 'build/pair-app/synclip-pair.apk' }
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$Abis = @($Abis | Select-Object -Unique)
if ($Abis.Count -eq 0) { throw 'Pass at least one Android ABI.' }
if (-not $MbedTlsLicense) { $MbedTlsLicense = Join-Path $NativeRoot "$($Abis[0])/_deps/mbedtls-src/LICENSE" }
if (-not (Test-Path -LiteralPath $MbedTlsLicense -PathType Leaf)) {
    throw 'Mbed TLS LICENSE missing. Build the Android native targets first or pass -MbedTlsLicense.'
}
$MbedTlsLicense = (Resolve-Path -LiteralPath $MbedTlsLicense).Path

function Invoke-Checked {
    param([string] $Tool, [string[]] $ToolArguments)
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Tool) failed with exit code $LASTEXITCODE."
    }
}

function Copy-ArchiveEntry {
    param($Source, $Destination, [string] $Name)
    $entry = $Destination.CreateEntry($Name)
    $inputStream = $Source.Open()
    try {
        $outputStream = $entry.Open()
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally {
        $inputStream.Dispose()
    }
}

$tools = Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'build-tools') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and [version] $_.Name -ge [version] '35.0.0' } |
    Sort-Object { [version] $_.Name } -Descending | Select-Object -First 1
if (-not $tools) { throw 'Install Android SDK Build Tools 35 or later.' }
$androidJar = Join-Path $SdkRoot 'platforms/android-34/android.jar'
if (-not (Test-Path -LiteralPath $androidJar -PathType Leaf)) {
    throw 'Install Android SDK Platform 34 (sdkmanager "platforms;android-34").'
}
$javaBin = if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME 'bin' } else {
    Split-Path -Parent (Get-Command javac.exe -ErrorAction Stop).Source
}
$source = Join-Path $project 'android/pair'
$work = Join-Path $project 'build/pair-app'
$downloads = Join-Path $project 'build/downloads'
New-Item -ItemType Directory -Force -Path $work, $downloads, (Split-Path -Parent $Output) | Out-Null
$stage = Join-Path $work ('.apk-' + [guid]::NewGuid().ToString('N'))
$classes = Join-Path $stage 'classes'
$dex = Join-Path $stage 'dex'
New-Item -ItemType Directory -Path $classes, $dex | Out-Null

try {
    $zxing = Join-Path $downloads 'zxing-core-3.5.4.jar'
    $hash = '71de5d89341b5fcf5dd89da7f44e84d825d0e084cdf3ec77c9abe26b0f0ceb13'
    if (-not (Test-Path -LiteralPath $zxing -PathType Leaf)) {
        $download = Join-Path $stage 'zxing.jar'
        $url = 'https://repo.maven.apache.org/maven2/com/google/zxing/core/3.5.4/core-3.5.4.jar'
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $download
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $hash) {
            throw 'ZXing download checksum mismatch.'
        }
        Move-Item -LiteralPath $download -Destination $zxing -Force
    }
    if ((Get-FileHash -LiteralPath $zxing -Algorithm SHA256).Hash -ne $hash) {
        throw "Unexpected ZXing checksum for $zxing."
    }

    $appJar = Join-Path $stage 'app.jar'
    $sources = @(Get-ChildItem -LiteralPath (Join-Path $source 'java') -Recurse -File -Filter '*.java')
    Invoke-Checked (Join-Path $javaBin 'javac.exe') (@('--release', '8', '-Xlint:-options',
        '-encoding', 'UTF-8', '-classpath', "$androidJar;$zxing", '-d', $classes) + @($sources.FullName))
    Invoke-Checked (Join-Path $javaBin 'jar.exe') @('cf', $appJar, '-C', $classes, '.')
    Invoke-Checked (Join-Path $tools.FullName 'd8.bat') @('--release', '--min-api', '29',
        '--lib', $androidJar, '--output', $dex, $appJar, $zxing)

    $unsigned = Join-Path $stage 'unsigned.apk'
    $aligned = Join-Path $stage 'aligned.apk'
    $signed = Join-Path $stage 'signed.apk'
    Invoke-Checked (Join-Path $tools.FullName 'aapt2.exe') @('link', '-I', $androidJar,
        '--manifest', (Join-Path $source 'AndroidManifest.xml'), '-o', $unsigned)

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $apk = [IO.Compression.ZipFile]::Open($unsigned, [IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $dex -File -Filter '*.dex') {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($apk, $file.FullName, $file.Name) | Out-Null
        }
        foreach ($abi in $Abis) {
            $library = Join-Path $NativeRoot "$abi/bin/libsynclip_pair_jni.so"
            if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
                $library = Join-Path $NativeRoot "$abi/libsynclip_pair_jni.so"
            }
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $apk, $library, "lib/$abi/libsynclip_pair_jni.so") | Out-Null
        }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $apk, $MbedTlsLicense, 'licenses/mbedtls.txt') | Out-Null
        $zxingArchive = [IO.Compression.ZipFile]::OpenRead($zxing)
        try {
            $license = $zxingArchive.Entries | Where-Object { $_.Name -match '^LICENSE(\.txt)?$' } |
                Select-Object -First 1
            if ($license) {
                Copy-ArchiveEntry $license $apk 'licenses/zxing.txt'
            } else {
                $licensePath = Join-Path $stage 'zxing-license.txt'
                $url = 'https://raw.githubusercontent.com/zxing/zxing/zxing-3.5.4/LICENSE'
                Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $licensePath
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $apk, $licensePath, 'licenses/zxing.txt') | Out-Null
            }
            $notice = $zxingArchive.Entries | Where-Object { $_.Name -match '^NOTICE(\.txt)?$' } |
                Select-Object -First 1
            if ($notice) { Copy-ArchiveEntry $notice $apk 'licenses/zxing-notice.txt' }
        } finally {
            $zxingArchive.Dispose()
        }
    } finally {
        $apk.Dispose()
    }

    $keystore = Join-Path $work 'synclip-development.p12'
    if (-not (Test-Path -LiteralPath $keystore -PathType Leaf)) {
        Invoke-Checked (Join-Path $javaBin 'keytool.exe') @('-genkeypair', '-keystore', $keystore,
            '-storetype', 'PKCS12', '-alias', 'synclip-development', '-storepass', 'android',
            '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
            '-dname', 'CN=SynClip Development')
    }
    $zipalign = Join-Path $tools.FullName 'zipalign.exe'
    $apksigner = Join-Path $tools.FullName 'apksigner.bat'
    Invoke-Checked $zipalign @('-f', '-P', '16', '4', $unsigned, $aligned)
    Invoke-Checked $apksigner @('sign', '--ks', $keystore, '--ks-key-alias', 'synclip-development',
        '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--v4-signing-enabled', 'false',
        '--out', $signed, $aligned)
    Invoke-Checked $apksigner @('verify', '--verbose', '--min-sdk-version', '29', $signed)
    Invoke-Checked $zipalign @('-c', '-P', '16', '4', $signed)
    Move-Item -LiteralPath $signed -Destination $Output -Force
    Write-Output "Built $Output"
} finally {
    if ($stage.StartsWith($work + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
