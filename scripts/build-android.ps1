[CmdletBinding()]
param(
    [string] $SdkRoot,
    [string] $NdkRoot,
    [string] $BuildRoot,
    [string] $Output,
    [ValidateRange(1, 128)]
    [int] $Jobs = 4,
    [ValidateSet('arm64-v8a', 'x86_64')]
    [string[]] $Abis = @('arm64-v8a', 'x86_64')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$project = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $project 'build/android' }
if (-not $Output) { $Output = Join-Path $project 'dist/synclip-android-v0.3.5.zip' }
$BuildRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildRoot)
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$Abis = @($Abis | Select-Object -Unique)
if ($Abis.Count -eq 0) { throw 'Pass at least one Android ABI.' }

function Invoke-Checked {
    param([string] $Tool, [string[]] $ToolArguments)
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Tool) failed with exit code $LASTEXITCODE."
    }
}

function Find-SdkTool {
    param([string] $Package, [string] $File)
    $best = Get-ChildItem -LiteralPath (Join-Path $SdkRoot $Package) -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName $File) -PathType Leaf) } |
        Sort-Object { [version] $_.Name } -Descending | Select-Object -First 1
    if (-not $best) { throw "Install Android SDK $Package providing $File." }
    Join-Path $best.FullName $File
}

