# android32

**The shared runtime of the 32-bit Android game ports for Nintendo Switch**

The ports run the 32-bit (armeabi / armeabi-v7a) Android builds of games on
the Switch. They load the game's own native libraries and provide what those
libraries expect from Android. This repository is the part every port has in
common. Each port keeps only its game's code and adds this as `runtime/`.

Ports using it:
[dcr_sea_nx](https://github.com/aks796/dcr_sea_nx) (Disney Crossy Road),
with Labyrinth 2, Angry Birds Space, PvZ Touch, Sonic & SEGA All-Stars Racing,
Flappy Birds Family and Asphalt 8: Airborne Retry moving over.

---

## What you get

* **Loader:** maps a game's `.so` files into a 32-bit Switch process, binds
  their imports and runs their constructors. It includes the relocations a
  32-bit PIE program needs before `main`, the code-memory views, and kernel
  helper stubs.
* **bionic:** Android's C library on top of newlib and libnx. Threads,
  condition variables, clocks that skip the HOME menu and sleep, files and
  paths inside the game's SD folder, sockets, memory maps, signals, zlib,
  wide characters and the C++ ABI helpers.
* **JNI:** a Java VM with no Java: classes, objects, strings, arrays and
  fields. The port's tables answer the game's calls.
* **Android NDK:** loopers, the window, asset and configuration shims, and
  OpenSL ES on audout for ports that need it.
* **Graphics:** EGL and GLES on mesa32 (nouveau), or a null renderer. Frame
  captures and self-tests are included.
* **The rest of a port's process:**
  * `main()` and setup: finds the player's APK by what is in it, unpacks the
    libraries once with a progress bar, and updates itself from a newer NRO;
  * `config.ini`;
  * HOME/sleep handling;
  * CPU boost for long frames;
  * a watchdog that reports hangs;
  * a crash handler that writes `crash.log`;
  * the log;
  * audio output and controller helpers.
* **The launcher:** the 64-bit NRO that installs the 32-bit program for a
  sphaira forwarder's icon.

---

## Using it in a port

The port adds this repository at `runtime/` (a git submodule) and keeps:

* `source/port_config.h`: the game's name, folder and Android package, and
  any setting that differs from the default;
* its own source files: `port_load()` and `port_run()`, the JNI tables, the
  setup plan, the `config.ini` options and whatever the game needs;
* a three-line `Makefile` that includes `runtime/runtime.mk`;
* `launcher/Makefile`, its icon, and any extra files for the NRO.

A port file with the same name as a runtime file replaces it. Weak functions
named `port_*` let a port change one behaviour without copying a file.

[docs/DESIGN.md](docs/DESIGN.md) explains how it fits together and
[docs/MIGRATING.md](docs/MIGRATING.md) how a port moves onto it. The settings
and callbacks of each part are in [docs/notes/](docs/notes).

---

## Building

A port builds it, with:

* Docker and the AArch32 toolchain image `ghcr.io/vita2hos/devcontainer/vita2hos`
* [libnx32](https://github.com/aks796/libnx32) 4.12.0 or newer, next to the port
  or where `DCR_LIBNX32` points
* [mesa32](https://github.com/aks796/mesa32): its `lib/` and `include/` in the
  port's `portlibs32/`
* `devkitpro/devkita64` for the launcher

`tools/check.sh` compiles every runtime file against a test port, to check a
change without a port.

---

## Credits

The `.so` loader derives from the Switch and Vita loader work of Andy Nguyen
(TheOfficialFloW) and fgsfds, ported to 32-bit with reference to
[vita2hos](https://github.com/xerpi/vita2hos) by xerpi. libnx is by the
switchbrew authors. Graphics use Mesa and libdrm_nouveau with devkitPro's
Switch patches.

The code was merged from seven ports' copies, each tested on hardware.

---

## License

MIT, see [LICENSE](LICENSE). `so_util.c` (MIT, Andy Nguyen and fgsfds) and
`nx32_virtmem.c` (ISC, libnx authors) keep their notices.
