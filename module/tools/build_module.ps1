<#
	Builds gmcl_rcx_win64.dll (MSVC, through the CMake that Visual Studio bundles) into
	module/builds/win64/ (gitignored), with openvr_api.dll next to it, then runs the module's own
	tests (the IK solver, the VR input files). Without -Install it never writes garrysmod/lua/bin,
	so it works while a game has the module loaded.
	Run through build_module.bat, or: powershell -ExecutionPolicy Bypass -File tools/build_module.ps1

	  builds/win64/      gmcl_rcx_win64.dll, openvr_api.dll, the test exes
	  builds/cmake-win/  the CMake tree

	-Install    then copy both DLLs into garrysmod/lua/bin, first keeping the installed ones in
	            lua/bin/rcx_previous/; refused while a running game has the module loaded
	-Rollback   put lua/bin/rcx_previous/ back into lua/bin (no build); refused the same way
	-Clean      delete the CMake tree first
	-Config     Release (default) or Debug
#>
param(
	[switch]$Install,
	[switch]$Rollback,
	[switch]$Clean,
	[ValidateSet("Release", "Debug")][string]$Config = "Release"
)
# Not "Stop": Windows PowerShell 5.1 turns a native tool's redirected stderr into terminating errors.
$ErrorActionPreference = "Continue"

$moduleDir = Split-Path -Parent $PSScriptRoot
$buildsDir = Join-Path $moduleDir "builds"
$outDir = Join-Path $buildsDir "win64"
$cmakeDir = Join-Path $buildsDir "cmake-win"
$luaBin = [System.IO.Path]::GetFullPath((Join-Path $moduleDir "..\..\..\lua\bin"))
$previousDir = Join-Path $luaBin "rcx_previous"
$moduleName = "gmcl_rcx_win64.dll"
$installed = @($moduleName, "openvr_api.dll")

function Find-CMake {
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	if (Test-Path $vswhere) {
		$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
		if ($vsPath) {
			$bundled = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
			if (Test-Path $bundled) { return $bundled }
		}
	}
	$onPath = Get-Command cmake -ErrorAction SilentlyContinue
	if ($onPath) { return $onPath.Source }
	throw "CMake not found: install Visual Studio with the C++ workload (it bundles CMake)"
}

# The version kVersion in src/module.cpp names ("0.55"), or "?".
function Get-SourceVersion {
	$line = Select-String -Path (Join-Path $moduleDir "src\module.cpp") -Pattern 'kVersion = "rcx ([0-9.]+)' | Select-Object -First 1
	if ($line) { return $line.Matches[0].Groups[1].Value }
	return "?"
}

# The game processes that have the installed module loaded, as "gmod.exe (pid N)" strings.
function Get-ModuleUsers {
	$target = (Join-Path $luaBin $moduleName).ToLowerInvariant()
	$users = @()
	foreach ($process in Get-Process -Name gmod, hl2, srcds -ErrorAction SilentlyContinue) {
		try {
			$loaded = $process.Modules | Where-Object { $_.FileName -and $_.FileName.ToLowerInvariant() -eq $target }
			if ($loaded) { $users += "$($process.ProcessName).exe (pid $($process.Id))" }
		} catch {
			$users += "$($process.ProcessName).exe (pid $($process.Id), modules unreadable)"
		}
	}
	return $users
}

# Refuses (returns $false) while a game has the module loaded or the file is locked.
function Test-CanReplace {
	$users = @(Get-ModuleUsers)
	if ($users.Count -gt 0) {
		Write-Host "FAILED: the installed module is loaded by $($users -join ', '); close that game first (a session never closes the maintainer's game)" -ForegroundColor Red
		return $false
	}
	$path = Join-Path $luaBin $moduleName
	if (Test-Path $path) {
		try {
			$stream = [System.IO.File]::Open($path, "Open", "ReadWrite", "None")
			$stream.Close()
		} catch {
			Write-Host "FAILED: $path is locked: $($_.Exception.Message)" -ForegroundColor Red
			return $false
		}
	}
	return $true
}

function Copy-Files([string]$from, [string]$to) {
	foreach ($name in $installed) {
		$source = Join-Path $from $name
		if (-not (Test-Path $source)) { continue }
		try {
			Copy-Item $source $to -Force -ErrorAction Stop
			Write-Host "copied $name to $to"
		} catch {
			Write-Host "FAILED: can't copy $name into ${to}: $($_.Exception.Message)" -ForegroundColor Red
			return $false
		}
	}
	return $true
}

function Show-Files([string]$dir) {
	if (-not (Test-Path $dir)) { return }
	foreach ($name in $installed) {
		$file = Join-Path $dir $name
		if (Test-Path $file) {
			$item = Get-Item $file
			Write-Host ("{0,-60} {1,10:N0} bytes  {2}" -f $item.FullName, $item.Length, $item.LastWriteTime)
		}
	}
}

if ($Rollback) {
	Write-Host "== Rollback from $previousDir" -ForegroundColor Cyan
	if (-not (Test-Path (Join-Path $previousDir $moduleName))) {
		Write-Host "FAILED: no previous module in $previousDir" -ForegroundColor Red
		exit 1
	}
	if (-not (Test-CanReplace)) { exit 1 }
	if (-not (Copy-Files $previousDir $luaBin)) { exit 1 }
	Show-Files $luaBin
	Write-Host "OK: rolled back (the build that was installed is still in builds/win64)" -ForegroundColor Green
	exit 0
}

