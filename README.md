<div align="center">

# ZenithOS

**A 64-bit operating system written from scratch — with a compositing desktop that uses every CPU core.**

Own UEFI bootloader · own SMP kernel · own drivers · own FAT32 · own TCP/IP stack · own sound system · own web browser · own window system · 24 apps · installs itself to disk

![ZenithOS desktop](docs/showcase.png)

</div>

---

## What it is

ZenithOS boots on UEFI PCs and virtual machines; once the firmware hands
over, the kernel has a modern, animated desktop on screen in under a second. Nothing is borrowed from Linux,
BSD or any other kernel: the bootloader, the memory manager, the scheduler,
the drivers, the network stack, the compositor, the font renderer and all of
the applications are written for this system, in C and a little assembly.

Some highlights:

- **Real multi-core.** Every CPU core is started and runs the preemptive
  scheduler. Apps such as *Zenith 3D* and *Mandelbrot* split their work over
  all cores — the `bench` command shows the speed-up (≈3.6× on 4 cores).
- **A compositor, not a framebuffer painter.** Windows have rounded corners,
  soft drop shadows, fade/slide animations and per-pixel transparency; the
  taskbar and start menu use a blurred "acrylic" look. Only changed regions
  are recomposed, so it stays smooth even in software.
- **Protected user programs.** Programs in `/bin` are ELF executables that run
  in ring 3 in their own address space and talk to the kernel through system
  calls. A crashing program is terminated; the system keeps running.
- **Networking.** An Intel e1000 driver plus a small TCP/IP stack: DHCP, ARP,
  ICMP ping, DNS and HTTP downloads with `wget`.
- **A web browser.** *Zenith Web* has its own HTML parser and layout engine
  (headings, paragraphs, lists, links, preformatted text, entities), follows
  redirects, keeps a history and also opens local `file://` pages and folders.
- **Disks that keep your files.** An AHCI (SATA) driver, GPT/MBR partitions
  and a read/write FAT32 file system with long file names. The first volume
  is mounted at `/disk`; files are loaded on demand and written back in the
  background. Wallpaper, accent colour, keyboard layout and volume are saved
  there too.
- **It installs itself.** *Install ZenithOS* copies the running system (boot
  loader, kernel, system image) onto a disk — either next to the files on an
  existing FAT32 volume or onto a freshly erased disk with a new GPT and EFI
  System Partition — and the computer then boots from that disk.
- **Sound.** An Intel High Definition Audio driver (codec discovery, automatic
  output routing, DMA streaming) feeds a 32-voice software mixer with a
  synthesizer: eight instruments, system sounds, WAV playback. *Piano* turns
  the keyboard into an instrument.
- **USB.** An xHCI host controller driver with hub support and HID keyboards,
  mice and tablets (report descriptors are parsed, keys auto-repeat), next to
  the classic PS/2 path.
- **Chess on every core.** The chess engine splits its alpha-beta search over
  all CPU cores.
- **Anti-aliased everything.** Text uses Roboto and DejaVu Sans Mono,
  pre-rasterised with hinting; icons are drawn as vectors at any size.

| | |
|---|---|
| ![Start menu](docs/startmenu.png) | ![Zenith 3D and the System Monitor on four cores](docs/multicore.png) |
| Start menu with live search | 3D renderer loading all four cores |
| ![Files and Text Editor](docs/apps.png) | ![Mandelbrot and Blocks](docs/games.png) |
| Files and the editor with C highlighting | Parallel Mandelbrot and Blocks |
| ![Welcome](docs/welcome.png) | ![Networking in the terminal](docs/network.png) |
| Welcome tour | DHCP, ping, DNS and HTTP from the shell |
| ![Zenith Web](docs/browser.png) | ![Settings](docs/settings.png) |
| Zenith Web rendering a local HTML page | Settings with live-generated wallpapers |

## Trying it

ZenithOS needs **UEFI** firmware (no legacy BIOS) and an **x86-64** CPU.

### Build

On a Debian/Ubuntu machine:

```sh
sudo apt install build-essential clang lld mtools dosfstools xorriso qemu-system-x86 ovmf python3
make            # -> build/zenithos.iso
make run        # boots the ISO in QEMU: 4 CPUs, network, sound and a 1 GiB SATA disk
```

`make run` creates `build/disk.img` (GPT + FAT32) on first use; it shows up as
`/disk`. After running *Install ZenithOS* (or `install sda` in the terminal),
`make run-disk` boots the installed system from that disk without the ISO.
`make run-fast` boots straight from the build tree without making an ISO;
`make run-usb` attaches the keyboard and mouse over USB (behind a hub) instead of PS/2.

### Virtual machines

- **QEMU:** `make run`, or manually:
  `qemu-system-x86_64 -machine q35 -m 2G -smp 4 -bios /usr/share/ovmf/OVMF.fd -cdrom build/zenithos.iso -nic user,model=e1000`
  (the mouse follows the host pointer thanks to the VMware absolute-pointer
  protocol that QEMU provides).
- **VirtualBox / VMware:** create a 64-bit "Other" VM, **enable EFI**, give it
  2 GB of RAM and a few CPUs, attach the ISO. Use an Intel PRO/1000 (e1000)
  network adapter for networking.

