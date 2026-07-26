# gearbox.ps1 - one command to build a mod and put it where the game finds it.
#
#   .\tools\gearbox.ps1 doctor
#   .\tools\gearbox.ps1 new my-mod
#   .\tools\gearbox.ps1 dev my-mod      # build + install; bind this to a key
#
# Native PowerShell twin of tools/gearbox. Same behaviour, same layout, so a
# mod directory built on Windows is byte-identical to one built on macOS.
# Set $env:GEARBOX_MODS_DIR if your install lives somewhere unusual.
param([Parameter(Position=0)][string]$Cmd = "help",
      [Parameter(Position=1)][string]$Target = "",
      [string]$Lang = "c")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tc   = Join-Path $root ".toolchains"

function ModsDir {
    if ($env:GEARBOX_MODS_DIR) { return $env:GEARBOX_MODS_DIR }
    return (Join-Path $root "data\mods")
}

# Apple's clang cannot target wasm32 and neither can MSVC. On Windows the usual
# sources are Emscripten's bundled clang or an upstream LLVM install.
function FindWasmClang([string]$exe) {
    $cands = @()
    if ($env:CC) { $cands += $env:CC }
    $cands += @((Get-Command $exe -ErrorAction SilentlyContinue).Source,
                "C:\Program Files\LLVM\bin\$exe.exe",
                "$env:EMSDK\upstream\bin\$exe.exe")
    foreach ($c in $cands) {
        if (-not $c -or -not (Test-Path $c)) { continue }
        if ((& $c --print-targets 2>$null | Out-String) -match "wasm32") { return $c }
    }
    return $null
}

function DetectLang([string]$d) {
    if (Test-Path (Join-Path $d "Cargo.toml")) { return "rust" }
    if (Test-Path (Join-Path $d "go.mod"))     { return "go" }
    if (Test-Path (Join-Path $d "assembly"))   { return "as" }
    if (Get-ChildItem $d -Filter *.wat   -ErrorAction SilentlyContinue) { return "wat" }
    if (Get-ChildItem $d -Filter *.zig   -ErrorAction SilentlyContinue) { return "zig" }
    if (Get-ChildItem $d -Filter *.cpp   -ErrorAction SilentlyContinue) { return "cpp" }
    if (Get-ChildItem $d -Filter *.c     -ErrorAction SilentlyContinue) { return "c" }
    return "unknown"
}

function Pack([string]$d, [string]$out) {
    # MANIFEST.json must be the archive's FIRST entry -- the loader validates it
    # before decompressing anything else. Compress-Archive gives no ordering
    # control, so the zip is built by hand.
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
    if (Test-Path $out) { Remove-Item $out -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($out, "Create")
    try {
        foreach ($n in @("MANIFEST.json", "mod.wasm", "thumbnail.png", "signature.bin")) {
            $p = Join-Path $d $n
            if (Test-Path $p) {
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $p, $n) | Out-Null
            }
        }
        $dataDir = Join-Path $d "data"
        if (Test-Path $dataDir) {
            Get-ChildItem $dataDir -Recurse -File | ForEach-Object {
                $rel = "data/" + $_.FullName.Substring($dataDir.Length + 1).Replace("\", "/")
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $_.FullName, $rel) | Out-Null
            }
        }
    } finally { $zip.Dispose() }
    Write-Host "packed $out ($((Get-Item $out).Length) bytes)"
}

