# run.ps1 - Launch the FTP4DOS DOS-7.1 test VM in QEMU.
#
#   .\run.ps1             # normal run: boot hdd.img, NIC + D: share + QMP on 4445
#   .\run.ps1 -Headless   # no GUI window (screendump via QMP)
#
# QMP listens on 127.0.0.1:4445 (4444 is used by the DOS-6.22 vm/).
# Control with:  $env:QMP_PORT=4445; py vmctl.py shot boot
#
# NIC: NE2000 ISA (int 0x60 / irq 10 / io 0x300) matching the Crynwr
# NE2000.COM packet driver in share\NET\.
param(
    [switch]$Headless
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$qemu = "C:\Program Files\qemu\qemu-system-i386.exe"
$hdd  = Join-Path $PSScriptRoot "hdd.img"

$args = @(
    "-machine", "pc",
    "-cpu", "pentium",
    "-m", "32",
    "-rtc", "base=localtime",
    "-qmp", "tcp:127.0.0.1:4445,server,nowait"
)

if ($Headless) { $args += @("-display", "sdl") }

# Boot disk (qcow2, inherited from myDosEmu).
$args += @("-drive", "file=$hdd,format=qcow2,if=ide,index=0,media=disk")

# VVFAT share as D: — read-only from guest side.
# After updating share/ content, restart QEMU to pick up changes.
$args += @("-drive", "file=fat:rw:share,if=ide,index=1")

# NE2000 ISA NIC matching Crynwr NE2000.COM (int 0x60 / irq 10 / io 0x300).
$args += @(
    "-netdev", "user,id=net0",
    "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=10"
)

$args += @("-boot", "order=c,menu=on")

Write-Host "Launching QEMU (QMP port 4445):" ($args -join " ")
Write-Host "  Control: `$env:QMP_PORT=4445; py vmctl.py shot boot"
& $qemu @args
