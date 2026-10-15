param(
    [ValidateRange(10, 3600)][int]$SmokeTimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# This entry builds the existing GUI project and its project references.
# Smoke mode needs an interactive desktop and elevation (the EXE manifest
# requests administrator). It does not certify VMX/driver runtime behavior.
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an administrator terminal: the GUI manifest requires elevation.'
    }
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $project = Join-Path $PSScriptRoot 'UnrealDbgProNativeWinUI.vcxproj'
    $outputDirectory = Join-Path $repoRoot 'x64\WinUI\Release'
    $executable = Join-Path $outputDirectory 'UnrealDbgProNativeWinUI.exe'
    $msbuild = $null
    $command = Get-Command MSBuild.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $msbuild = $command.Source }
    if (-not $msbuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
            throw 'MSBuild/vswhere not found. Install the C++ build tools, v142, matching SDK and WDK.'
        }
        $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($LASTEXITCODE -ne 0 -or -not $msbuild) { throw 'vswhere could not locate MSBuild.' }
    }
    if (-not (Test-Path -LiteralPath $project -PathType Leaf)) { throw "Missing project: $project" }

    Write-Host '[1/4] Restore WinUI dependencies'
    # Separate process preserves this script's error handling and exit status.
    $powershell = Join-Path $PSHOME 'powershell.exe'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'RestoreWinUiPackages.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Dependency restore failed: $LASTEXITCODE" }

    Write-Host '[2/4] Build GUI and project references (Release/x64)'
    $logDirectory = Join-Path $repoRoot '_artifacts\gui-validation'
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $buildLog = Join-Path $logDirectory 'gui-release.binlog'
    Push-Location -LiteralPath $repoRoot
    try {
        & $msbuild $project '/t:Build' '/p:Configuration=Release' '/p:Platform=x64' '/m' '/v:minimal' "/bl:$buildLog"
        $buildExitCode = $LASTEXITCODE
    } finally { Pop-Location }
    if ($buildExitCode -ne 0) { throw "MSBuild failed: $buildExitCode. Log: $buildLog" }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "MSBuild returned success but GUI executable is missing: $executable"
    }

    Write-Host '[3/4] Validate runtime dependency libraries'
    & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'RuntimeDependencies\Build-RuntimeDependencies.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw "Runtime dependency validation failed: $LASTEXITCODE" }

    Write-Host '[4/4] Run GUI smoke test (requires Windows App Runtime 1.8 x64)'
    $process = Start-Process -FilePath $executable -ArgumentList '--ui-smoke-test' -WorkingDirectory $outputDirectory -PassThru
    try {
        if (-not $process.WaitForExit($SmokeTimeoutSeconds * 1000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "GUI smoke test timed out after $SmokeTimeoutSeconds seconds."
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0) { throw "GUI smoke test failed: $($process.ExitCode)" }
    } finally { $process.Dispose() }
    Write-Host "PASS: build, runtime dependency validation and GUI smoke test. Build log: $buildLog"
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