switch ($Cmd) {

"doctor" {
    Write-Host "Gearbox toolchain report`n"
    foreach ($p in @(@("C","clang"), @("C++","clang++"))) {
        $f = FindWasmClang $p[1]
        Write-Host ("  {0,-16} {1}" -f $p[0], $(if ($f) { $f } else { "MISSING (need a wasm32-capable clang)" }))
    }
    foreach ($t in @(@("rust","cargo\bin\cargo.exe","cargo"), @("zig","zig\zig.exe","zig"),
                     @("tinygo","tinygo\bin\tinygo.exe","tinygo"), @("wat2wasm","wabt\bin\wat2wasm.exe","wat2wasm"))) {
        $local = Join-Path $tc $t[1]
        $found = if (Test-Path $local) { $local }
                 elseif (Get-Command $t[2] -ErrorAction SilentlyContinue) { (Get-Command $t[2]).Source }
                 else { "MISSING" }
        Write-Host ("  {0,-16} {1}" -f $t[0], $found)
    }
    Write-Host ""
    Write-Host ("  mods folder     " + (ModsDir))
    $chk = Join-Path $root "build\odmod-check.exe"
    Write-Host ("  odmod-check     " + $(if (Test-Path $chk) { $chk } else { "not built" }))
}

"new" {
    if (-not $Target) { Write-Host "usage: gearbox.ps1 new <name> [-Lang c|cpp]"; exit 2 }
    $d = Join-Path (Get-Location) $Target
    if (Test-Path $d) { Write-Host "$d already exists"; exit 1 }
    New-Item -ItemType Directory -Path $d | Out-Null
    @"
{
  "schema": 1,
  "id": "com.example.$Target",
  "name": "$Target",
  "version": "1.0.0",
  "description": "A Gearbox mod.",
  "authors": ["you"],
  "gearbox": "1.0",
  "modules": ["Core", "UI", "GameState.Read"],
  "limits": { "memoryPages": 16, "fuelPerTurn": 200000 }
}
"@ | Set-Content -Path (Join-Path $d "MANIFEST.json") -Encoding UTF8
    $ext = if ($Lang -eq "cpp") { "cpp" } else { "c" }
    Copy-Item (Join-Path $root "sdk\examples\hello-panel\mod.c") (Join-Path $d "mod.$ext")
    Copy-Item (Join-Path $root "sdk\gearbox.h") (Join-Path $d "gearbox.h")
    Write-Host "created $d"
    Write-Host '  edit MANIFEST.json -- change "id" to your own reverse-DNS name'
    Write-Host "  then:  .\tools\gearbox.ps1 dev $Target"
}

{ $_ -in "build","dev" } {
    $d = if ($Target) { (Resolve-Path $Target).Path } else { (Get-Location).Path }
    if (-not (Test-Path (Join-Path $d "MANIFEST.json"))) { Write-Host "no MANIFEST.json in $d"; exit 1 }
    $lang = DetectLang $d
    Write-Host "building $(Split-Path -Leaf $d)  [$lang]"
    $wasm = Join-Path $d "mod.wasm"

    switch ($lang) {
      { $_ -in "c","cpp" } {
        $exe = if ($lang -eq "cpp") { "clang++" } else { "clang" }
        $cc = FindWasmClang $exe
        if (-not $cc) { Write-Host "no wasm32-capable $exe found"; exit 1 }
        $inc = if (Test-Path (Join-Path $d "gearbox.h")) { $d } else { Join-Path $root "sdk" }
        $srcs = Get-ChildItem $d -Filter "*.$(if ($lang -eq 'cpp') {'cpp'} else {'c'})" | ForEach-Object { $_.FullName }
        $extra = if ($lang -eq "cpp") { @("-fno-exceptions","-fno-rtti","-std=c++17") } else { @() }
        & $cc --target=wasm32 -nostdlib -O2 -I $inc @extra `
              -Wl,--no-entry -Wl,--allow-undefined -o $wasm @srcs
        if ($LASTEXITCODE -ne 0) { exit 1 }
      }
      "rust" { Push-Location $d; & (Join-Path $tc "cargo\bin\cargo.exe") build --release --target wasm32-unknown-unknown; Pop-Location
               Get-ChildItem $d -Recurse -Filter *.wasm | Where-Object { $_.FullName -match "release" } |
                 Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName $wasm -Force } }
      "zig"  { Push-Location $d; & (Join-Path $tc "zig\zig.exe") build-exe -target wasm32-freestanding -fno-entry `
                 -O ReleaseSmall --export=mod_load "-femit-bin=$wasm" (Get-ChildItem $d -Filter *.zig | ForEach-Object { $_.FullName }); Pop-Location }
      "go"   { Push-Location $d; & (Join-Path $tc "tinygo\bin\tinygo.exe") build -target=wasm-unknown -no-debug -o $wasm .; Pop-Location }
      "as"   { Push-Location $d; & ".\node_modules\.bin\asc.cmd" assembly/index.ts --config asconfig.json --target release; Pop-Location }
      "wat"  { $w = Join-Path $tc "wabt\bin\wat2wasm.exe"
               if (-not (Test-Path $w)) { $w = "wat2wasm" }
               & $w (Get-ChildItem $d -Filter *.wat | Select-Object -First 1).FullName -o $wasm }
      default { Write-Host "cannot tell what language this is"; exit 1 }
    }

    if (-not (Test-Path $wasm)) { Write-Host "build produced no mod.wasm"; exit 1 }

    # A stray import is the failure mode in every language; catch it here where
    # the message is useful rather than at load time.
    & python3 (Join-Path $root "tools\wasm_imports.py") $wasm | Out-Null
    if ($LASTEXITCODE -ne 0) {
        & python3 (Join-Path $root "tools\wasm_imports.py") $wasm
        exit 1
    }

    $odmod = Join-Path $d ((Split-Path -Leaf $d) + ".odmod")
    Pack $d $odmod

    if ($Cmd -eq "dev") {
        $out = ModsDir
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        Copy-Item $odmod $out -Force
        Write-Host "installed $(Split-Path -Leaf $odmod) -> $out"
        Write-Host "In the game: Mod Menu -> Reload modloader"
    }
}

"install" {
    $d = if ($Target) { (Resolve-Path $Target).Path } else { (Get-Location).Path }
    $odmod = Join-Path $d ((Split-Path -Leaf $d) + ".odmod")
    if (-not (Test-Path $odmod)) { Write-Host "no .odmod -- run build first"; exit 1 }
    $out = ModsDir
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item $odmod $out -Force
    Write-Host "installed -> $out"
    Write-Host "In the game: Mod Menu -> Reload modloader"
}

default {
    Write-Host "gearbox.ps1 <doctor|new|build|install|dev> [target] [-Lang c|cpp]"
    Write-Host "  dev = build + install, the one to use after every edit"
}
}
