# clover-emu

Run the **NES Classic Edition** and **Super NES Classic Edition** firmware on a Mac — the
real thing, not a re-implementation.

clover-emu boots a console's original Linux userland straight from its firmware image
(`.hsqs`): BusyBox `init`, the init scripts, Nintendo's `clover-mcp` system manager, the
Lua-driven home menu (`ReedPlayer-Clover`) and Nintendo's own emulators — `kachikachi`
(NES) and `canoe` (SNES). Every one of those programs is the original 32-bit ARM binary,
executed unmodified. Video, audio and controllers are bridged to the Mac with SDL2 and
OpenGL.

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
                     macOS · SDL2 · OpenGL
```

- **CPU** — [dynarmic](https://github.com/azahar-emu/dynarmic) JIT-compiles ARMv7
  (ARM, VFPv4, NEON) to ARM64. Each guest thread gets its own JIT instance; each guest
  process its own 4 GiB address space (fastmem) and exclusive monitor for
  `ldrex`/`strex`. The kernel "kuser helper" page is provided.
- **Kernel** — ~200 Linux ARM EABI syscalls. All guest processes live inside one host
  process, so `fork` (copy-on-write via `vm_copy`), `vfork`, `execve` (including `#!`
  scripts), `wait4`, `kill`, process groups, resource limits and signal delivery (proper
  `rt_sigframe`s, `SA_RESTART`) are emulated in-process — which keeps a single window
  across the menu → game → menu handoffs. The kernel crypto socket API (`AF_ALG`, used
  for SHA-1 checks of ROMs and suspend points) is backed by CommonCrypto.
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
for a precise 60 Hz tick (SNES). Guest threads run at macOS's user-interactive QoS so
`nanosleep`-based frame pacing in the home menu stays accurate.

## Requirements

- macOS on Apple Silicon (developed and tested on arm64)
- Xcode command line tools (`xcode-select --install`)
- Homebrew packages:

```sh
brew install cmake ninja sdl2 squashfs
```

`squashfs` provides `unsquashfs`, used once to extract a firmware image.

## Building

```sh
git clone --recursive https://github.com/ravensclawx/clover-emu.git
cd clover-emu
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

If you cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

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

Pass the firmware image you want to boot — it works the same for both consoles:

```sh
./build/clover-emu dp-nes-release-v1.0.7-0-g4ea4041.hsqs        # NES Classic
./build/clover-emu dp-snesusa-release-v2.0.7-0-geb2b275.hsqs    # Super NES Classic
```

If there's only one `.hsqs` in the folder, plain `./build/clover-emu` boots it.

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
| RESET (to menu)    | `Esc` / `R` / Home             | Guide (NES also: L shoulder)   |
| POWER off          | close the window               |                                |
| Fullscreen         | `F11`                          |                                |

Player 1 is the keyboard plus the first gamepad; player 2 appears when a second gamepad
is connected. On the NES, the top/left face buttons act as A/B too.

RESET works like the console's button: the game writes a suspend point and you return to
the menu. Closing the window slides the power switch off — the firmware saves and shuts
down cleanly (it may take a second or two).

### Command-line options

```
clover-emu [options] [FIRMWARE.hsqs]

--image FILE      firmware image (same as the positional argument)
--root DIR        boot an already-extracted rootfs folder instead
--data DIR        persistent /var/lib folder (default: saves/<image name>)
--window WxH      window size in points (default 1280x720)
--fullscreen      start fullscreen
--run 'CMD ARGS'  run a single guest program instead of booting /sbin/init
--nowindow        no window (useful with --run)
--fps             print the guest frame rate
--strace          log every guest syscall
--log LIST        extra logging: proc,vfs,hle,gl,signal,mem,dev,syscall
--shots DIR       write a screenshot every 2 s (debugging)
```

Example — poke around the firmware's shell:

```sh
./build/clover-emu dp-nes-release-v1.0.7-0-g4ea4041.hsqs --nowindow \
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

macOS disks are usually case-insensitive, but the SNES firmware contains both
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
  display.cpp     host window, fb0 presentation, input mapping
third_party/
  dynarmic/       ARM JIT (git submodule)
```

## Limitations

- Tested with the USA NES Classic and USA SNES Classic firmware. Other regions
  (Famicom Mini, Super Famicom Mini, EUR) use the same software stack and will probably
  work, but are untested.
- No hakchi / USB-gadget features and no networking beyond what the firmware does locally.
- Rendering goes through macOS's legacy OpenGL 2.1 (deprecated by Apple, still fully
  functional on current macOS).
- The emulated 60 Hz refresh isn't locked to the Mac display's refresh, so on a 60 Hz
  screen you may very occasionally see a repeated frame.
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
