# Everything the Windows test VM needs, done once, at its first logon.
#
# Called by tools/autounattend.xml. It runs as the local administrator the
# answer file just created, with a desktop already up, which is the first
# moment all of this is possible -- OpenSSH's capability installer and an NSIS
# package both want a real session, not WinPE.
#
# WHY IT EXISTS. Every step below was a thing somebody had to do by hand in the
# VM, from screenshots relayed back and forth: enable SSH, run the guest tools,
# find out afterwards whether either worked. A VM that is worth rebuilding is
# one that needs none of that.
#
# It writes C:\od-setup-report.txt and drops a copy on the desktop, because the
# whole point is to be able to SEE what happened without being able to log in.
$ErrorActionPreference = "Continue"
$report = @()
function Say($s) { Write-Host $s; $script:report += $s }

Say "OpenDoctrines VM first-logon setup -- $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
Say "arch: $env:PROCESSOR_ARCHITECTURE"
Say ""

# ── SSH, which is how the Mac gets in ──
#
# The INBOX capability, not winget's package: this is an ARM64 component
# shipped with Windows, so it needs no download and no x64 emulation. winget's
# OpenSSH publishes x64/x86 installers only and filters itself out here.
Say "== OpenSSH Server =="
try {
    $cap = Get-WindowsCapability -Online -Name OpenSSH.Server* | Select-Object -First 1
    if ($cap.State -ne "Installed") {
        Add-WindowsCapability -Online -Name $cap.Name | Out-Null
    }
    Set-Service -Name sshd -StartupType Automatic -ErrorAction Stop
    Start-Service sshd -ErrorAction Stop
    Say "  sshd: $((Get-Service sshd).Status)"

    # The rule is usually created by the capability, but not always.
    if (-not (Get-NetFirewallRule -Name "OpenSSH-Server-In-TCP" -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name "OpenSSH-Server-In-TCP" -DisplayName "OpenSSH Server (sshd)" `
            -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 | Out-Null
        Say "  firewall rule created"
    } else {
        Say "  firewall rule already present"
    }

    # DO NOT SET DefaultShell TO POWERSHELL. It looks like an obvious
    # convenience and it breaks `ssh host <command>` completely: sshd hands the
    # command to the shell with cmd's /c option, powershell does not understand
    # it, and the session connects, exits 0 and returns NO OUTPUT AT ALL. Every
    # command appears to succeed, silently, which is far worse than an error.
    # Tried 2026-09-30 and reverted the same evening.
    #
    # cmd.exe is the default and handles exec properly. Anything that wants
    # PowerShell asks for it by name.
    Remove-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell -ErrorAction SilentlyContinue
    Say "  default shell: cmd (deliberately NOT powershell)"
} catch {
    Say "  FAILED: $($_.Exception.Message)"
}
Say ""

# ── the SPICE/QEMU guest tools ──
#
# Clipboard sharing (spice-vdagent), the shared folder (spice-webdavd) and the
# QEMU guest agent (qemu-ga, which is what utmctl exec talks to) all live in
# this one package. It is an x86/x64 build: its SERVICES are user-mode and
# Windows-on-ARM emulates those, but the virtio DRIVERS in it are kernel-mode
# and cannot be. Driver errors during this step are expected and not fatal --
# they are also why the display stays at 1024x768 whatever happens here.
Say "== SPICE guest tools =="
$tools = $null
foreach ($d in (Get-PSDrive -PSProvider FileSystem)) {
    $p = Join-Path $d.Root "spice-guest-tools.exe"
    if (Test-Path $p) { $tools = $p; break }
}
if ($tools) {
    Say "  found: $tools"
    try {
        $p = Start-Process -FilePath $tools -ArgumentList "/S" -Wait -PassThru
        Say "  installer exit: $($p.ExitCode)"
    } catch {
        Say "  FAILED to run: $($_.Exception.Message)"
    }
    Start-Sleep -Seconds 5
    $svc = Get-Service | Where-Object {
        $_.Name -match 'spice|vdagent|qemu' } | ForEach-Object { "$($_.Name)=$($_.Status)" }
    if ($svc) { Say "  services: $($svc -join ', ')" }
    else      { Say "  services: none appeared (expected if x64 emulation refused them)" }
} else {
    Say "  spice-guest-tools.exe not found on any drive -- skipped"
}
Say ""

# ── what the Mac needs to know ──
Say "== addresses =="
foreach ($ip in (Get-NetIPAddress -AddressFamily IPv4 |
                 Where-Object { $_.IPAddress -notlike '127.*' })) {
    Say "  $($ip.InterfaceAlias): $($ip.IPAddress)"
}
Say ""
Say "log in from the Mac:  ssh -p 2222 odtest@127.0.0.1   (password: odtest)"

$report | Set-Content C:\od-setup-report.txt
$report | Set-Content "$env:PUBLIC\Desktop\od-setup-report.txt"