### Real hardware

Write the ISO to a USB stick (it is a hybrid image) and boot it in UEFI mode:

```sh
sudo dd if=build/zenithos.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

The live system runs from RAM. SATA disks with a FAT32 volume are mounted at
`/disk`, and nothing is written to a disk unless you save files there or run
the installer. Keyboards and mice work over USB (xHCI, also behind hubs) or
PS/2; sound works on Intel HD Audio. Press a key during the boot countdown to
pick a different screen resolution; the installer remembers the current one.

## Using it

| Shortcut | Action |
|---|---|
| **Win** / click the Z | Start menu — just start typing to search |
| **Ctrl+Alt+T** | Terminal |
| **Alt+Tab** | Switch windows |
| **Alt+F4** | Close window |
| **Win+←/→**, **Win+↑/↓** | Snap left/right, maximize/restore |
| **Win+D**, **Win+E** | Show desktop, open Files |
| **Print Screen** | Screenshot to `~/Pictures` |
| **Ctrl+Alt+Del** | System Monitor |

The keyboard layout defaults to German (QWERTZ); click **DE** in the taskbar or
run `layout us` to switch.

### Applications

Files · Terminal · Text Editor · System Monitor · Zenith 3D · Mandelbrot ·
Blocks · Snake · Chess · Minesweeper · 2048 · Piano · Paint · Calculator ·
Clock (stopwatch, timer, calendar) · Images · System Log · Zenith Web ·
Network · Settings · Install ZenithOS · About · Welcome — plus the ring-3
programs *Plasma* and *Life* that open their own windows.

### The shell

The terminal runs **zsh** (the Zenith SHell): line editing, history, tab
completion, `>`/`>>` redirection and about 65 built-in commands, for example

```
ls -l   cd   cat   grep   tree   hexdump   cp   mv   rm -r   edit
ps   kill   free   df   uptime   lscpu   lspci   dmesg   neofetch
lsblk   mount   umount   sync   mkfs   install   play   beep   volume
ifconfig   dhcp   ping   nslookup   wget   calc   bench   cal   matrix
```

`open` starts apps, files and web pages (`open http://example.com`).
Anything else is looked up in `/bin` and started as a user process.

## Writing programs

User programs are plain C against a small libc (`user/libc/zenith.h`):

```c
#include <zenith.h>

int main(int argc, char **argv)
{
    printf("Hello from ring 3, I am pid %d\n", getpid());

    zwin_t *w = zwin_open(320, 200, "My window");      /* a real window */
    zfill(w, 0, 0, 320, 200, ZRGB(40, 20, 90));
    ztext(w, 20, 20, "Drawn by a user program", 0xFFFFFF, 15);
    zwin_present(w);

    struct z_event ev;
    while (zwin_event(w, &ev, -1) && ev.type != ZEV_CLOSE) {}
    return 0;
}
```

Drop the file into `user/apps/`, run `make`, and it appears in `/bin`.

## How it is built

```
boot/        UEFI bootloader (PE32+, no EDK2): GOP mode setting, ELF loading,
             page tables, memory map, hand-over to the kernel
kernel/
  arch/      GDT/IDT/TSS, interrupt and syscall entry, context switch, SMP start-up
  mm/        physical page allocator, 4-level paging, kernel heap
  proc/      SMP scheduler, wait channels, mutexes, parallel_for, processes, syscalls
  dev/       ACPI, local APIC + I/O APIC, timers, RTC, PS/2 + vmmouse, xHCI USB + HID,
             AHCI SATA, block devices + GPT/MBR, HD Audio + mixer/synth, PCI, serial, power
  fs/        VFS with mount points, RAM file system, tar initrd loader, FAT32, tty
  net/       e1000 driver, Ethernet/ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS, HTTP
  gfx/       2D graphics (anti-aliased shapes, gradients, blur, shadows), font renderer
  wm/        compositing window manager, desktop shell, widgets, vector icons
  apps/      the built-in applications, including the Zenith Web browser
user/        libc and ring-3 programs
tools/       font baking, symbol table generation, headless QEMU test driver
```

About 27 000 lines of C, assembly and Python.

`tools/qemu-test.py` boots the system headless, types, clicks and takes
screenshots through QMP — that is how the pictures above were made.

## Status

Version 1.0 "Aurora". It boots, it is fun, and it is honest about its limits:

- Only `/disk` (FAT32 on SATA) is persistent; the rest of the tree lives in
  RAM. No NVMe or USB storage yet, and no ext4/NTFS.
- USB covers keyboards, mice and tablets (no USB 3 hubs).
- Networking supports Intel e1000-family cards and plain HTTP (no TLS). Zenith
  Web renders HTML structure only — no CSS, images or JavaScript.
- Sound needs an Intel HD Audio controller; there is no audio input.

## Credits

Written by **jojojonas169-debug** with [Claude Code](https://claude.com/claude-code).
ZenithOS replaces the earlier EliteOS project in this repository.

Fonts: [Roboto](https://github.com/googlefonts/roboto) (Apache License 2.0) and
[DejaVu Sans / Sans Mono](https://dejavu-fonts.github.io/) (Bitstream Vera license;
the chess pieces come from DejaVu Sans), see `assets/fonts/`.
