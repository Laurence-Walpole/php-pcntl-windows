<#
.SYNOPSIS
  Builds php_pcntl.dll for a given php.exe.

.DESCRIPTION
  Detects the PHP version, thread-safety, architecture and compiler from the
  target php.exe, downloads the matching PHP "devel pack" from windows.php.net
  (headers + import library) and compiles src\pcntl_win.c with MSVC.

  Requirements: Visual Studio 2017 or later with the "Desktop development with
  C++" workload (Build Tools edition is enough). Nothing else.

.EXAMPLE
  .\build\build.ps1                       # builds for the php.exe found on PATH
  .\build\build.ps1 -Php C:\php\php.exe   # builds for a specific PHP
#>
[CmdletBinding()]
param(
	[string]$Php = "php",
	[string]$DevelDir,
	[string]$Out,
	[switch]$DebugBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "src"
$develCache = Join-Path $PSScriptRoot "devel"

function Resolve-Php([string]$candidate) {
	if (Test-Path $candidate -PathType Leaf) { return (Resolve-Path $candidate).Path }
	$cmd = Get-Command $candidate -ErrorAction SilentlyContinue
	if (-not $cmd) { throw "php executable '$candidate' not found. Pass -Php C:\path\to\php.exe" }
	$path = $cmd.Source
	# Herd-style php.bat wrappers: follow them to the real php.exe
	if ($path -like "*.bat" -or $path -like "*.cmd") {
		$content = Get-Content $path -Raw
		if ($content -match '"([^"]+php\.exe)"') { return $Matches[1] }
	}
	return $path
}

function Get-PhpBuildInfo([string]$phpExe) {
	$probe = Join-Path $PSScriptRoot "probe.php"
	$ver = (& $phpExe -n -f $probe 2>$null)
	if (-not $ver) { throw "Could not run $phpExe" }
	$parts = "$ver".Trim().Split("|")
	$build = $parts[4]           # e.g. API20240924,NTS,VS17
	$compiler = ($build.Split(",") | Where-Object { $_ -match '^V[CS]\d+$' } | Select-Object -First 1)
	if (-not $compiler) { throw "Could not determine the compiler from build id '$build'" }
	[pscustomobject]@{
		Exe = $phpExe
		Version = $parts[0]
		Bits = [int]$parts[1]
		Ts = $parts[2]
		MajorMinor = $parts[3]
		BuildId = $build
		Compiler = $compiler.ToLower()
		Arch = if ([int]$parts[1] -eq 64) { "x64" } else { "x86" }
	}
}

function Get-DevelPack($info) {
	$name = "php-devel-pack-$($info.Version)-$($info.Ts)-Win32-$($info.Compiler)-$($info.Arch)"
	$dir = Join-Path $develCache $name
	if (Test-Path (Join-Path $dir "include\main\php.h")) { return $dir }
	# some packs extract to a folder without the "nts" marker; look for any php.h under a folder with the version
	$existing = Get-ChildItem $develCache -Directory -ErrorAction SilentlyContinue | Where-Object {
		$_.Name -like "php-$($info.Version)-devel-*$($info.Arch)" -and (Test-Path (Join-Path $_.FullName "include\main\php.h"))
	} | Select-Object -First 1
	if ($existing) { return $existing.FullName }

	New-Item -ItemType Directory -Force $develCache | Out-Null
	$zip = Join-Path $develCache "$name.zip"
	$urls = @(
		"https://windows.php.net/downloads/releases/$name.zip",
		"https://windows.php.net/downloads/releases/archives/$name.zip",
		"https://windows.php.net/downloads/qa/$name.zip",
		"https://windows.php.net/downloads/qa/archives/$name.zip"
	)
	$ok = $false
	foreach ($u in $urls) {
		try {
			Write-Host "Downloading $u"
			Invoke-WebRequest -Uri $u -OutFile $zip -UseBasicParsing -Headers @{ "User-Agent" = "pcntl-win-build" }
			$ok = $true
			break
		} catch { }
	}
	if (-not $ok) { throw "Could not download the PHP devel pack '$name.zip' from windows.php.net. Download it manually and pass -DevelDir." }
	$tmp = Join-Path $develCache "_extract"
	if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
	Expand-Archive -Path $zip -DestinationPath $tmp -Force
	$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	Move-Item $inner.FullName $dir
	Remove-Item $tmp -Recurse -Force
	Remove-Item $zip -Force
	return $dir
}

function Find-VcVars([string]$arch) {
	if ($env:PCNTL_VCVARS -and (Test-Path $env:PCNTL_VCVARS)) { return $env:PCNTL_VCVARS }
	$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path $vswhere)) { throw "Visual Studio (with the C++ workload) was not found. Install 'Build Tools for Visual Studio' and select 'Desktop development with C++'." }
	$vcvarsall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Auxiliary\Build\vcvarsall.bat" | Select-Object -First 1
	if (-not $vcvarsall) { throw "No Visual Studio installation with the C++ toolset was found." }
	return $vcvarsall
}

