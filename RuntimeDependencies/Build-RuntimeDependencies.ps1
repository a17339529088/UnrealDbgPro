param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$msbuildCommand = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
if ($msbuildCommand) {
    $msbuild = $msbuildCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with MSVC v142 and Windows SDK first.' }
    $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild was not found.' }
}
foreach ($project in 'AIHelper\AIHelper.vcxproj','Encryption\Encryption.vcxproj','TestRuntime.vcxproj') {
    & $msbuild (Join-Path $PSScriptRoot $project) /p:Configuration=$Configuration /p:Platform=x64 /m /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project" }
}
$outputDirectory = Join-Path $repoRoot "x64\$Configuration\bin"
& (Join-Path $outputDirectory 'TestRuntime.exe') $outputDirectory
if ($LASTEXITCODE -ne 0) { throw "Runtime dependency tests failed: $LASTEXITCODE" }
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $outputDirectory 'AIHelper.dll'),(Join-Path $outputDirectory 'D-encryption.dll')
