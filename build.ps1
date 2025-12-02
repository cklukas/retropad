$ErrorActionPreference = "Stop"

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
    if ($LASTEXITCODE -eq 0) { exit 0 }

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
    exit $LASTEXITCODE
}

Write-Error "No usable MSVC build environment found. Install Visual Studio (Desktop development with C++) or Build Tools."
exit 1
