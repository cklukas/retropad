$ErrorActionPreference = "Stop"

# Default WinAppSDK dir (matches Makefile) unless caller overrides.
if (-not $env:WINAPPSDK_DIR) {
    $defaultWinUi = Join-Path -Path $PSScriptRoot -ChildPath "microsoft.windowsappsdk.winui.1.8.251105000"
    if (Test-Path $defaultWinUi) {
        $env:WINAPPSDK_DIR = $defaultWinUi
    }
}

$winuiHeader = Join-Path -Path $env:WINAPPSDK_DIR -ChildPath "include\winrt\Windows.UI.Xaml.Hosting.h"
if (-not (Test-Path $winuiHeader)) {
    Write-Error "WinUI headers not found. Set WINAPPSDK_DIR to the WinAppSDK WinUI 1.8 package root (expected: include\winrt\Windows.UI.Xaml.Hosting.h). Current WINAPPSDK_DIR='$($env:WINAPPSDK_DIR)'."
    exit 1
}

$binDir = Join-Path -Path $PSScriptRoot -ChildPath "binaries"
$winuiRuntimeDir = Join-Path -Path $env:WINAPPSDK_DIR -ChildPath "runtimes-framework\win-x64\native"
$foundationRoot = Join-Path -Path $PSScriptRoot -ChildPath "microsoft.windowsappsdk.foundation.1.8.251104000"
$foundationBootstrapDir = Join-Path -Path $foundationRoot -ChildPath "runtimes\win-x64\native"
$foundationRuntimeDir = Join-Path -Path $foundationRoot -ChildPath "runtimes-framework\win-x64\native"

if (-not (Test-Path $winuiRuntimeDir)) {
    Write-Error "WinUI runtime binaries not found at '$winuiRuntimeDir'. Ensure the WinUI 1.8 package is present (microsoft.windowsappsdk.winui.1.8.x)."
    exit 1
}
if (-not (Test-Path $foundationBootstrapDir) -or -not (Test-Path $foundationRuntimeDir)) {
    Write-Error "WinAppSDK foundation runtimes not found (expected under '$foundationRoot'). Extract microsoft.windowsappsdk.foundation.1.8.x into the repo."
    exit 1
}

function Copy-RuntimeBinaries {
    if (-not (Test-Path $binDir)) { New-Item -ItemType Directory -Path $binDir | Out-Null }
    $copySets = @(
        @{ Source = $foundationBootstrapDir; Filter = "*.dll" },
        @{ Source = $foundationRuntimeDir;   Filter = "*.dll" },
        @{ Source = $foundationRuntimeDir;   Filter = "*.pri"  },
        @{ Source = $winuiRuntimeDir;        Filter = "*.dll"  }
    )
    foreach ($set in $copySets) {
        Get-ChildItem -Path $set.Source -Filter $set.Filter -File -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName -Destination $binDir -Force }
    }
}

# Try a few known Visual Studio toolchain setups and run nmake.
$candidates = @(
    @{ Path = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"; Args = "-arch=amd64" },
    @{ Path = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"; Args = "" },
    @{ Path = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"; Args = "-arch=amd64" },
    @{ Path = "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"; Args = "-arch=amd64" },
    @{ Path = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"; Args = "-arch=amd64" },
    @{ Path = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat"; Args = "-arch=amd64" },
    @{ Path = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"; Args = "" },
    @{ Path = "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"; Args = "" }
)

foreach ($cand in $candidates) {
    if (-not (Test-Path $cand.Path)) { continue }

    Write-Host "Using $($cand.Path)"
    $callPart = if ([string]::IsNullOrWhiteSpace($cand.Args)) {
        "call `"$($cand.Path)`""
    } else {
        "call `"$($cand.Path)`" $($cand.Args)"
    }

    $cmdLine = "$callPart && nmake /f makefile"
    & cmd.exe /c $cmdLine
    if ($LASTEXITCODE -eq 0) {
        Copy-RuntimeBinaries
        exit 0
    }

    if ($LASTEXITCODE -eq 2 -or $LASTEXITCODE -eq 1) {
        # Common linker failure when retropad.exe is locked/running
        $locked = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "retropad" }
        if ($locked) {
            Write-Warning "retropad.exe appears to be running/locked. Close it and rerun build.ps1."
        }
    }
    Write-Warning "Build failed with $($cand.Path) (exit $LASTEXITCODE). Trying next option..."
}

# Fall back to current shell if nmake is already available.
if (Get-Command nmake -ErrorAction SilentlyContinue) {
    Write-Host "Using existing environment (nmake already on PATH)"
    nmake /f makefile
    if ($LASTEXITCODE -eq 0) {
        Copy-RuntimeBinaries
    }
    exit $LASTEXITCODE
}

Write-Error "No usable MSVC build environment found. Install Visual Studio (Desktop development with C++) or Build Tools."
exit 1