if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) {
    throw 'Android SDK not found. Set ANDROID_HOME or pass -SdkRoot.'
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path

if (-not $NdkRoot) {
    $ndkToolchain = Find-SdkTool -Package 'ndk' -File 'build/cmake/android.toolchain.cmake'
    $NdkRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ndkToolchain))
}
$NdkRoot = (Resolve-Path -LiteralPath $NdkRoot).Path
$ndkRevision = Select-String -LiteralPath (Join-Path $NdkRoot 'source.properties') -Pattern '^Pkg\.Revision\s*=\s*(\d+)'
if (-not $ndkRevision -or [int] $ndkRevision.Matches[0].Groups[1].Value -lt 27) {
    throw 'Install Android NDK r27 or newer to build 16 KB page-compatible binaries.'
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { Find-SdkTool -Package 'cmake' -File 'bin/cmake.exe' }
$ninjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
$ninja = if ($ninjaCommand) { $ninjaCommand.Source } else { Find-SdkTool -Package 'cmake' -File 'bin/ninja.exe' }
$d8 = Find-SdkTool -Package 'build-tools' -File 'd8.bat'
if ($env:JAVA_HOME) {
    $javac = Join-Path $env:JAVA_HOME 'bin/javac.exe'
    $jar = Join-Path $env:JAVA_HOME 'bin/jar.exe'
} else {
    $javac = (Get-Command javac.exe -ErrorAction Stop).Source
    $jar = (Get-Command jar.exe -ErrorAction Stop).Source
}
$androidJar = Join-Path $SdkRoot 'platforms/android-29/android.jar'
if (-not (Test-Path -LiteralPath $androidJar -PathType Leaf)) {
    throw 'Install Android SDK Platform 29 (sdkmanager "platforms;android-29").'
}

New-Item -ItemType Directory -Force -Path $BuildRoot, (Split-Path -Parent $Output) | Out-Null
$buildPath = (Resolve-Path -LiteralPath $BuildRoot).Path.TrimEnd('\', '/')
$stage = Join-Path $buildPath ('.package-' + [guid]::NewGuid().ToString('N'))
$zipTemp = "$Output.$([guid]::NewGuid().ToString('N')).tmp"
New-Item -ItemType Directory -Path (Join-Path $stage 'classes') | Out-Null

try {
    $classDir = Join-Path $stage 'classes'
    $classJar = Join-Path $stage 'classes.jar'
    $dexJar = Join-Path $stage 'synclip.jar'
    $javaSources = Get-ChildItem -LiteralPath (Join-Path $project 'android/java') -Recurse -File -Filter '*.java'
    if (-not $javaSources) { throw 'Android Java bootstrap source is missing.' }
    Invoke-Checked -Tool $javac -ToolArguments (@('--release', '8', '-Xlint:-options', '-encoding', 'UTF-8', '-classpath', $androidJar, '-d', $classDir) + @($javaSources.FullName))
    Invoke-Checked -Tool $jar -ToolArguments @('cf', $classJar, '-C', $classDir, '.')
    Invoke-Checked -Tool $d8 -ToolArguments @('--release', '--min-api', '29', '--lib', $androidJar, '--output', $dexJar, $classJar)

    $moduleFiles = [ordered] @{
        'payload/synclip.jar' = $dexJar
    }
    foreach ($abi in $Abis) {
        $abiBuild = Join-Path $buildPath $abi
        $nativeBin = Join-Path $abiBuild 'bin'
        Invoke-Checked -Tool $cmake -ToolArguments @(
            '-S', $project, '-B', $abiBuild, '-G', 'Ninja',
            "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake')",
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$nativeBin", "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$nativeBin",
            "-DANDROID_ABI=$abi", '-DANDROID_PLATFORM=android-29',
            '-DANDROID_STL=c++_static', '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON'
        )
        Invoke-Checked -Tool $cmake -ToolArguments @('--build', $abiBuild, '--parallel', "$Jobs", '--target', 'synclip-android-launcher', 'synclip_android', 'synclip_pair_jni')
        $moduleFiles["payload/$abi/synclip-android-launcher"] = Join-Path $nativeBin 'synclip-android-launcher'
        $moduleFiles["payload/$abi/libsynclip_android.so"] = Join-Path $nativeBin 'libsynclip_android.so'
        $mbedTlsLicense = Join-Path $abiBuild '_deps/mbedtls-src/LICENSE'
    }
    $moduleFiles['licenses/mbedtls.txt'] = $mbedTlsLicense

    $companionApk = Join-Path $stage 'synclip-pair.apk'
    & (Join-Path $PSScriptRoot 'build-pair-app.ps1') -SdkRoot $SdkRoot -NativeRoot $buildPath -MbedTlsLicense $mbedTlsLicense -Output $companionApk -Abis $Abis
    $manifestPath = Join-Path $project 'android/pair/AndroidManifest.xml'
    $manifest = [xml] [IO.File]::ReadAllText($manifestPath)
    $companionVersion = $manifest.DocumentElement.GetAttribute('versionCode', 'http://schemas.android.com/apk/res/android')
    if ($companionVersion -notmatch '^[1-9][0-9]*$') { throw 'Invalid Android companion versionCode.' }
    $versionFile = Join-Path $stage 'synclip-pair.version'
    [IO.File]::WriteAllText($versionFile, $companionVersion + [char] 10, [Text.Encoding]::ASCII)
    $moduleFiles['payload/synclip-pair.apk'] = $companionApk
    $moduleFiles['payload/synclip-pair.version'] = $versionFile

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipStream = [IO.File]::Open($zipTemp, [IO.FileMode]::CreateNew)
    try {
        $zip = [IO.Compression.ZipArchive]::new($zipStream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($name in @('module.prop', 'customize.sh', 'service.sh', 'action.sh', 'companion.sh', 'uninstall.sh')) {
                $entry = $zip.CreateEntry($name)
                $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
                try {
                    $contents = [IO.File]::ReadAllText((Join-Path $project "magisk/$name"))
                    $writer.Write($contents.Replace("`r`n", "`n"))
                } finally {
                    $writer.Dispose()
                }
            }
            foreach ($file in $moduleFiles.GetEnumerator()) {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.Value, $file.Key) | Out-Null
            }
        } finally {
            $zip.Dispose()
        }
    } finally {
        $zipStream.Dispose()
    }
    Move-Item -LiteralPath $zipTemp -Destination $Output -Force
    Write-Output "Built $Output"
} finally {
    if (Test-Path -LiteralPath $zipTemp -PathType Leaf) { Remove-Item -LiteralPath $zipTemp -Force }
    if ($stage.StartsWith($buildPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
