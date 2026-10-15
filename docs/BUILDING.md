# Building

This is the procedure. When a step's *reason* matters -- why the ELF is padded,
why the recompiler runs under Linux, why the audio microcode loads at `0x1080` --
[PORTING.md](PORTING.md) explains it, and [GAME-INTERNALS.md](GAME-INTERNALS.md)
records what the game itself turned out to be.

The port builds for **Windows, Linux and macOS** from this one tree. Linux and
macOS have a script each that does the whole thing from a clean clone:

```sh
bash tools/setup_linux.sh --install      # or tools/setup_macos.sh
bash tools/build_linux.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
```

Windows is still step by step, below. Everything from *Clone* onwards applies to
all three; the per-platform prerequisites are in the next section.

## Requirements

| Tool | Version | Why |
|---|---|---|
| Git | any recent | submodules |
| CMake | 3.20+ | required by N64Recomp and the runtime |
| Ninja | any | the generator every upstream project assumes |
| Clang | 15+ | N64Recomp's output is validated against Clang and MSVC |
| Python | 3.10+ | splat and the symbol tooling in `tools/` |
| MIPS binutils | any | assembling the disassembly into an ELF (phase 01) |

Per-platform prerequisites and the one-command build are below: [Windows](#windows),
[Linux](#linux), [macOS](#macos).

**Do not build with modern GCC.** Recompiled output has a documented history of
miscompiling under modern GCC's optimizer. CMake will warn if you try.

### Windows

Verify what you have:

```powershell
pwsh -File tools/check_toolchain.ps1
```

Install what you don't (run in an elevated terminal):

```powershell
winget install --id Kitware.CMake
winget install --id Ninja-build.Ninja
winget install --id LLVM.LLVM
winget install --id Python.Python.3.12
winget install --id Microsoft.VisualStudio.2022.BuildTools `
    --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The Build Tools install supplies the Windows SDK and the C runtime that Clang
links against on Windows; Clang alone is not enough.

A MIPS assembler is not available through winget. Phase 01 needs one of:

- **WSL** (`sudo apt install binutils-mips-linux-gnu`) — simplest, and what most
  decompilation projects assume.
- A prebuilt `mips64-elf` toolchain on `PATH`.

The Microsoft Store `python` stub on a fresh Windows install is not Python. If
`python --version` opens the Store, install the real thing with the winget
command above.

### Linux

Tested on Ubuntu 26.04 x86-64. The packages, and what each is for:

| Package | Needed by |
|---|---|
| `clang`, `lld` | the port. Not GCC -- see above. A versioned `clang-21` counts: Debian and Ubuntu ship those without the unversioned metapackage, and `build_linux.sh` picks the highest it finds. `WR64_CC`/`WR64_CXX` override the choice |
| `cmake`, `ninja-build`, `pkg-config` | the build |
| `libsdl2-dev` | the window, the pad, the audio device |
| `libfreetype-dev` | RmlUi's font engine, under `recompui` |
| `libgtk-3-dev` | `nativefiledialog-extended`, which RT64 opens the ROM picker with |
| `libvulkan-dev`, `mesa-vulkan-drivers` | RT64's only renderer here. Skip the drivers if the proprietary NVIDIA or AMD stack is installed |
| `vulkan-tools` | `vulkaninfo`, for when the renderer will not start |
| `binutils-mips-linux-gnu` | assembling the disassembly into an ELF |
| `python3`, `python3-venv` | splat and the symbol tooling in `tools/` |

```sh
bash tools/setup_linux.sh            # prints what is missing
bash tools/setup_linux.sh --install  # installs it, with sudo
```

`setup_linux.sh` also initialises the submodules, clones the reference
decompilation into `reference/`, and builds the Python environment splat runs
in. Then:

```sh
bash tools/build_linux.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
./build-linux/WaveRace64Recomp
```

The dump argument is needed on the first build only; after that
`bash tools/build_linux.sh` rebuilds from source alone. **Pass it again when
`recomp/wr64.toml` changes** (on any platform, `python tools/generate_game.py
<dump>`): its hooks go into the generated sources, so an older `RecompiledFuncs/`
still builds but without them -- the course buoy fix is one. RT64 renders through
Vulkan here, so a working Vulkan driver is not optional -- `vulkaninfo --summary`
should name your GPU.

**Verified** on Ubuntu 26.04 under WSL2/WSLg, Clang 21.1.8, CMake 4.2.3: builds
and links, `--identify` validates a dump, and the port boots, opens its window,
draws, walks the menus and runs the attract race. The only Vulkan device there
is Mesa's `llvmpipe` software rasteriser -- no GPU is passed through -- and even
so the game held **14-21 fps against the 20 it asks for**, with water at
Original. That is a correctness result, not a performance one: on llvmpipe
nothing about frame cost generalises.

**Re-verified** with the sea extension and the draw distance at Extended (then
called Maximum): builds
with no warnings beyond the vendored libraries' CMake deprecations, boots, walks
the menus, reaches the attract race at **16-20 fps**, and emits the ring --
`48 sectors x 9 circles, 860 to 16192 units` -- with zero errors. That one line
exercises the whole of the new path on Linux: the game-thread sampling through
`func_8004D30C`, the publish and claim across threads, and the GBI emission.

**Keep water at Original when testing here**, which is the default. On llvmpipe
the modern renderer is roughly ten times slower than the game's own path -- a run
at Best reached main loop tick 1,000 in 900 seconds where Original reached 10,000
in a fraction of that, and never got past the title screen. That is a fact about a software rasteriser, not about the
renderer.

Settings, saves and mods live in `$XDG_DATA_HOME/WaveRace64Recomp`, or
`~/.local/share/WaveRace64Recomp`.

### macOS

Apple Silicon, macOS 15 or newer. **Not tested by this project's maintainers**:
the macOS support here comes from [PR #2][macos-pr], which was built and played
on an M3 Max, and is kept building but not run on a Mac by anyone here. Treat a
macOS-only failure as a real bug and report it.

[macos-pr]: https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2

Prerequisites, from Xcode and Homebrew:

```sh
brew install cmake ninja python sdl2 freetype
```

Xcode itself is needed for the **Metal Toolchain** -- RT64 compiles its shaders
to Metal with it, and the command line tools alone do not include it. If the
build reports it missing:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
    xcodebuild -downloadComponent MetalToolchain
```

MIPS binutils has no formula worth relying on, so `setup_macos.sh` builds it
from checksum-pinned source into `build-toolchain/`. That takes a few minutes,
once.

```sh
bash tools/setup_macos.sh
bash tools/build_macos.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
open build-macos/WaveRace64Recomp.app
```

The product is a double-clickable bundle, ad-hoc signed for local use and not
notarized. `tools/package_macos.py` -- which `build_macos.sh` runs -- copies the
non-system dylibs and frameworks into `Contents/Frameworks`, rewrites their
install names, and signs from the inside out.

CMake is told `CMAKE_FIND_FRAMEWORK=LAST` so Homebrew's `libSDL2.dylib` is
chosen over a leftover `~/Library/Frameworks/SDL2.framework`. The framework
build of SDL2 is typically an `@rpath` binary; linking it without adding an
`LC_RPATH` makes the app abort at launch with "Library not loaded".

Settings, saves and mods live in `~/Library/Application Support/WaveRace64Recomp`.
The dump stays outside the bundle; pick it in the launcher and it is remembered.

## Clone

```
git clone --recurse-submodules <this repo>
```

If you already cloned without submodules:

```
git submodule update --init --recursive
```

## The reference decompilation

The disassembly step runs inside a checkout of the Wave Race 64 decompilation
by LLONSIT and contributors, whose splat configuration, symbol tables and
linker scripts it uses. Clone it into `reference/` (which is ignored) and put
your dump beside its config under the name it expects:

```
git clone https://github.com/LLONSIT/Wave-Race-64.git reference/wr64-decomp
copy your.z64 reference\wr64-decomp\baserom.us.rev1.z64
```

Nothing from that checkout is committed here. The splat config this project
runs, `recomp/wr64.us.rev1.asm.yaml`, is derived from the decompilation's at
build time by `tools/make_asm_only_yaml.py` and is ignored as well; the
pipeline scripts generate it when it is missing.

## Configure and build

The project is phase-gated so a partially finished tree always builds. Phase 00
needs no options:

```
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
```

Later phases turn on what they earn:

| Option | Turned on by | Effect |
|---|---|---|
| `WR64_BUILD_RECOMPILER` | phase 02 | builds the N64Recomp tool from `lib/` |
| `WR64_WITH_RECOMPILED` | phase 02 | compiles the generated `RecompiledFuncs/` |
| `WR64_WITH_RUNTIME` | phase 03 | links ultramodern, librecomp and RT64 |

## Verify your dump

```
./build/WaveRace64Recomp --identify path/to/your.z64
```

Exit code 0 means the dump matches the pinned target. Anything else means stop
and fix that before continuing — a mismatched revision produces failures later
that look like tooling bugs.

## Known issues

### CMake 4.x and old vendored dependencies

CMake 4 removed compatibility with `cmake_minimum_required(VERSION <3.5)` and
hard-errors on it. Six files under `lib/RT64/src/contrib` still declare a lower
minimum:

```
imgui/examples/example_glfw_vulkan   2.8
implot/.github                       3.0
mupen64plus-win32-deps/SDL2*/cmake   3.0
re-spirv/external/SPIRV-Headers/...  3.0
```

All six are example, CI, or `find_package` config files that RT64's build does
not normally descend into, so this is expected to be harmless. If configure
fails naming one of them, the workaround is:

```
cmake -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ...
```

Do not "fix" this by editing the submodules.

### Clang is not added to PATH by its installer

`winget install LLVM.LLVM` installs to `C:\Program Files\LLVM\bin` without
registering it. Add that directory to your user PATH.

## Building with the runtime (phase 03 onward)

Apply the project's patches to RT64 first (see *The game's own black borders*
below for what they do and why they are a script). This also applies the
inspector hook from `tools/patch_rt64_inspector.py`, which the port links
against, so the build always needs it -- the debug menu it feeds is on F1 in
every build (see [HUD-INSPECTOR.md](HUD-INSPECTOR.md)):

```
python tools/patch_rt64.py
python tools/patch_runtime_shutdown.py
```

`patch_rt64.py` runs every other RT64 patch itself, including the modern water
renderer (`patch_rt64_water.py`) -- the port does not compile without the
headers that one adds, whatever the water setting is. `patch_runtime_shutdown.py`
is separate only because it patches N64ModernRuntime rather than RT64; it joins
the runtime's workers before the shutdown path frees what they are reading. The
per-platform build scripts run both.

On Windows, use **clang-cl**, not `clang++`, once `WR64_WITH_RUNTIME=ON`. On
Linux and macOS, plain `clang`/`clang++` is right and `clang-cl` does not exist;
the reason for the Windows rule is below and does not apply there.

```
cmake -B build-rt -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON
cmake --build build-rt
```

**Always build optimized** (`RelWithDebInfo`: optimized, with the symbols the
crash handler needs). The project defaults to it when no build type is given,
but a build type already in a directory's cache wins over that default, and a
Debug build of this port is not merely slow -- it breaks the audio. The
recompiled audio microcode is run on a thread of its own, once per frame, and
the game double-buffers its command lists on the assumption that the RSP is
done with a list long before that buffer's turn comes round again, two frames
later. Unoptimized, one task takes 5 to 24 ms (measured) against a 16.7 ms
frame, so the game regularly starts rewriting a list the microcode is still
reading; the microcode then sees a splice of two frames' commands, and one
particular splice -- an ENVMIXER inheriting the frame-end SAVEBUFF's sample
count -- walks a buffer off the end of DMEM onto the command jump table. That
is the "audio frame dropped" click. Optimized, a task takes well under a
millisecond and the window closes. See docs/findings/phase-05.md.

RT64 decides its warning flags from `CMAKE_CXX_SIMULATE_ID`: a Clang targeting
the MSVC ABI gets `/W4`, on the assumption that such a Clang is `clang-cl`. That
assumption is wrong for `clang++`, which targets the same ABI through the GNU
driver and rejects `/W4` outright, so the whole of RT64 fails to compile. Using
the driver RT64 expects is simpler than patching the submodule.

Changing compiler invalidates the CMake cache, and the options do not survive
it. Delete the build directory when switching rather than reconfiguring in
place, or the build will silently come back with the runtime switched off.

## Recompiling the audio microcode (phase 05 onward)

The game's audio is mixed by the RSP, so the port needs the cartridge's audio
microcode recompiled alongside the game code. That is a second tool with its own
config, run once after the recompiler is built:

```
python tools/patch_rsprecomp.py
wsl bash tools/wsl_build_recompiler.sh
wsl bash tools/wsl_recompile_rsp.sh
```

This writes `RecompiledFuncs/aspMain_rsp.cpp`, which the CMake build picks up
automatically and compiles with SSSE3 and SSE4.1 enabled -- librecomp's RSP
vector unit is written against those intrinsics. Like everything else under
`RecompiledFuncs/`, the file is derived from your dump and is not committed.

Without it the port still runs; it is simply silent, because the RSP callback
falls back to reporting each audio task complete without running it.

## Playing it

Double-click the program -- `build-fe\WaveRace64Recomp.exe` on Windows,
`build-linux/WaveRace64Recomp` on Linux, `build-macos/WaveRace64Recomp.app` on
macOS -- and pick your dump in the launcher; it is remembered. From a shell, a
path skips the launcher:

```
build-fe/WaveRace64Recomp.exe <your dump>.z64
```

Started by double-click, everything the port prints goes to `wr64.log` in the
settings directory below, and no console window opens. Started from a shell it
prints to the shell as before, so redirecting stderr to a file still works.

It can be started from any directory, by double-click or from a shell, and a
relative path to the dump is taken relative to where you typed it. Settings,
controller profiles and mod state live beside the files RT64 keeps there, in:

| | |
|---|---|
| Windows | `%LOCALAPPDATA%\WaveRace64Recomp` |
| Linux | `$XDG_DATA_HOME/WaveRace64Recomp`, or `~/.local/share/WaveRace64Recomp` |
| macOS | `~/Library/Application Support/WaveRace64Recomp` |

The first line the port prints says which. A file called `portable.txt` next to
the executable keeps them next to the executable instead.

A gamepad is used if one is attached, and the keyboard works at the same time --
neither has to be chosen, and player one always has both. Those bindings are
defaults and every one is rebindable in the controls tab; a keyboard profile
saved by an earlier build keeps whatever it holds until it is reset there.

On the keyboard: arrow keys are the analog
stick, `X` is A, `C` is B, `Z` is Z, `Enter` is Start, `A` and `S` are the
shoulder buttons, `I`/`J`/`K`/`L` are the C buttons (the camera) and
`T`/`F`/`G`/`H` are the D-pad.

The window is sized to the largest whole multiple of 320x240 that fits the
display, and is resizable.

Two things help when reporting a problem. The port prints a transcript of the
game's own state to stderr -- title screen, main menu, rider select, racing --
so redirecting stderr to a file says where it got to. And
`tools/capture_window.ps1` photographs the window at intervals:

```
powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 -OutDir shots -Count 12 -IntervalSeconds 5
```

For a repeatable session without touching the pad, `WR64_INPUT_SCRIPT` points at
a file of timed inputs; `tools/scripts/race.txt` drives the game from boot into
a race. Leave the variable unset and nothing is injected.

## Packaging a release

One script per platform, each producing an archive under `dist/` plus its
SHA-256. Each puts the license text of every third-party component the release
contains into its `licenses/` folder, from `tools/third_party_licenses.txt`, and
stops if one of those files is missing. None of them will package a dump or a save -- they refuse to continue
if they find one where they are staging -- and the Unix one refuses to overwrite
an archive that already exists, because a published checksum should not quietly
start describing different bytes.

```
powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -BuildDir build-fe -Version 1.0.2
python3 tools/package_release.py --version 1.0.2
```

The second one reads the platform it is running on: a `.tar.gz` on Linux with
the executable, the assets, a launcher and a note about the distribution
packages it needs at run time; a `.zip` on macOS holding the signed `.app`,
written with `ditto` so the bundle's symlinks and the executable bit survive.
Debug info is split into a second archive where an `objcopy` is available.

### A redistributable macOS build

A local macOS build links against Homebrew's SDL2 and FreeType, and a Homebrew
bottle is built for the OS it was installed on -- so the bundle ends up
advertising a minimum macOS newer than the one it was compiled for, and refuses
to launch on exactly the systems it claims to support.
`tools/package_macos.py` derives `LSMinimumSystemVersion` from the binaries
actually shipped, so this is visible rather than silent. To fix it, build the
libraries against the same deployment target:

```sh
bash tools/build_macos_dependencies.sh
WR64_BUILD_DIR=build-macos-release \
WR64_DEPENDENCY_PREFIX="$PWD/build-macos-deps/install" \
    bash tools/build_macos.sh
python3 tools/package_release.py --version 1.0.2 --build-dir build-macos-release
```

SDL2, FreeType and libpng are pinned by version and by SHA-256 there, and the
script checks each built dylib's architecture and deployment target before it
finishes. Test the extracted archive before publishing it.

## The frontend UI

`lib/RecompFrontend` is the shared library every N64: Recompiled port uses for
the parts that are not game-specific: `recompinput` for controller mapping,
rebindable keys and per-device profiles, and `recompui` for the config and mod
menus, built on RmlUi and drawn through RT64. `WR64_WITH_FRONTEND` is off by default in
CMake; the build scripts and the releases turn it on.

Apply the project's patch to it first -- it adds the call that puts the first pad
on player one without going through the assignment modal (see
[PORTING.md](PORTING.md), *Players, pads and profiles*):

```
python tools/patch_recompinput.py
```

```
cmake -B build-fe -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
cmake --build build-fe
```

Three things were needed to build it outside the tree it was written in, all in
this project's `CMakeLists.txt` rather than as submodule patches:

- `sdl2_SOURCE_DIR` points at the SDL2 that RT64 already vendors. The frontend
  expects the variable FetchContent would have set, and two SDL2s in one process
  is not a thing that ends well.
- `__PRFCHWINTRIN_H` is defined for the frontend's directory. SDL 2.26 works
  around an old Clang bug by defining `_m_prefetch`; current Clang has it as a
  builtin, so the workaround is now the error. RT64 defines this guard for its
  own targets and the frontend does not inherit it.
- RT64's `DXC` variable and its option lists are re-declared. `recompui`
  compiles its own HLSL through RT64's shader functions, which are global, but
  the variables they expand are set inside RT64's directory -- so from a sibling
  directory the shader command line begins with the `.hlsl` file and no
  compiler, produces nothing, and fails a step later on an empty file.

`patches/ui_funcs.h` exists because `recompui` includes it by a hardcoded
relative path out of the library and into the port. Upstream marks it "TODO:
Forced game includes".

Two globals are declared `extern` inside recompui and defined by the port, in
`src/frontend.cpp`: `supported_games` and the SDL `window`.

### What the port supplies, and what it does not

The menus look and behave the same across every N64: Recompiled port because
recompui carries the theme in code -- a palette of named colours with the same
defaults the other recomps use -- and its elements style themselves from it. A
port that writes its own colours into `assets/recomp.rcss` does not restyle
those elements, it competes with them. So that file supplies the font and
nothing else. To restyle the menus, set theme colours from C++ with
`recompui::theme::set_theme_color`.

The font family in the stylesheet is the name declared inside the `.ttf`, not
its filename: `LatoLatin-Regular.ttf` calls itself `LatoLatin`. A mismatch fails
silently and strangely -- every element lays out and draws in the right place,
with no text in any of them.

`recompui::programconfig::set_program_name` and `set_program_id` must both be
called before anything is built. The launcher throws from its constructor
otherwise, and the process dies with nothing on stderr but `abort() has been
called`.

### Assets the port owns

`assets/` holds what recompui asks the port for and does not ship itself:

- `recomp.rcss`, the stylesheet, which is the font rule and nothing else.
- `promptfont/promptfont.ttf`, the controller and keyboard glyphs recompui draws
  input prompts with. PromptFont by Yukari "Shinmera" Hafner, v1.10, SIL Open
  Font License 1.1. Committed rather than fetched during the build, because a
  build that needs the network is a build that fails without one. See
  `assets/promptfont/README.md`.
- `icons/`, eleven SVGs recompui loads by name: Caret, Cont, Keyboard,
  PlusKeyboard, Question, Quit, RecordBorder, RecordSpinner, Reset, Trash, X.
  A missing icon is easy to miss and confusing when noticed: the control that
  uses it is still laid out, focusable and clickable, and draws nothing.

The icons are drawn for this project rather than taken from another port. The
other N64: Recompiled ports are GPL-3.0 and this repository is MIT, so their
assets cannot simply be copied in without changing the licence of this one.

Lato and Noto Emoji are not in `assets/`: the build copies them out of the RmlUi
submodule, which this repository already has.

## Display resolution and aspect ratio

The port opens fullscreen at the display's own resolution and aspect ratio, and
the game is rendered widescreen rather than pillarboxed.

Most of that is RT64's, exposed through the Graphics tab: **Resolution: Auto**
renders at the window's true pixel size instead of upscaling 320x240, and
**Aspect Ratio: Expand** widens the frustum to the window's shape. Both are
already the library's defaults. Two things had to change here.

**Window Mode** defaults to Windowed upstream. It is set to Fullscreen on a
first run only -- after `finalize()` has loaded the player's settings, and only
when no `graphics.json` exists -- so choosing Windowed in the menu sticks.

**The window is created at the display's size when fullscreen.** This is the one
that actually made widescreen work. RT64 derives the aspect ratio it expands
into from the swap chain's dimensions, and the window was being created at a
whole multiple of 320x240 -- a 4:3 swap chain, into which "Expand" expands
nothing. The menu said Expand and the game stayed 4:3.

### The game's own black borders

Wave Race 64 never draws to its whole 320x240 framebuffer. Every frame --
title, attract, menus, racing -- is drawn inside the region from (8, 20) to
(311, 219), a 303x199 window with black borders around it that a CRT's overscan
was meant to hide. Nothing hides them on a modern display: presented as-is
they are a black bar across the top tenth of the picture and, once the frame is
widened, forty pixels down each side. The values are the scissor the game sets,
read from RT64 during the title screen, the attract sequence and a championship
race; all three agree (`include/wr64/display.h`).

Two things in RT64 go wrong because of that region, and `tools/patch_rt64.py`
patches both. **Run it before configuring** -- it is scripted because it
patches a submodule, and a submodule update would revert it silently:

```
python tools/patch_rt64.py
```

**The frame was treated as "not 4:3" and its HUD stretched.** RT64 widens the
3D frustum for widescreen but keeps 2D content at its original shape in the
middle of the frame. It decides whether a framebuffer is the game's main 4:3
frame by comparing the scissor's shape to 4:3, within 10%; 303x199 is 1.52,
14% off, so every HUD element was stretched across the widened frame instead,
and the speed readout ended up at the far right edge and cut off. The patch
also accepts a scissor covering three quarters of the framebuffer's width and
height as the main frame, whatever its shape.

**The borders were presented as black bars.** The final blit mapped the whole
framebuffer to the window. The patch lets the port name the region the game
draws into (`wr64::display::crop_to_content()`, called as the window is
created), and the blit then scales that region to fit the window instead.
The region is not the same in every mode: with two players the game draws
into (8, 12)-(311, 229), taller than the single-player region, and a crop
pinned to the latter cut the top and bottom off the split screen in 0.1. The
display-list rewriter (`src/dlrewrite.cpp`) now reads the scissor the game
sets each frame and moves the crop to follow it, once the region has held
for a few frames.
Vertically it fits exactly; horizontally the widened frame has more picture
than the region, and the window shows as much of it as its shape allows -- the
frame a wider CRT would have shown, with nothing black around it. The HUD sits
at 4:3 inside that, where the cartridge put it.

**A 3D pass that covers the drawn region is not always widened.** RT64 widens
a 3D pass only when it reaches both edges of the frame it draws into. This
game scissors its world to (8, 20)-(311, 219), and in most frames nothing else
touches the framebuffer, so the frame's scissor is that same region and the
test passes. In the championship's warm-up round something else in the frame
touches the whole 320x240 framebuffer: the frame's scissor becomes the whole
thing, the world falls eight pixels short at each end, and the warm-up alone
was rendered at 4:3. The patch allows a sixteenth of the frame's width in
tolerance, which is more than the border and far less than the inset boxes the
select screens draw their models into. RT64 asks the question in two places --
once to render the pass across the widened frame, once to widen the frustum
that fills it -- and both are patched; answering only the first stretches the
game's 4:3 frustum across a wide viewport, which looks like a stretched image
rather than a wider view.

**HUD Placement** in the Graphics tab chooses whether the race HUD is anchored to
the frame's edges or kept at 4:3 in the middle. The cartridge names no edges
itself, so the display-list rewriter supplies them ([PORTING.md](PORTING.md) §7).

## Frame rate

**Framerate** in the Graphics tab is RT64's interpolation, the same mechanism
the other recompiled ports use. *Original* shows the game's own frames.
*Display* presents at the display's refresh rate and *Manual* at a chosen rate,
capped at the display's; in both, RT64 draws the frames in between two game
frames by interpolating each object's transform -- position, rotation and scale
-- from one to the next. The game itself is untouched: Wave Race 64's physics,
camera and timers run at the rate the cartridge chose, and that rate varies.
The game writes a divider that its video-interrupt handler counts retraces
against: 3 in races, the menus and the attract demo (20 frames per second), 2 on
the course select and results screens (30), 1 briefly at boot (60). RT64 measures it from the swaps and follows
it as it changes.

Nothing in the game had to be tagged for this. RT64 pairs each object's matrix
with the previous frame's on its own, by matching draw calls of the same
combiner, render mode and triangle count and then by nearest position, and in
the title, menus and attract demo it pairs about 98% of transforms per frame.
The remaining few are drawn where the newer frame puts them, so an object that
fails to pair judders at the game's rate against a smooth background rather than
smearing. Anything the game rebuilds every frame from vertices -- the water
surface's waves, the HUD -- animates at the game's rate; the camera's movement
over the water is smooth because the water is drawn in world space under the
interpolated view. Zelda 64: Recompiled tags every matrix with the actor it
belongs to from inside the game's code; that needs the drawing code to be
decompiled, and two thirds of this game's is not. Where objects paired badly the
port rewrites the display list before RT64 sees it: the sky and the water carry
explicit ids, a rider's parts are paired by their matrices' addresses, and a
camera only continues one that drew into the same part of the screen. See
[TRANSFORM-PAIRING.md](TRANSFORM-PAIRING.md).

**How frames are presented had to change for any of that to work.** The
frontend was handing RT64 the *Console* presentation mode: show the buffer the
N64's video interface would have shown, which for this triple-buffered game is
the one finished two frames earlier. RT64 only interpolates when the buffer it
has just drawn is the one being presented, which under Console never happens
for a game that buffers at all -- so the Framerate setting did nothing, however
it was set. The port now uses *PresentEarly*, as the other ports do: each frame
is shown as soon as it is drawn, two frames of latency go away, and
interpolation is possible. `WR64_PRESENT_MODE=console|skip|early` selects a
mode by name for comparing them; it is a testing knob, not a setting.

Two rates matter when something looks wrong, and the port prints both whenever
the first changes:

```
[wr64] the game is running at 20 frames per second (it asked for 20); presenting at 60 (display 60 Hz)
```

The first is measured at `osViSwapBuffer`, which the game calls once per frame
it finishes (`patches/framerate.cpp`), so it says whether the machine is
keeping up with the game -- a race that keeps dropping from 30 to 20 says so
each time. The second is what the Framerate setting resolved to. With Display
selected on a 60 Hz panel the menus are presented at three times the game's
rate and a race at twice; a 144 Hz panel gets 144 either way.

## The web page

This fork's `main` branch also builds with [Emscripten](https://emscripten.org) into a page, `WaveRace64Recomp.html`,
that draws through WebGPU. `lib/RT64`, `lib/N64ModernRuntime` and `lib/RecompFrontend` are this port's own, at the
same commits, and [N64Recomp-browser](https://github.com/amazingalek/N64Recomp-browser) (`lib/N64Recomp-browser`)
patches their checkouts with the web support and makes the game a page. Mods and Quit are off on the page.

The port's own patch scripts then go on top, as for the desktop. `tools/web_patch.sh` does both in that order, from
clean submodules, so it can be run again at any time. The one script that meets the web support,
`tools/patch_rt64_water.py`, sees it in and applies `tools/patches/rt64-water-web.patch`: the same water renderer, its
shaders built as WGSL like the rest of RT64's on the page. The desktop build doesn't need the web patches, and a
checkout patched for the page builds the desktop game too.

With the reference decompilation in `reference/wr64-decomp` (the page was built with its commit 7784087) and your
dump beside it, on Linux (or WSL) with the Emscripten SDK installed and activated, [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga)
on `PATH` (`cargo install naga-cli`), and `spirv-opt` on `PATH` (SPIRV-Tools, `apt install spirv-tools`; without it
the prebuilt shaders below come out several times the size):

```bash
bash tools/web_patch.sh
bash tools/wsl_setup_splat.sh                                      # splat's Python environment (.venv), once
bash tools/wsl_build_recompiler.sh                                 # N64Recomp and RSPRecomp, patched, in build-linux
python3 tools/generate_game.py "Wave Race 64 (USA) (Rev A).z64"    # runs them
embuilder build sdl2-mt freetype-legacysjlj                        # once
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-web --target WaveRace64Recomp
```

The generated code is the same for both builds; the patched N64Recomp's `funcs.h` declares every function it calls,
which a WebAssembly build needs. Put [ccache](https://ccache.dev) on `PATH` when you configure and keep the build dir
inside the checkout, and a second build compiles nothing it has compiled before; `-DN64RECOMP_BROWSER_FAST_LINK=ON`
links in seconds instead of minutes, for trying something, not for a page people play.

`build-web` then holds `WaveRace64Recomp.html`, `.js`, `.wasm`, `.data` and `coi_serviceworker.js`. The page needs a
browser with WebGPU and must be cross-origin isolated: serve the folder with `Cross-Origin-Opener-Policy: same-origin`
and `Cross-Origin-Embedder-Policy: require-corp`, or on a host that can't set headers, as is, and
`coi_serviceworker.js` sets them after one reload. The player picks their dump in the launcher; it, the settings and
the saves are kept in the browser's IndexedDB for that origin. Sound goes through N64Recomp-browser's
`frontend/audio_output.h`, an AudioWorklet reading a ring in the page's memory, not SDL's queue on the page's busy
main thread.

The page builds the shaders the game draws with from `src/rt64_shaders.txt`, specialized and run through `spirv-opt`
when the page is built, so no scene draws with the slow ubershader: 139 shader descriptions, 100 pipelines. The list is
in sections: section 0 (63 descriptions: the launcher, the title and the menus to course select, which every visit
draws), then the attract demo, each of the nine courses, a Championship race with the other riders, and Stunt Mode.
In Chrome the whole list is built behind the game from its first frame, and the game waits only when a frame wants 4
or more of a section's shaders that aren't built yet, under a small "Preparing graphics" indicator in a corner.
Firefox builds a pipeline in line with the rest of its GPU work, so there section 0 is built before the game passes
its first frame and each other section when the game first draws with its shaders, the game waiting. A shader the
list misses is built behind the game, and the page remembers it and builds it with its section on the next visit.

The list is recorded by code, not by hand: `bash tools/record_shaders.sh build-web <your dump>` (its header says how)
plays `tools/shader_plan.json` in headless Chrome, a page load a section, in about 17 minutes; then build the page
again. The plan passes the menus by pressing until the game's state (`gGameState`, 0x800DAB24) says the next screen is
up, puts each course's place in the course select cursor, and holds the gas while `tools/autopilot.js` steers for the
next buoy on the side it is passed, turning off a wall it has run into, and keeps the countdown that retires a rider
off the course from running out. 2P VS isn't recorded: it needs a second pad.

RT64's render to RAM, which draws each frame a second time at the N64's size and reads it back into RDRAM, is off on
the page (`src/frontend.cpp`; the desktop keeps RT64's default). In a browser the readback makes every display list
wait for the GPU process, and the game's CPU never reads a frame: the one place it would, `sys_main.c`'s copy of the
last frame in display modes 1 and 2, is never reached, since nothing sets `D_800DAB1C` to either. Watched through the
menus, a race and its pause menu on the page, the mode stayed 0 throughout, and the pause dims the live scene.

Played from the published page (main b4bcb9d, N64Recomp-browser 421f902) on 2026-10-14, RTX 2070, a cold profile
each, one page at a time (the desktop's own browser open beside it, the GPU 36-42% busy before the runs, the host's
CPU 15-22%), by `tools/shader_plan.json`'s `sunset` section with N64Recomp-browser's `tools/play_page.mjs`: the
launcher, Load ROM, the title, the menus to course select and 50 s of Sunset Bay, with sound and no WebGPU errors.

```
                     wait (s)                  presents/s   GPU busy, W   GPU process, browser (cores)
Chrome 155 headless  1.8 (14, the boot)        59.6         27%  47       0.21  0.76
Firefox 158          3.5 (50, the boot),       59.6         28%  47       0.30  0.93
                     0.5 (13, Sunset Bay)
at 3840x2000, the stand-in for a laptop's integrated graphics:
Chrome 155 headless  1.8 (14)                  59.6         36%  54       0.22  0.77
Firefox 158          4.2 (50), 0.5 (13)        59.4         36%  77       0.36  1.01
```

Sound was short only in the boot's wait (1,161-3,553 frames), and once in Firefox at 3840x2000 for about 0.2 s more
on the way into the race (9,792 frames). Chrome builds the rest of the list behind the game: all 100 pipelines were
built 3-4 s after its boot wait ended.

Render to RAM on against off, the same page built both ways, headless Chrome 155, the same race, two runs each
interleaved on that host: 59.4 against 59.6 presents a second, the GPU process 0.23 against 0.20-0.24 cores, the
browser 0.79-0.83 against 0.73-0.82. At the game's 20 frames a second the readback costs a list's wall time, not
frames or CPU, so this only shows on a slower machine.

Played again from the page built on N64Recomp-browser d3b9031 (N64ModernRuntime-web f0e0884, RecompFrontend-web 75fe596,
rt64-web f34b19c; main a0fda71, gh-pages a735120) on 2026-10-15, its files served locally with the cross-origin headers,
RTX 2070, 1256x645, a fresh profile each, the host's every core busy with other work, the logos, the title and the
attract demo, 60 s, with N64Recomp-browser's `tools/play_page.mjs`: headless Chrome 155 waited 2.8 s for 14 and then
drew 59.6 presents a second (median), Firefox 158 waited 6.4 s for 50 and then drew 58.7. No WebGPU errors or exceptions
in either.

Played again from Pages, built on N64Recomp-browser 7277a2e (rt64-web 8bea892, tile copies reused from one display list
to the next; main 6c784b1, gh-pages 4e0ad30) on 2026-10-15, RTX 2070, 1256x645, a fresh profile each, the host's every
core busy with other work, the logos, the title and the attract demo, 60 s, with N64Recomp-browser's
`tools/play_page.mjs`: headless Chrome 155 waited 3.0 s for 14 and then drew 59.3 presents a second (median), Firefox
158 waited 5.9 s for 50 and then drew 59.4. Sound short only in the waits, no WebGPU errors or exceptions in either.
