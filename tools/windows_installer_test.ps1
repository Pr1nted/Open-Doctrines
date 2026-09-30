# Does the Windows installer install, appear, and uninstall cleanly?
#
# THE ONE SURFACE CI CANNOT REACH. tools/qualify.sh proves the Windows BUILD
# runs -- it plays a game on the GitHub runner through software OpenGL. What
# no automated check has ever touched is the thing most Windows players
# actually use: the NSIS installer. Whether it lands in Program Files, whether
# the Start-menu shortcut points at a file that exists, whether "Add or remove
# programs" lists it, and whether uninstalling takes it all away again.
#
# Every one of those has a way of being wrong that a build test cannot see. A
# shortcut pointing at $INSTDIR\OpenDoctrines.exe when the binary moved into a
# subdirectory is a working build and a broken install.
#
# RUN IT LIKE THIS, elevated, in the guest:
#
#   powershell -ExecutionPolicy Bypass -File windows_installer_test.ps1 `
#              -Installer C:\od\OpenDoctrines-1.2.2-win64.exe
#
# It is driven from macOS by tools/windows_installer_test.sh, which pushes this
# and the installer into the VM with utmctl and reads the result back.
#
# IT UNINSTALLS WHAT IT INSTALLS. The last thing it does is put the machine
# back, so the VM does not accumulate a copy per run and the uninstall path is
# itself under test. If the script dies in the middle, the install is still
# there -- deliberately, so it can be looked at.

param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [string]$ResultFile = "$env:TEMP\od-installer-result.txt"
)

$ErrorActionPreference = "Continue"
$checks = 0
$failed = 0
$lines  = @()

function Check($ok, $what, $detail = "") {
    $script:checks++
    $tag = if ($ok) { "ok  " } else { "FAIL"; $script:failed++ }
    $line = "  $tag  $what"
    if ($detail) { $line += "  [$detail]" }
    Write-Host $line
    $script:lines += $line
}
function Section($n) { Write-Host ""; Write-Host "== $n =="; $script:lines += ""; $script:lines += "== $n ==" }

Section "before anything"
if (-not (Test-Path $Installer)) {
    Write-Host "  FAIL  the installer is not at $Installer"
    "FAIL: installer missing" | Set-Content $ResultFile
    exit 1
}
Check $true "the installer is here" ((Get-Item $Installer).Length.ToString() + " bytes")

# Where NSIS puts it, and where the uninstaller registers itself. Both are read
# from the registry afterwards rather than assumed, because assuming the path
# is how a test passes on a machine where the install went somewhere else.
$uninstallKeys = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OpenDoctrines",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OpenDoctrines"
)
function Get-UninstallEntry {
    # Test-Path then Get-ItemProperty is a race during an uninstall: the key
    # can be marked for deletion between the two, and Get-ItemProperty then
    # throws "Illegal operation attempted on a registry key that has been
    # marked for deletion" instead of returning nothing. A key being deleted
    # is exactly the state this function is polled in, so treat the throw as
    # what it means -- no entry -- rather than letting it print and unwind.
    foreach ($k in $uninstallKeys) {
        try { if (Test-Path $k) { return Get-ItemProperty $k -ErrorAction Stop } }
        catch { continue }
    }
    return $null
}

Check ($null -eq (Get-UninstallEntry)) "it is not already installed"

Section "installing, silently"
$p = Start-Process -FilePath $Installer -ArgumentList "/S" -Wait -PassThru
Check ($p.ExitCode -eq 0) "the installer exits 0" "exit $($p.ExitCode)"
Start-Sleep -Seconds 3

$entry = Get-UninstallEntry
Check ($null -ne $entry) "Add or remove programs lists it"

