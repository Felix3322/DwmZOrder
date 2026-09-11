param([ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$cmakeCommand=Get-Command cmake -ErrorAction SilentlyContinue
$cmake=if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (!$cmake) {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $cmake=& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' | Select-Object -First 1
    }
}
if (!$cmake) { throw 'Install Visual Studio 2022 / Build Tools with C++ desktop tools, Windows SDK and CMake.' }
$build=Join-Path $PSScriptRoot 'build'
$sourceInputs=@()
$manifestPath=Join-Path $PSScriptRoot 'source-manifest.json'
if (Test-Path -LiteralPath $manifestPath) {
    $sourceManifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach ($entry in $sourceManifest) {
        if ($entry.path -match '(\.(cpp|h|asm|rc|manifest)$|CMakeLists\.txt$)') {
            $sourceInputs+=@{path=$entry.path;sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $entry.path) -Algorithm SHA256).Hash}
        }
    }
}
& $cmake -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
& $cmake --build $build --config $Configuration --parallel 2
if ($LASTEXITCODE) { throw "Build failed: $LASTEXITCODE" }
$bin=Join-Path $build "bin/$Configuration"
foreach ($name in @('DwmOrderTool.exe','DwmZOrder.dll')) {
    $file=Get-Item -LiteralPath (Join-Path $bin $name)
    if ($file.Length -eq 0) { throw "Empty output: $name" }
}
foreach ($inputFile in $sourceInputs) {
    if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $inputFile.path) -Algorithm SHA256).Hash -ne $inputFile.sha256) { throw "Source changed during build: $($inputFile.path)" }
}
$outputHashes=@(foreach ($name in @('DwmOrderTool.exe','DwmZOrder.dll')) { @{path=$name;sha256=(Get-FileHash -LiteralPath (Join-Path $bin $name) -Algorithm SHA256).Hash} })
@{configuration=$Configuration;inputs=$sourceInputs;outputs=$outputHashes;buildVerified=$true} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build "$Configuration-receipt.json") -Encoding UTF8
Write-Output "BUILD=SUCCESS"
Write-Output "BIN=$bin"
