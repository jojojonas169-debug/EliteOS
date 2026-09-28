<div align="center">

# ZenithOS

**A 64-bit operating system written from scratch — with a compositing desktop that uses every CPU core.**

Own UEFI bootloader · own SMP kernel · own drivers · own TCP/IP stack · own window system · 18 apps

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
- **USB.** An xHCI host controller driver with hub support and HID keyboards,
  mice and tablets (report descriptors are parsed, keys auto-repeat), next to
  the classic PS/2 path.
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

## Trying it

ZenithOS needs **UEFI** firmware (no legacy BIOS) and an **x86-64** CPU.

### Build

On a Debian/Ubuntu machine:

```sh
sudo apt install build-essential clang lld mtools dosfstools xorriso qemu-system-x86 ovmf python3
make            # -> build/zenithos.iso
make run        # boots the ISO in QEMU with 4 CPUs and a network card
```

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

Everything runs from RAM; nothing on your disks is touched. Keyboards and mice
work over USB (xHCI, also behind hubs) or PS/2. Press a key during the boot countdown to pick a different
screen resolution; the choice is remembered on writable media.

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
Blocks · Snake · Paint · Calculator · Clock (stopwatch, timer, calendar) ·
Images · System Log · Network · Settings · About · Welcome — plus the ring-3
programs *Plasma* and *Life* that open their own windows.

### The shell

The terminal runs **zsh** (the Zenith SHell): line editing, history, tab
completion, `>`/`>>` redirection and about 55 built-in commands, for example

```
ls -l   cd   cat   grep   tree   hexdump   cp   mv   rm -r   edit
ps   kill   free   df   uptime   lscpu   lspci   dmesg   neofetch
ifconfig   dhcp   ping   nslookup   wget   calc   bench   cal   matrix
```

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
             PCI, serial, power
  fs/        RAM file system with tar initrd loader, terminal line discipline
  net/       e1000 driver, Ethernet/ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS, HTTP
  gfx/       2D graphics (anti-aliased shapes, gradients, blur, shadows), font renderer
  wm/        compositing window manager, desktop shell, widgets, vector icons
  apps/      the built-in applications
user/        libc and ring-3 programs
tools/       font baking, symbol table generation, headless QEMU test driver
```

About 20 000 lines of C, assembly and Python.

`tools/qemu-test.py` boots the system headless, types, clicks and takes
screenshots through QMP — that is how the pictures above were made.

## Status

Version 1.0 "Aurora". It boots, it is fun, and it is honest about its limits:

- The file system lives in RAM: changes are lost at shutdown.
- USB covers keyboards, mice and tablets (no USB storage or USB 3 hubs).
- Networking supports Intel e1000-family cards and plain HTTP (no TLS).
- No sound.

## Credits

Written by **jojojonas169-debug** with [Claude Code](https://claude.com/claude-code).
ZenithOS replaces the earlier EliteOS project in this repository.

Fonts: [Roboto](https://github.com/googlefonts/roboto) (Apache License 2.0) and
[DejaVu Sans Mono](https://dejavu-fonts.github.io/) (Bitstream Vera license),
see `assets/fonts/`.