$installDir = $null
if ($entry) {
    $installDir = $entry.InstallLocation
    if (-not $installDir -and $entry.UninstallString) {
        $installDir = Split-Path ($entry.UninstallString -replace '"', '')
    }
    Check ($null -ne $entry.DisplayVersion -and $entry.DisplayVersion -ne "") `
          "and gives a version" $entry.DisplayVersion
}

Check ($null -ne $installDir -and (Test-Path $installDir)) "the install directory exists" $installDir

if ($installDir -and (Test-Path $installDir)) {
    $exe = Get-ChildItem -Path $installDir -Filter "OpenDoctrines.exe" -Recurse -ErrorAction SilentlyContinue |
           Select-Object -First 1
    Check ($null -ne $exe) "the game executable is in it" $(if ($exe) { $exe.FullName } else { "not found" })

    # THE DATA DIRECTORY, because a game that installs without its maps starts
    # and then cannot begin a game -- which looks like a crash, not a packaging
    # fault.
    $maps = Get-ChildItem -Path $installDir -Filter "*.odmap" -Recurse -ErrorAction SilentlyContinue
    Check ($maps.Count -gt 0) "and its maps came with it" "$($maps.Count) .odmap"
}

Section "the Start menu"
# Both the per-user and the all-users menu: NSIS writes one or the other
# depending on how it was configured, and a shortcut nobody can find is the
# whole reason an installer exists rather than a zip.
$menus = @(
    "$env:ProgramData\Microsoft\Windows\Start Menu\Programs",
    "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"
)
$shortcuts = @()
foreach ($m in $menus) {
    if (Test-Path $m) {
        $shortcuts += Get-ChildItem -Path $m -Filter "*OpenDoctrines*.lnk" -Recurse -ErrorAction SilentlyContinue
    }
}
Check ($shortcuts.Count -gt 0) "there is a Start-menu shortcut" "$($shortcuts.Count) found"

foreach ($s in $shortcuts) {
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($s.FullName).TargetPath
    Check ($target -and (Test-Path $target)) "$($s.Name) points at a file that exists" $target
}

Section "uninstalling"
if ($entry -and $entry.UninstallString) {
    # RUN IT THE WAY WINDOWS DOES. "Add or remove programs" executes
    # UninstallString verbatim, so that is what gets tested here -- no _?=.
    #
    # _?= was the obvious thing to reach for, because it makes /S block until
    # the uninstall is finished instead of forking. It also guarantees a
    # failure: _?= tells NSIS to run in place rather than re-exec from %TEMP%,
    # and a running executable cannot delete itself, so Uninstall.exe and its
    # directory are always left behind. The first run of this test duly
    # reported "1 file(s)" left -- a defect that existed only because the test
    # asked for it. Uninstalling the real way leaves nothing.
    #
    # The cost is that the first process returns immediately after handing off
    # to its %TEMP% copy, so its exit code says nothing and there is nothing to
    # wait on. Poll for the outcome instead, which is the claim worth making
    # anyway: after uninstalling, the machine is back.
    $un = $entry.UninstallString -replace '"', ''
    Start-Process -FilePath $un -ArgumentList "/S"

    $gone = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 2
        if ($null -eq (Get-UninstallEntry) -and -not (Test-Path $installDir)) { $gone = $true; break }
    }
    Check $gone "the uninstall finishes" $(if ($gone) { "~$($i * 2)s" } else { "still not done after 120s" })

    Check ($null -eq (Get-UninstallEntry)) "and Add or remove programs forgets it"

    $leftover = @()
    if ($installDir -and (Test-Path $installDir)) {
        $leftover = Get-ChildItem -Path $installDir -Recurse -ErrorAction SilentlyContinue
    }
    Check ($leftover.Count -eq 0) "nothing is left in the install directory" "$($leftover.Count) file(s)"

    $stillThere = @()
    foreach ($m in $menus) {
        if (Test-Path $m) {
            $stillThere += Get-ChildItem -Path $m -Filter "*OpenDoctrines*.lnk" -Recurse -ErrorAction SilentlyContinue
        }
    }
    Check ($stillThere.Count -eq 0) "and the Start-menu shortcut is gone" "$($stillThere.Count) left"
} else {
    Check $false "the uninstaller is registered so it can be run"
}

Write-Host ""
Write-Host "$checks checks, $failed failed"
$lines += ""
$lines += "$checks checks, $failed failed"
$lines += $(if ($failed -eq 0) { "RESULT: PASS" } else { "RESULT: FAIL" })
$lines | Set-Content $ResultFile
exit $(if ($failed -eq 0) { 0 } else { 1 })
