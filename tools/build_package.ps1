[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$')]
    [string]$Version,

    [string]$BuildDirectory = 'cmake-build-package',
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot

function Resolve-ProjectPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'JetBrains\CLion\bin\cmake\win\x64\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin\cmake\win\x64\bin\cmake.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'CMake was not found. Install CLion/CMake or add cmake.exe to PATH.'
}

function Find-VcVars64 {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installation) {
            $candidate = Join-Path ($installation | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
        $candidate = Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'Visual Studio 2022 C++ build tools were not found.'
}

function Import-VcVarsEnvironment([string]$VcVarsPath) {
    $command = "call `"$VcVarsPath`" >nul && set"
    $environmentLines = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $environmentLines) {
        if ($line.StartsWith('=')) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Executable $($Arguments -join ' ')"
    }
}

function Find-BuildFile([string]$BuildRoot, [string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildRoot $Name),
        (Join-Path (Join-Path $BuildRoot 'Release') $Name))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "$Name was not produced in $BuildRoot."
}

$cmake = Find-CMake
$vcVars64 = Find-VcVars64
$buildRoot = Resolve-ProjectPath $BuildDirectory
$outputRoot = Resolve-ProjectPath $OutputDirectory

Import-VcVarsEnvironment $vcVars64
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

Write-Host "Building LAN Speak $Version"
Invoke-Checked $cmake @(
    '-S', $projectRoot,
    '-B', $buildRoot,
    "-DLANSPEAK_VERSION_OVERRIDE=$Version"
)
Invoke-Checked $cmake @('--build', $buildRoot, '--config', 'Release')

$guiExe = Find-BuildFile $buildRoot 'LanSpeak.exe'
$coreExe = Find-BuildFile $buildRoot 'LanSpeakCore.exe'
$testsExe = Find-BuildFile $buildRoot 'LanSpeakTests.exe'

Invoke-Checked $testsExe @()

foreach ($executable in @($guiExe, $coreExe)) {
    $embeddedVersion = (Get-Item -LiteralPath $executable).VersionInfo.FileVersion
    if ($embeddedVersion -ne $Version) {
        throw "Version mismatch in ${executable}: expected $Version, found $embeddedVersion."
    }
}

Copy-Item -LiteralPath $guiExe -Destination (Join-Path $outputRoot 'LanSpeak.exe') -Force
Copy-Item -LiteralPath $coreExe -Destination (Join-Path $outputRoot 'LanSpeakCore.exe') -Force

$archiveBaseName = "LanSpeak-$Version-win-x64"
$stagingRoot = Join-Path $buildRoot 'package'
$packageRoot = Join-Path $stagingRoot $archiveBaseName
$archivePath = Join-Path $outputRoot "$archiveBaseName.zip"

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

try {
    Copy-Item -LiteralPath $guiExe -Destination $packageRoot
    Copy-Item -LiteralPath $coreExe -Destination $packageRoot
    foreach ($file in @('LICENSE', 'README.md', 'README_RU.md', 'CHANGELOG.md', 'CHANGELOG_RU.md')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $packageRoot
    }

    Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $archivePath -CompressionLevel Optimal -Force
} finally {
    if (Test-Path -LiteralPath $packageRoot) {
        Remove-Item -LiteralPath $packageRoot -Recurse -Force
    }
}

$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
Write-Host "Package: $archivePath"
Write-Host "SHA-256: $hash"