$phpExe = Resolve-Php $Php
$info = Get-PhpBuildInfo $phpExe
Write-Host "PHP $($info.Version) $($info.Ts.ToUpper()) $($info.Arch) ($($info.BuildId))"

if (-not $DevelDir) { $DevelDir = Get-DevelPack $info }
Write-Host "Devel pack: $DevelDir"

if (-not $Out) {
	$distDir = Join-Path $root "dist"
	New-Item -ItemType Directory -Force $distDir | Out-Null
	$Out = Join-Path $distDir "php_pcntl-$($info.MajorMinor)-$($info.Ts)-$($info.Arch).dll"
}

$vcvars = Find-VcVars $info.Arch
$objDir = Join-Path $PSScriptRoot "obj\$($info.MajorMinor)-$($info.Ts)-$($info.Arch)"
New-Item -ItemType Directory -Force $objDir | Out-Null

$defines = @(
	"/D _WINDOWS", "/D WINDOWS=1", "/D ZEND_WIN32=1", "/D PHP_WIN32=1", "/D WIN32", "/D _MBCS", "/D _USE_MATH_DEFINES",
	"/D ZEND_COMPILE_DL_EXT=1", "/D COMPILE_DL_PCNTL", "/D PCNTL_EXPORTS=1", "/D _CRT_SECURE_NO_WARNINGS", "/D _CRT_NONSTDC_NO_DEPRECATE"
)
if ($info.Ts -eq "ts") { $defines += "/D ZTS=1"; $defines += "/D ZEND_ENABLE_STATIC_TSRMLS_CACHE=1" }
if ($DebugBuild) { $defines += @("/D _DEBUG", "/D ZEND_DEBUG=1"); $opt = "/Od /Zi /MDd" } else { $defines += @("/D NDEBUG", "/D ZEND_DEBUG=0"); $opt = "/O2 /Oi /GF /MD /Zi" }

$includes = @("include", "include\main", "include\Zend", "include\TSRM", "include\win32") | ForEach-Object { "/I `"$(Join-Path $DevelDir $_)`"" }
$lib = Join-Path $DevelDir "lib\php8.lib"
if (-not (Test-Path $lib)) { $lib = Get-ChildItem (Join-Path $DevelDir "lib") -Filter "php*.lib" | Where-Object { $_.Name -match '^php\d+\.lib$' } | Select-Object -First 1 -ExpandProperty FullName }
$pdb = [System.IO.Path]::ChangeExtension($Out, ".pdb")

$res = Join-Path $objDir "pcntl_win.res"
$rc = "rc /nologo /D PCNTL_PHP_VERSION=\`"$($info.MajorMinor)\`" /fo `"$res`" `"$src\pcntl_win.rc`""
$cl = "cl /nologo $opt /W3 /MP $($defines -join ' ') $($includes -join ' ') /Fo`"$objDir\\`" /Fd`"$objDir\\`" `"$src\pcntl_win.c`" /link /DLL /OUT:`"$Out`" /PDB:`"$pdb`" /IMPLIB:`"$objDir\php_pcntl.lib`" `"$res`" `"$lib`" kernel32.lib advapi32.lib"
$cmd = "call `"$vcvars`" $($info.Arch) >nul 2>&1 && $rc && $cl"
Write-Host "Compiling..."
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Compilation failed (exit code $LASTEXITCODE)" }

# PHP refuses a module whose PE "linker version" is newer than the core's (win32/winutil.c), although all
# VS 2015-2022 toolsets are ABI compatible. Stamp the core's linker version into our header when ours is newer.
function Get-LinkerVersion([byte[]]$bytes) {
	$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
	return @{ Offset = $peOffset + 24 + 2; Major = $bytes[$peOffset + 24 + 2]; Minor = $bytes[$peOffset + 24 + 3] }
}
$coreDll = Get-ChildItem (Split-Path $phpExe) -Filter "php*.dll" | Where-Object { $_.Name -match '^php\d+(ts)?\.dll$' } | Select-Object -First 1
if ($coreDll) {
	$core = Get-LinkerVersion ([IO.File]::ReadAllBytes($coreDll.FullName))
	$ours = [IO.File]::ReadAllBytes($Out)
	$mine = Get-LinkerVersion $ours
	if ($mine.Major -eq 14 -and $core.Major -eq 14 -and [int]($mine.Minor / 10) -gt [int]($core.Minor / 10)) {
		Write-Host "Stamping linker version $($core.Major).$($core.Minor) (core) over $($mine.Major).$($mine.Minor) so PHP's loader accepts the module"
		$ours[$mine.Offset] = $core.Major
		$ours[$mine.Offset + 1] = $core.Minor
		[IO.File]::WriteAllBytes($Out, $ours)
	}
}

# sanity check: load it
$check = & $phpExe -n -d "extension=$Out" -f (Join-Path $PSScriptRoot "check.php") 2>&1
Write-Host "Built $Out"
Write-Host "Load check with $($info.Exe): $check"
if ("$check" -notmatch "^loaded") { throw "The built DLL did not load into $($info.Exe)" }
