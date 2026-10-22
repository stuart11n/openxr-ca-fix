# Configure and build the layer with the VS 2022 generator. Output: build\Release\
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build "$root\build" --config $Config
exit $LASTEXITCODE
