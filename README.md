# clover-emu

Run the **NES Classic Edition** and **Super NES Classic Edition** firmware on Windows — the
real thing, not a re-implementation. On Windows the app is called **Console Classics**.

clover-emu boots a console's original Linux userland straight from its firmware image
(`.hsqs`): BusyBox `init`, the init scripts, Nintendo's `clover-mcp` system manager, the
Lua-driven home menu (`ReedPlayer-Clover`) and Nintendo's own emulators — `kachikachi`
(NES) and `canoe` (SNES). Every one of those programs is the original 32-bit ARM binary,
executed unmodified. Video, audio and controllers are bridged to the PC with SDL2 and
OpenGL. (This is a Windows port of the original macOS project, whose macOS build is kept.)

| Console | Firmware tested | Status |
|---|---|---|
| NES Classic Edition (USA) | `dp-nes` v1.0.7 | ✅ boots, menu, games, suspend points, saves, reset, power-off |
| Super NES Classic Edition (USA) | `dp-snesusa` v2.0.7 | ✅ boots, menu, games, suspend points, rewind, saves, reset, power-off |

Both run at a locked 60 fps — the consoles' native 720p60 output (see
[Timing](#timing)).

> **No Nintendo software is included in this repository.** You need a firmware image
> dumped from a console you own.

---

## How it works

The firmware image is only a root filesystem — there is no kernel in it, and the hardware
(Allwinner R16 SoC, Mali-400 GPU) isn't practical to emulate. clover-emu instead does what
`qemu-user` does: it runs the ARM programs in user mode and plays the role of the Linux
kernel itself.

```
            original firmware (ARMv7, unmodified)
 ┌───────────────────────────────────────────────────────────┐
 │ init ─ rcS ─ clover-mcp ─┬─ ReedPlayer-Clover (home menu)  │
 │                          └─ kachikachi (NES) / canoe (SNES)│
 │ glibc 2.22 · ld-linux · libstdc++ · libpng · LuaJIT · …    │
 ├───────────────────────────────────────────────────────────┤
 │ libSDL2 / libMali (EGL+GLES2) / libasound  ← replaced by   │
 │ generated stub libraries that trap into the host           │
 └───────────────────────────────────────────────────────────┘
        │ svc #0 (Linux syscalls)          │ svc #HLE (library calls)
 ┌──────▼──────────────────────────────────▼─────────────────┐
 │ clover-emu                                                 │
 │  • ARMv7 → ARM64 JIT (dynarmic)                            │
 │  • Linux kernel emulation: processes, threads, fork/exec,  │
 │    signals, futex, mmap, pipes, sockets, epoll, AF_ALG,    │
 │    /proc, /dev/fb0, Allwinner /dev/disp, …                 │
 │  • VFS: read-only rootfs + tmpfs /var,/tmp + persistent    │
 │    /var/lib                                                │
 │  • HLE: SDL2 → host SDL2, GLES2 → desktop OpenGL,          │
 │    ALSA → SDL audio                                        │
 └───────────────────────────────────────────────────────────┘
                Windows (or macOS) · SDL2 · OpenGL
```

- **CPU** — [dynarmic](https://github.com/azahar-emu/dynarmic) JIT-compiles ARMv7
  (ARM, VFPv4, NEON) to x86-64 on Windows (ARM64 on Apple Silicon). Each guest thread gets its own JIT instance; each guest
  process its own 4 GiB address space (fastmem) and exclusive monitor for
  `ldrex`/`strex`. The kernel "kuser helper" page is provided.
- **Kernel** — ~200 Linux ARM EABI syscalls. All guest processes live inside one host
  process, so `fork` (copy-on-write via `vm_copy`), `vfork`, `execve` (including `#!`
  scripts), `wait4`, `kill`, process groups, resource limits and signal delivery (proper
  `rt_sigframe`s, `SA_RESTART`) are emulated in-process — which keeps a single window
  across the menu → game → menu handoffs. The kernel crypto socket API (`AF_ALG`, used
  for SHA-1 checks of ROMs and suspend points) is backed by built-in SHA-1/SHA-256.
- **Libraries** — at boot clover-emu generates tiny ARM ELF shared objects that replace
  `libSDL2.so`, `libMali.so` (all EGL/GLES symbols) and `libasound.so.2`. Each exported
  function is `svc #n; bx lr`; the host decodes the AAPCS-VFP (hard-float) arguments and
  calls real SDL2/OpenGL. Everything else — glibc, the dynamic linker, libstdc++,
  LuaJIT — is the firmware's own. `SDL_CreateThread` calls the guest's own
  `pthread_create`, so SDL threads get real glibc TLS.
- **Graphics** — GLES2 shaders are translated to GLSL 1.20; the guest's default
  framebuffer is a 1280×720 offscreen FBO scaled into the window on swap. `/dev/fb0` is
  real shared memory: `clover-mcp`'s splash screens and the SNES `canoe` emulator (which
  renders on the CPU straight into fb0 and page-flips with `FBIOPAN_DISPLAY`) show up
  directly.
- **Console buttons** — the power switch is `SDL_SCANCODE_POWER` held down, RESET is
  `SDLK_VOLUMEUP` (found by disassembling `clover-mcp`). SIGTERM becomes `SDL_QUIT`
  exactly like real SDL, which is how the emulators write suspend points when you press
  RESET.

### Timing

The consoles drive HDMI at 1280×720 **60.00 Hz** (74.25 MHz pixel clock), and the firmware
asks for exactly that mode (`MenuResolution = 1280x720-60`). Nintendo's emulators lock one
emulated frame to each display refresh rather than running at the original NTSC rate
(~60.1 Hz), absorbing the difference in audio. clover-emu reproduces the hardware's
pacing: GL swaps block like Mali's `eglSwapBuffers` (NES), and `FBIO_WAITFORVSYNC` waits
for the next refresh (SNES). On a ~60 Hz monitor that refresh is the host's real vblank —
the window is presented with vsync and every presented frame is one console frame — so
frames reach the screen evenly, without tearing. On other refresh rates a precise 60 Hz
timer paces the console instead (`CLOVER_VSYNC=0` / `1` forces either mode). High-resolution
timers keep `nanosleep`-based pacing in the home menu accurate.

## Requirements

- Windows 10 (version 1803 or later) or Windows 11, 64-bit
- A GPU with OpenGL 2.1 (any current NVIDIA / AMD / Intel driver)

## Windows

The program is called **Console Classics** (`Console Classics.exe`) and needs nothing
extra installed: a built-in SquashFS reader extracts the firmware.

**Playing** — put `Console Classics.exe` and `SDL2.dll` in a folder together with your
`.hsqs` image(s) and double-click the exe:

- one image: that console turns on directly;
- several images: a full-screen chooser shows one card per console (pad, keyboard or mouse);
  the last console you played is pre-selected;
- no image: a file dialog asks where it is, and the choice is remembered.

The first boot of an image shows a progress bar while it is extracted. The window opens
full-screen (`F11` or `Alt`+`Enter` toggles). Texts follow the Windows display language
(English, Português, Español, Français, Deutsch, Italiano, Nederlands, Русский, 日本語,
简体中文, 繁體中文, 한국어). With no console attached, output goes to `Console Classics.log` next
to the exe, and errors are shown in a dialog. Settings live in `%APPDATA%\Console Classics`.

**Português do Brasil** — the firmware only ships European Portuguese. Console Classics
replaces it with its own Brazilian Portuguese translation when the console boots: the
language list shows *Português (Brasil)*, the home menu texts are in pt-BR and the
"SHUTTING DOWN..." screen becomes "DESLIGANDO...". Nothing in the `.hsqs` image or the
extracted firmware is changed (the translated files live in `run/<name>/lang/` and are
mapped over the originals). `--no-ptbr` keeps the original European Portuguese.

**In-game menu** — `Esc` or Select+Start on a gamepad pauses the game and opens a menu:
*Resume*, *Game list*, *Fullscreen* and *Turn off and quit*.

**Building** — Visual Studio 2022 (or Build Tools) with *Desktop development with C++*, then:

```powershell
git clone --recursive https://github.com/altiereslima/clover-emu.git
cd clover-emu
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
```

The script downloads SDL2 and Boost headers into `third_party/` and builds
`build\Console Classics.exe` with MSVC and Ninja. Started from a terminal, the exe attaches
to that console, so `--run` / `--nowindow` work there too.

## macOS

The original macOS build (Apple Silicon) is kept, but has not been re-tested since the
Windows port. It needs the Xcode command line tools and
`brew install cmake ninja sdl2 squashfs` (`unsquashfs` extracts the firmware):

```sh
git clone --recursive https://github.com/altiereslima/clover-emu.git
cd clover-emu
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

If you cloned without `--recursive`: `git submodule update --init --recursive`.

## Firmware

Dump the firmware from **your own** console — for example with
[hakchi2-CE](https://github.com/TeamShinkansen/Hakchi2-CE) — and copy the root filesystem
image (`.hsqs`) into the clover-emu folder, e.g.:

```
dp-nes-release-v1.0.7-*.hsqs        NES Classic Edition
dp-snesusa-release-v2.0.7-*.hsqs    Super NES Classic Edition
```

Only the root filesystem is needed — clover-emu stands in for the kernel, so no kernel
image is required.

## Running

Double-click `Console Classics.exe` (see [Windows](#windows)), drag a `.hsqs` onto it, or
pass the firmware image you want to boot — it works the same for both consoles:

```powershell
& ".\Console Classics.exe" dp-nes-release-v1.0.7-0-g4ea4041.hsqs        # NES Classic
& ".\Console Classics.exe" dp-snesusa-release-v2.0.7-0-geb2b275.hsqs    # Super NES Classic
```

(On macOS the program is `./build/clover-emu`.)

The first boot of an image extracts it to `firmware/<image name>/` (takes a few seconds);
every image gets its own save folder, `saves/<image name>/`, so consoles never share
saves. You can even run the NES and SNES side by side.

The window boots exactly like the hardware: splash screen, first-time language setup,
then the home menu.

### Controls

| Console button     | Keyboard                       | Gamepad (by position)          |
|--------------------|--------------------------------|--------------------------------|
| D-pad              | Arrow keys                     | D-pad / left stick             |
| A                  | `X` (or `K`)                   | right face button              |
| B                  | `Z` (or `J`)                   | bottom face button             |
| X (SNES)           | `S`                            | top face button                |
| Y (SNES)           | `A`                            | left face button               |
| L / R (SNES)       | `Q` / `W`                      | shoulder buttons               |
| SELECT             | Right Shift / Tab / Backspace  | Back / View                    |
| START              | Enter                          | Start / Menu                   |
| Pause menu         | `Esc`                          | Select+Start together          |
| RESET (to menu)    | `R` / Home                     | Guide (NES also: L shoulder)   |
| POWER off          | close the window               |                                |
| Fullscreen toggle  | `F11` / `Alt`+`Enter`          |                                |

Player 1 is the keyboard plus the first gamepad; player 2 appears when a second gamepad
is connected. On the NES, the top/left face buttons act as A/B too.

Gamepads go through SDL's GameController layer: XInput pads (Xbox 360/One/Series and
compatibles such as 8BitDo in X-input mode) as well as DualShock 4, DualSense and Switch Pro
controllers work out of the box, and they can be plugged in or out while running. For an
unusual pad, drop a `gamecontrollerdb.txt` from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) next to the executable.
The **pause menu** freezes the game and offers *Resume*, *Game list* (same as RESET),
*Fullscreen* and *Turn off and quit*. It is the gamepad's way out, since the Xbox Guide
button is often captured by the Windows Game Bar.

RESET works like the console's button: the game writes a suspend point and you return to
the menu. Closing the window slides the power switch off — the firmware saves and shuts
down cleanly (it may take a second or two).

### Command-line options

```
"Console Classics.exe" [options] [FIRMWARE.hsqs]      (macOS: clover-emu)

--image FILE      firmware image (same as the positional argument)
--root DIR        boot an already-extracted rootfs folder instead
--data DIR        persistent /var/lib folder (default: saves/<image name>)
--window WxH      window size in points (default 1280x720)
--windowed        start in a window (default: fullscreen)
--fullscreen      start fullscreen (the default)
--no-ptbr         keep the firmware's European Portuguese menus
--run 'CMD ARGS'  run a single guest program instead of booting /sbin/init
--nowindow        no window (useful with --run)
--fps             print the guest frame rate
--strace          log every guest syscall
--log LIST        extra logging: proc,vfs,hle,gl,signal,mem,dev,syscall
--shots DIR       write a screenshot every 2 s (debugging)
```

Example — poke around the firmware's shell:

```powershell
& ".\Console Classics.exe" dp-nes-release-v1.0.7-0-g4ea4041.hsqs --nowindow `
    --run "/bin/sh -c 'uname -a; cat /etc/clover/VERSION'"
```

### Where things are stored

| Path                           | Guest path                     | Contents                                         |
|--------------------------------|--------------------------------|--------------------------------------------------|
| `<name>.hsqs`                  | —                              | your firmware image                              |
| `firmware/<name>/`             | `/`                            | extracted firmware (read-only to the guest)      |
| `firmware/<name>.casefold/`    | (see below)                    | firmware paths that clash on a case-insensitive disk |
| `saves/<name>/`                | `/var/lib`                     | persistent: settings, saved suspend points, SRAM |
| `run/<name>/`                  | `/var`, `/tmp`, `/run`, `/sys` | recreated on every boot (tmpfs on the console)   |

Delete `saves/<name>/` for a factory-fresh console. Unsaved suspend points live in
`/var/cache` (tmpfs), so like on the real console they are lost at power-off unless you
save them to a slot.

Windows (NTFS) and macOS disks are usually case-insensitive, but the SNES firmware contains both
`/etc/clover/VERSION` and `/etc/clover/version/`. During extraction any such clashing
path is stored in `firmware/<name>.casefold/` and mapped back by exact case, so the guest
still sees both.

## Source layout

```
src/
  main.cpp        boot, options, firmware extraction, firmware/saves/run folders
  thread.cpp      guest threads, dynarmic glue, CP15, host→guest calls, precise sleeps
  memory.cpp      per-process 4 GiB guest address space
  loader.cpp      ELF loader, execve, initial stack/auxv, kuser helpers
  signals.cpp     signal delivery, sigreturn, process exit/reaping
  syscalls.cpp    syscalls: files, processes, signals, time, rlimits, sendfile
  syscalls2.cpp   syscalls: mmap, futex, sockets, poll/select/epoll, eventfd, AF_ALG
  vfs.cpp         guest path resolution, mounts, exact-case overrides, host-backed files
  devices.cpp     /dev (fb0, disp, console, …) and /proc
  hle.cpp         HLE registry, stub-library (ELF .so) generator, guest symbol lookup
  hle_sdl.cpp     libSDL2: window, GL contexts, render targets, input, events, audio
  hle_sdl2.cpp    libSDL2: threads, semaphores, atomics, WAV, surfaces, 2D renderer
  hle_gl.cpp      libGLESv2 → desktop OpenGL, vsync emulation
  hle_alsa.cpp    libasound PCM → SDL audio
  display.cpp     host window, fb0 presentation, frame pacing, input mapping, pause menu
  ipc.cpp         in-memory pipes, sockets, eventfd, poll/epoll
  hash.cpp        SHA-1 / SHA-256 for AF_ALG
  launcher.cpp    console chooser, first-run progress, in-game menu (drawn in the window)
  ui.cpp          localized dialogs, file picker, log file (Windows GUI program)
  lang_ptbr.cpp   Brazilian Portuguese menus and shutdown screen
  platform_*.cpp  threads, timers, shared memory (Windows / POSIX)
  hostfs_*.cpp    host file access, symlinks/FIFOs as marker files on Windows
  squashfs.cpp    built-in SquashFS extractor (Windows)
  gl_loader.cpp   OpenGL entry points beyond 1.1 (Windows)
res/              Windows icon, version info and manifest
third_party/
  dynarmic/       ARM JIT (git submodule)
```

## Limitations

- Tested with the USA NES Classic and USA SNES Classic firmware. Other regions
  (Famicom Mini, Super Famicom Mini, EUR) use the same software stack and will probably
  work, but are untested.
- No hakchi / USB-gadget features and no networking beyond what the firmware does locally.
- Rendering goes through legacy desktop OpenGL 2.1.
- On displays that are not ~60 Hz (120/144 Hz…) the console is paced by a timer, so you
  may very occasionally see a repeated frame.
- Known issue: the home menu does not reload its own settings file
  (`home-menu/save/system-save.json`), so it asks for the language at every boot and forgets
  menu options such as the sort order. Game saves and suspend points are not affected.
- clover-emu is not a security boundary — guest programs can read and write files inside
  the emulator's run/data folders.

## Credits

- [dynarmic](https://github.com/azahar-emu/dynarmic) — ARM dynamic recompiler (0BSD)
- [SDL2](https://www.libsdl.org/) — window, input and audio (zlib)
- [hakchi2-CE](https://github.com/TeamShinkansen/Hakchi2-CE) — the community tool for
  dumping your console's firmware

Nintendo, NES, Super NES and the Classic Edition consoles are trademarks of Nintendo.
This project is not affiliated with or endorsed by Nintendo.

## License

MIT — see [LICENSE](LICENSE). Third-party code under `third_party/` keeps its own license.
