# Make a Windows machine able to run tools/qualify.sh.
#
# Run ONCE, in an ELEVATED PowerShell (Win+X -> "Terminal (Admin)"):
#
#   powershell -ExecutionPolicy Bypass -File tools\windows_vm_setup.ps1
#
# Then RESTART, and build from Git Bash -- not PowerShell, the suite is bash:
#
#   cd /c/OpenDoctrines
#   OD_JOBS=1 OD_CMAKE_ARGS="-A x64" tools/qualify.sh build-x64
#
# WHY -A x64 IS NOT OPTIONAL, EVEN ON AN ARM64 MACHINE
#
# The mod runtime cannot be built by MSVC for ARM64 at all. WAMR ships
# invokeNative -- the assembly trampoline the interpreter calls host functions
# through -- as MASM for x64 and ia32, and as GAS for AArch64. armasm64 cannot
# assemble GAS, so the symbol is never defined and the link fails with
# "unresolved external symbol invokeNative" after a full build. CMakeLists.txt
# stops that at configure now and says so, but the fix is still -A x64.
#
# x64 is what ships anyway. On an ARM64 machine the compiler runs natively
# (VS installs ARM64-hosted x64 cross tools) and only the test binaries are
# emulated, which is fast enough to be worth doing.
#
# WHAT THIS WAS WRITTEN AGAINST
#
# A Windows 11 ARM64 guest in UTM on an M1, used to qualify the Windows build
# while GitHub Actions was unavailable. Every step below is one that actually
# ran there; the three things that DID NOT work are recorded as comments rather
# than removed, because each one looks like the obvious approach:
#
#   Add-WindowsCapability -Name OpenSSH.Server   fails 0x800f0950 -- the
#       Feature-on-Demand payload is not obtainable on an Insider build.
#   winget install Microsoft.OpenSSH.Beta        "No package found": it
#       publishes x64/x86 installers only, and winget filters by architecture.
#   VC.Tools.ARM64 alone                        builds nothing usable, for the
#       invokeNative reason above. The x64 toolset is the one you need.
#
# SSH is deliberately not set up here. It only saves the person driving the
# machine some clicks, it cost three dead ends to chase, and it is not what
# makes Windows tested.
#
# HOW FAR THIS IS ACTUALLY VERIFIED
#
# Every winget command below was run by hand on that guest and succeeded, with
# the ARM64 installers each package resolved to. The SCRIPT has never been run
# end to end -- it was assembled afterwards from what worked, on a Mac with no
# PowerShell to parse it. So expect a typo rather than a wrong instruction.

$ErrorActionPreference = "Stop"

$me = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "NOT ELEVATED. Win+X -> 'Terminal (Admin)', then run this again." -ForegroundColor Red
    Write-Host "A non-elevated run half-succeeds, which is worse than failing." -ForegroundColor Red
    exit 1
}
Write-Host "elevated OK" -ForegroundColor Green

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is missing. Install 'App Installer' from the Microsoft Store first."
}

function Install-Pkg($id, $override) {
    Write-Host "==> $id" -ForegroundColor Cyan
    $a = @("install", "--id", $id, "--exact", "--silent",
           "--accept-package-agreements", "--accept-source-agreements")
    if ($override) { $a += @("--override", $override) }
    & winget @a
    # -1978335189 is "already installed", the normal answer on a re-run.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        throw "$id failed (winget exit $LASTEXITCODE)"
    }
}

# Git for Windows: the suite is bash. Git Bash is the shell CI uses on Windows
# too, so this is not a second implementation to keep in step.
Install-Pkg "Git.Git"

# CMake as a standalone CLI rather than the copy inside Visual Studio, so plain
# `cmake` is on PATH in Git Bash.
Install-Pkg "Kitware.CMake"

# Node 22+: the stand-in account service signs with WebCrypto Ed25519, which
# arrived in node 18. On older node the file does not report an unsupported
# algorithm -- it fails to PARSE.
Install-Pkg "OpenJS.NodeJS.LTS"

# python.org's, NOT the Microsoft Store's. Windows keeps a python3.exe stub in
# WindowsApps that prints an advertisement and exits; `command -v python3` finds
# it and nothing works. tools/find_python.sh resolves around that by running the
# interpreter rather than trusting the name, but a real Python still has to exist.
Install-Pkg "Python.Python.3.12"

# LLVM for a clang that can target wasm32. Without it tests/build_test_mods.sh
# builds no fixture mods, and ModRuntimeTest quietly runs 1 check instead of 113
# -- including the fuel limit, which is the check that matters most here because
# it depends on a WAMR patch that only takes effect under MSVC.
#
# The installer does not add clang to PATH. tests/build_test_mods.sh and
# tests/check_doc_examples.sh both look under C:\Program Files\LLVM, so that is
# handled; if you move it, set CC instead.
Install-Pkg "LLVM.LLVM"

# BOTH toolsets. x64 is what builds (see the header); ARM64 is harmless to have
# and saves a second trip through the installer if it is ever wanted.
# --includeRecommended pulls the Windows SDK, without which nothing links.
Install-Pkg "Microsoft.VisualStudio.2022.BuildTools" @"
--quiet --wait --norestart --nocache
--add Microsoft.VisualStudio.Workload.VCTools
--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64
--add Microsoft.VisualStudio.Component.VC.Tools.ARM64
--includeRecommended
"@.Replace("`r`n", " ").Replace("`n", " ")

# py, not python: the Store stub shadows a real install and does nothing useful
# when invoked non-interactively.
Write-Host "==> Pillow" -ForegroundColor Cyan
& py -3.12 -m pip install --quiet --upgrade pip
& py -3.12 -m pip install --quiet Pillow

Write-Host ""
Write-Host "RESTART NOW." -ForegroundColor Yellow
Write-Host "Visual Studio Build Tools asks for it, and PATH changes are not"
Write-Host "visible to Git Bash until you do."
Write-Host ""
Write-Host "Then, in Git Bash:" -ForegroundColor Cyan
Write-Host "  git clone https://github.com/Pr1nted/Open-Doctrines.git /c/OpenDoctrines"
Write-Host "  cd /c/OpenDoctrines"
Write-Host "  command -v cmake node python3        # three paths expected"
Write-Host "  OD_JOBS=1 OD_CMAKE_ARGS=`"-A x64`" tools/qualify.sh build-x64"
Write-Host ""
Write-Host "OD_JOBS=1 is not timidity: this project is memory-hungry to compile"
Write-Host "and an OOM-killed build looks exactly like a compiler crash."
Write-Host ""
Write-Host "Expect 'play a real game' to SKIP on a VM -- no GL context means"
Write-Host "raylib cannot open a window. Everything before it still counts."