$version = Get-SourceVersion
Write-Host "== Windows (MSVC), rcx $version, $Config" -ForegroundColor Cyan
$cmake = Find-CMake
if ($Clean -and (Test-Path $cmakeDir)) { Remove-Item -Recurse -Force $cmakeDir }

& $cmake -S $moduleDir -B $cmakeDir -A x64 "-DRCX_OUTPUT_DIR=$outDir"
if ($LASTEXITCODE -eq 0) { & $cmake --build $cmakeDir --config $Config --parallel }
if ($LASTEXITCODE -ne 0) {
	Write-Host "FAILED: build" -ForegroundColor Red
	exit 1
}
Get-ChildItem $outDir -Include "*.exp", "*.lib" -Recurse | Remove-Item -Force
Write-Host "built $outDir\$moduleName" -ForegroundColor Green

# The IK solver's unit tests run on every build; a failure fails the build.
$ikTest = Join-Path $outDir "rcx_ik_test.exe"
if (Test-Path $ikTest) {
	& $ikTest
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_ik_test" -ForegroundColor Red
		exit 1
	}
}

# The VR input files, written without SteamVR and checked as JSON; a failure fails the build.
$testsDir = Join-Path $outDir "tests"
$inputTest = Join-Path $testsDir "rcx_input_files_test.exe"
if (Test-Path $inputTest) {
	& $inputTest
	if ($LASTEXITCODE -eq 0) { & python -I (Join-Path $PSScriptRoot "check_input_files.py") $testsDir }
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: the VR input files" -ForegroundColor Red
		exit 1
	}
}

# The engine safety layer against this machine's game DLLs (skipped where there is no game, as on CI); a failure fails the build.
$guardTest = Join-Path $testsDir "rcx_engine_guard_test.exe"
$gameBin = [System.IO.Path]::GetFullPath((Join-Path $moduleDir "..\..\..\..\bin\win64"))
if ((Test-Path $guardTest) -and (Test-Path (Join-Path $gameBin "engine.dll"))) {
	& $guardTest $gameBin
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_engine_guard_test" -ForegroundColor Red
		exit 1
	}
}

# The merged-mesh builder against the Python prototype's output, when the fixtures were written on this machine
# (tools/mesh_prototype/merge_cases.py --bake --fixtures builds/win64/tests/mesh_fixtures); a failure fails the build.
$meshTest = Join-Path $testsDir "rcx_mesh_merge_test.exe"
$meshFixtures = Join-Path $testsDir "mesh_fixtures"
if ((Test-Path $meshTest) -and (Test-Path $meshFixtures)) {
	& $meshTest $meshFixtures
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_mesh_merge_test" -ForegroundColor Red
		exit 1
	}
}

# rcx.sys's memory reading on this machine; a failure fails the build.
$sysTest = Join-Path $testsDir "rcx_sys_memory_test.exe"
if (Test-Path $sysTest) {
	& $sysTest
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_sys_memory_test" -ForegroundColor Red
		exit 1
	}
}

# The x64 detour on hand-written functions; a failure fails the build.
$detourTest = Join-Path $testsDir "rcx_detour_test.exe"
if (Test-Path $detourTest) {
	& $detourTest
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_detour_test" -ForegroundColor Red
		exit 1
	}
}

# VR-42: the VR bridge finds a device whose vtable an overlay copied; a failure fails the build.
$vptrTest = Join-Path $testsDir "rcx_bridge_vptr_test.exe"
if (Test-Path $vptrTest) {
	& $vptrTest
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_bridge_vptr_test" -ForegroundColor Red
		exit 1
	}
}

# The native path's Direct3D 9Ex to Direct3D 11 ordering without a CPU wait; a failure fails the build.
$syncTest = Join-Path $testsDir "rcx_shared_sync_test.exe"
if (Test-Path $syncTest) {
	& $syncTest
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_shared_sync_test" -ForegroundColor Red
		exit 1
	}
}

# The D3D12 hand-off for a game device on D3D9On12; a failure fails the build.
$on12Test = Join-Path $testsDir "rcx_d3d9on12_test.exe"
if (Test-Path $on12Test) {
	& $on12Test
	if ($LASTEXITCODE -ne 0) {
		Write-Host "FAILED: rcx_d3d9on12_test" -ForegroundColor Red
		exit 1
	}
}

if ($Install) {
	Write-Host "== Install into $luaBin" -ForegroundColor Cyan
	if (-not (Test-CanReplace)) { exit 1 }
	if (Test-Path (Join-Path $luaBin $moduleName)) {
		New-Item -ItemType Directory -Force $previousDir | Out-Null
		Write-Host "keeping the installed module in $previousDir (build_module.bat -Rollback puts it back)"
		if (-not (Copy-Files $luaBin $previousDir)) { exit 1 }
	}
	if (-not (Copy-Files $outDir $luaBin)) { exit 1 }
}

Write-Host ""
Show-Files $outDir
if ($Install) { Show-Files $luaBin }
Write-Host "OK: rcx $version" -ForegroundColor Green
exit 0
