# android32

**The shared runtime of the 32-bit Android game ports for Nintendo Switch**

The ports run the 32-bit (armeabi / armeabi-v7a) Android builds of games on
the Switch. They load the game's own native libraries and provide what those
libraries expect from Android. This repository is the part every port has in
common. Each port keeps only its game's code and adds this as `runtime/`.

Ports using it:

| Port | Game |
| --- | --- |
| [dcr_sea_nx](https://github.com/aks796/dcr_sea_nx) | Disney Crossy Road |
| [labyrinth2_nx](https://github.com/aks796/labyrinth2_nx) | Labyrinth 2 |
| [pvz_touch_nx](https://github.com/aks796/pvz_touch_nx) | Plants vs. Zombies Touch |
| [abspace_nx](https://github.com/aks796/abspace_nx) | Angry Birds Space |
| [a8retry_nx](https://github.com/aks796/a8retry_nx) | Asphalt 8: Airborne Retry |
| [flappybirdsfamily_nx](https://github.com/aks796/flappybirdsfamily_nx) | Flappy Birds Family |
| [sonic_allstars_nx](https://github.com/aks796/sonic_allstars_nx) | Sonic & SEGA All-Stars Racing |
| [ducktales_nx](https://github.com/Thorhax/Ducktales-NX) | DuckTales: Remastered |

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
* **Graphics:** EGL and GLES on Mesa's nouveau driver (mesa32 or
  mesa-switch32), or a null renderer. Frame captures, self-tests and a log of
  the first GL errors are included.
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
* Mesa in the port's `portlibs32/` (its `lib/` and `include/`), either
  [mesa32](https://github.com/aks796/mesa32) (Mesa 20.1) or
  [mesa-switch32](https://github.com/aks796/mesa-switch32) (Mesa 26.2). The
  build sees which one is there and links it (`PORT_MESA` in `runtime.mk`).
  Mesa 26.2 needs the cache syscalls 0x5D-0x5F in the NPDM, and
  `npdm.json.in` allows them.
* `devkitpro/devkita64` for the launcher

`tools/check.sh` compiles every runtime file against a test port, to check a
change without a port.

---

## Credits

The `.so` loader derives from the Switch and Vita loader work of Andy Nguyen
(TheOfficialFloW) and fgsfds, ported to 32-bit with reference to
[vita2hos](https://github.com/xerpi/vita2hos) by xerpi. libnx is by the
switchbrew authors. Graphics use Mesa: with libdrm_nouveau and devkitPro's
Switch patches (mesa32), or with danfromtico's mesa-switch Horizon backend
(mesa-switch32).

The code was merged from seven ports' copies, each tested on hardware.

---

## License

MIT, see [LICENSE](LICENSE). `so_util.c` (MIT, Andy Nguyen and fgsfds) and
`nx32_virtmem.c` (ISC, libnx authors) keep their notices.
