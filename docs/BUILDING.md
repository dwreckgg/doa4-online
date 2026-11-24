# Building Dead or Alive 4 Online

This is for contributors. Players do not need it: they download a release (see the
[README](../README.md)). The repository holds **no game data**: you build the game, then run it against
files extracted from your own copy of *Dead or Alive 4*. Sections 1 to 11 are the Windows build; the native
Linux build is [section 12](#12-native-linux-build).

The game is a static recompilation: the Xbox 360 code is translated ahead of time into C++
(`generated/`, checked in), and the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) supplies the
runtime (kernel, GPU, audio, input). The port builds against the SDK fork
[Dwreckgg/rexglue-sdk](https://github.com/Dwreckgg/rexglue-sdk) at the commit pinned in
`netplay_sdk.txt`.

## 1. The game version

The recompiled code matches one exact `default.xex`. Other regions or revisions will not work.

| | |
|---|---|
| Disc image used | `Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso` (Redump naming) |
| Title ID | `544307D1` |
| `default.xex` SHA-1 | `af8011945037b37c98fb8a78d5a4e844dd56c33f` |
| `default.xex` MD5 | `b2eb9c5df1f2060ae9c616945450289f` |
| Extracted size | about 6.8 GB |

Make the image from a disc you own. After extracting (section 5), check the hash:

```bat
certutil -hashfile xex\DOA4\default.xex SHA1
```

## 2. Tools

Versions are the ones the release workflow and the maintainers use; nearby versions will probably work.

| Tool | Version used | Where to get it |
|---|---|---|
| Windows 10/11, 64-bit, a CPU with SSE4.2 (Intel Core from 2008 (Nehalem) / AMD FX from 2011 (Bulldozer) or newer) | Windows 11 | The build targets `x86-64-v2`. |
| Git | any recent | <https://git-scm.com> or `winget install Git.Git` |
| Visual Studio 2026 Build Tools (v18), workload **Desktop development with C++** | 18 | <https://visualstudio.microsoft.com/downloads/>. It provides the Windows SDK, the MSVC libraries, CMake and Ninja (the build uses the bundled CMake and Ninja). |
| LLVM / Clang (the compiler; MSVC's compiler is not used) | 23.1.4 | `winget install LLVM.LLVM` (installs to `C:\Program Files\LLVM`) |
| Python 3 (the ISO extractor, the symlink fix, the test runners) | 3.14 | <https://www.python.org> or `winget install Python.Python.3.14` |

Optional: [extract-xiso](https://github.com/XboxDev/extract-xiso) as an alternative ISO extractor
(section 5).

## 3. Folder layout

Keep the three folders side by side. The code generator's manifest (`dead_or_alive_4_manifest.toml`)
looks for the game at `../xex/DOA4`, so this layout matters when regenerating code (section 8); for
building and running, any paths work.

```
doa4\
  doa4-online\     this repository
  rexglue-sdk\     the SDK, built from source together with the game
  xex\DOA4\        the extracted game files (default.xex, *.afs, movie\, SOUND\, dic\)
```

## 4. Get the sources

From a plain `cmd` window:

```bat
mkdir C:\doa4
cd /d C:\doa4
git clone -b main https://github.com/Dwreckgg/doa4-online doa4-online
type doa4-online\netplay_sdk.txt
```

`netplay_sdk.txt` holds two lines, `repo=` (the SDK repository) and `commit=` (the SDK commit this
version of the port is built with). Clone that repository and check out that commit; the release
workflow does exactly this:

```bat
git clone --filter=blob:none https://github.com/Dwreckgg/rexglue-sdk rexglue-sdk
git -C rexglue-sdk checkout <the commit from netplay_sdk.txt>
git -C rexglue-sdk submodule update --init --recursive
py -3 doa4-online\tools\fix_git_symlinks.py rexglue-sdk
```

The release is built from the `main` branch (the repository's default branch), as cloned above. CMake prints a harmless "no v* tag
reachable from HEAD" warning on a fresh SDK clone.

### Fix the SDK's symlinks (Windows)

Two SDK submodules (`libmspack`, `o1heap`) contain git symlinks. Unless symlinks are enabled (Windows
Developer Mode plus `git config --global core.symlinks true` *before* cloning), git checks them out as
small text files holding a path, and the build fails on them (for example errors in
`thirdparty/libmspack/cabextract/mspack/lzxd.c`, whose content is just `../../libmspack/mspack/lzxd.c`).
The last command above replaces them with real copies. It reports `replaced 16 symlink stub(s)` the
first time and nothing afterwards; it is safe to run again. `git status` in the SDK then shows the two
submodules as modified (` m`), which is expected.

## 5. The game files

The game reads the files from a folder, not from the disc image. Xbox 360 discs use the XDVDFS file
system, which ordinary ISO tools cannot open.

**Usually nothing to do here:** the first time `dead_or_alive_4.exe` starts without game files it shows
a setup screen. Choose your disc image and it checks the version, asks where to extract (default: a
`DOA4` folder beside the image; it shows the space needed and free) and extracts with a progress bar; or
choose a folder you extracted before. The Esc menu's **Game** tab can change the location later.

**By hand:** `tools/xdvdfs_extract.py` (a short Python script, no dependencies) does the same extraction
from a command line. It finds the game partition itself (DOA4 is an XGD2 disc; its partition starts at
byte `0xFD90000`).

```bat
cd /d C:\doa4
py -3 doa4-online\tools\xdvdfs_extract.py "Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso" xex\DOA4 --list
py -3 doa4-online\tools\xdvdfs_extract.py "Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso" xex\DOA4
```

The first command only lists the contents (a quick check that the image is readable); the second
extracts about 6.8 GB. Then check the `default.xex` hash (section 1). The ISO is not needed afterwards.

**Alternative:** [extract-xiso](https://github.com/XboxDev/extract-xiso) (`extract-xiso-Win64_Release.zip`
from its releases): `extract-xiso -x -d xex\DOA4 "<image>.iso"`. It was not the tool used for the first
build, so check `extract-xiso -h` if that command line does not work.

## 6. Build

`tools\np_build.bat` is the entry point. It sets up Visual Studio's environment, puts LLVM first on
`PATH`, and runs CMake with the flags the build needs. It reads the SDK folder from `DOA4_SDK` (forward
slashes). From a plain `cmd` window (not PowerShell). The script leaves the window in the `doa4-online` folder, so
start there and call it relative to it:

```bat
cd /d C:\doa4\doa4-online
set "DOA4_SDK=C:/doa4/rexglue-sdk"
tools\np_build.bat configure
tools\np_build.bat build
```

`configure` passes `-DDOA4_RELEASE_CHANNEL=alpha`, so a build folder configured earlier with another channel (the
old default was `netplay`) moves to `alpha` too; check `out\build\win-amd64-release\CMakeCache.txt` for
`DOA4_RELEASE_CHANNEL:STRING=alpha`.

`configure` also decides whether the build uses the profile in `pgo/` (profile-guided optimisation, `DOA4_PGO`);
[PGO.md](PGO.md) says how, and how to make the profile. If it refuses (`pgo: refused:`), `set DOA4_PGO=off`
builds without the profile; in CI, deleting `pgo/doa4.profdata` and `pgo/source.txt` does the same.

`configure` runs once (or after deleting `out\build`); `build` every time. It expects Visual Studio's
Build Tools at `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` and LLVM at
`C:\Program Files\LLVM`; for other install folders, run the commands by hand as below. `build` with a
second word, `np_build.bat build <tag>`, also copies the exe and its two DLLs to
`out\build\win-amd64-release\np_bin\<tag>\`.

The same, by hand (this is what the script and the release workflow run; the workflow finds
`VsDevCmd.bat` with `vswhere` and adds `-DDOA4_RELEASE_TAG=`, `-DDOA4_RELEASE_REPO=` and
`-DDOA4_RELEASE_CHANNEL=alpha`):

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d C:\doa4\doa4-online

rem Configure (once, or after deleting out\build):
cmake --preset win-amd64-release -DREXSDK_DIR=C:/doa4/rexglue-sdk "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DDOA4_RELEASE_CHANNEL=alpha

rem Build (every time):
cmake --build --preset win-amd64-release
```

- `VsDevCmd.bat` sets up the Windows SDK and puts Visual Studio's CMake and Ninja on `PATH`; LLVM goes
  first on `PATH` so `clang` / `clang++` are found (the presets name them).
- `REXSDK_DIR` builds the SDK from source alongside the game (no separate SDK install). Use forward
  slashes.
- The `-march=x86-64-v2` flags are required: the preset does not set a CPU level and the SDK uses
  SSSE3/SSE4.1 intrinsics, which fail to compile without it.
- The first build compiles the SDK and the large generated code and takes a while; later builds are
  incremental.
- The swimsuit converter's files are compiled with floating-point flags of their own (`CMakeLists.txt`,
  `DOA4_SUITCONV_COMPILE_OPTIONS`: no contraction, no fast-math, no FMA); do not change them.

The result is in `out\build\win-amd64-release\`: `dead_or_alive_4.exe` with `rexruntime.dll` and
`TracyClient.dll` beside it.

**Release identity.** A local build reports the release tag `dev` and counts as self-built: only the same
build can join it online. The release workflow sets `DOA4_RELEASE_TAG` to `alpha-v<x.y.z>`, where
`x.y.z` is `DOA4_NETPLAY_VERSION` in `CMakeLists.txt`, three numbers **set by hand** for each release
(the workflow refuses a version whose release already exists). The default channel is `alpha`.

## 7. Run

Start `out\build\win-amd64-release\dead_or_alive_4.exe`. On the first start (or when the saved location
has gone) the setup screen asks for the game files (section 5). Two command-line options override the
saved locations: `--game_data_root=<folder holding default.xex>` (the Game tab's **Change...** is then
off) and `--user_data_root=<folder>` (saves and the shader cache). Logs are in
`out\build\win-amd64-release\logs\dead_or_alive_4_NNN.log`.

## 8. Regenerating the recompiled code

`generated/` is checked in, so a normal build does not need this. Only after adding a function address to
`hints.toml` (for example after a log line `Call to invalid or unregistered function at guest address
0x...`), in the same `cmd` window as the build:

```bat
cmake --build --preset win-amd64-release --target dead_or_alive_4_codegen
cmake --preset win-amd64-release -DREXSDK_DIR=C:/doa4/rexglue-sdk "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2"
cmake --build --preset win-amd64-release
```

The generator reads `../xex/DOA4/default.xex` (the folder layout in section 3). Reconfiguring is needed
only if new `.cpp` files were generated. `commands.txt` has the upstream author's notes on this loop.

## 9. Tests

**Host tests.** The mod code (unlocks, photo, graphics options, netplay rules, the swimsuit converter,
and so on) has small tests that build with `clang-cl` without CMake or the game: one
`tests\host\run_<name>_test.bat` per module, each printing its own summary line (`PASSED: N checks, 0
failures` or `<name>: all checks passed`). To run them all, judged by that line and the exit code
(`--only <name> ...` for some), from a `cmd` window:

```bat
cd /d C:\doa4\doa4-online
set "REXSDK_DIR=C:\doa4\rexglue-sdk"
set "DOA4_CI=1"
py -3 -I tools\run_host_tests.py
```

The runners take Visual Studio's `VsDevCmd.bat` from `DOA4_VSDEVCMD` and LLVM's `bin` folder from
`DOA4_LLVM_BIN` when set (defaults: the Build Tools and `C:\Program Files\LLVM\bin` of section 2; the
release workflow sets both). `REXSDK_DIR` is the SDK folder some runners include headers from.

**Tool tests.** In the same window, in the repository folder:

```bat
set "DOA4_CI=1"
py -3 -I tests\tools\test_release_tools.py
py -3 -I tests\tools\test_np_tools.py
```

The launcher's tests (`launcher\tests`, Pester 3.4 from Windows PowerShell 5.1) are kept for the day the
launcher comes back: `Invoke-Pester -Path launcher/tests -ExcludeTag SymlinkStubs`.

**No game byte is in the repository.** The tests that check the code against the game's real tables (the
costume tables, the unlock tables, the suit loader) read them from a decrypted image of your own
`default.xex`: set `DOA4_TEST_IMAGE` to that file's path (the whole image, loaded at `0x82000000`). Each
range the tests read is checked against a SHA-1 first, so a different or damaged image is refused.
Without the variable those sections print `SKIPPED (DOA4_TEST_IMAGE unset)` and the rest still run; a
variable that is set to a missing or short file, or to an image that fails a pin, makes the test fail with
the reason. The unlock and Extras tests always run on synthetic tables and repeat on the real ones when
the image is there.

**The swimsuit converter's tests** (`suitconv_*`) take optional environment variables that point at your
own DOAX2 disc and game files; each section whose variable is unset prints
`<group>: SKIPPED (<VAR> unset)` and the rest still run. All are read-only:
`DOA4_SUITCONV_DISC` (the DOAX2 ISO or an extracted folder), `DOA4_SUITCONV_GAME_DIR` (the DOA4 game
folder), `DOA4_SUITCONV_ORACLE_DIR` (the reference converter's output: the 68 suit files and
`manifest.txt`), `DOA4_SUITCONV_LEVELS_DIR`, `DOA4_SUITCONV_TRACE_DIR` and `DOA4_SUITCONV_BUST_FILE`
(the reference converter's per-level outputs, traces and bust strengths). `DOA4_SUITCONV_OUT` moves a
runner's output folder, `DOA4_SUITCONV_ONLY` limits the all-suits sections to the designs it lists
(diagnosis only), and `DOA4_SUITCONV_REUSE=1` reuses converter objects already built (mutation probes).
Each runner's header comment says which ones it reads.

## 10. Releases

A release is built by the workflow `.github/workflows/netplay-release.yml`: it reads
`DOA4_NETPLAY_VERSION` from `CMakeLists.txt` (three numbers), builds the tag `alpha-v<x.y.z>`, runs the
host tests and the tool tests, stages the three game files, `SHA256SUMS.txt`, `THIRD_PARTY_NOTICES.md`
and a zip with `tools/release_stage.py`, and publishes a pre-release. It stops with a clear message if a
release of that version exists: raise `DOA4_NETPLAY_VERSION` first. The launcher is not part of the
release for now.

Each version needs its section in `CHANGELOG.md` (newest first, `## <x.y.z> (<date>)` with `### New`,
`### Fixed`, `### Notes`): the release notes and the zip's `README-netplay.txt` carry it
(`tools/changelog_section.py <x.y.z>` prints it), and a version without a section is not staged or
published.

## 11. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Compile errors in `thirdparty/libmspack/...` or `thirdparty/o1heap/...` | Symlink stubs: run `tools/fix_git_symlinks.py` on the SDK folder (section 4). |
| Errors about SSSE3 / SSE4.1 intrinsics (`_mm_...` functions) | Configure was run without the `-march=x86-64-v2` flags. Delete `out\build\win-amd64-release` and configure again. |
| `imgui.h` or `rex/version.h` not found | The two include directories after `rexglue_setup_target(dead_or_alive_4)` in `CMakeLists.txt` are missing (an old checkout, or a clone of the upstream repository, which lacks them). |
| `clang` not found / CMake picks the wrong compiler | LLVM's `bin` folder is not first on `PATH` in that window. |
| CMake warning `no v* tag reachable from HEAD` | Harmless; the build is not affected. |
| `no XDVDFS volume found` from the extractor | The file is not an Xbox / Xbox 360 game image (or is damaged). |

## 12. Native Linux build

Since 0.3.8 the source also builds as a native Linux program (x86-64), thanks to rynoster. There is no Linux
download: this is for people who build it themselves. **Players who want to play online with everyone else should
use the Windows release under Proton** (see the README's Linux section). A self-built game reports the release
tag `dev`, and online it can only join the same self-built game (section 6, "Release identity").

The steps below are the ones the CI job `.github/workflows/linux-build.yml` runs on every push to `main`, on
Ubuntu 24.04. That job builds the game and runs its host tests, but it does not start the game, so the
"Run" part is less tested than the build. Other distributions need the same packages under their own names.

**Packages (Ubuntu 24.04).** clang 23 from [apt.llvm.org](https://apt.llvm.org), by its own script, then the build
tools and the libraries the SDK and SDL3 need:

```sh
sudo apt-get update
sudo apt-get install -y wget gnupg lsb-release
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 23
sudo apt-get install -y clang-23
sudo apt-get install -y cmake ninja-build build-essential git curl unzip autoconf python3 libstdc++-14-dev
sudo apt-get install -y pkg-config libx11-xcb-dev libwayland-dev libgtk-3-dev
sudo apt-get install -y libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libdecor-0-dev libdbus-1-dev libibus-1.0-dev libudev-dev \
  libdrm-dev libgbm-dev libegl1-mesa-dev
sudo apt-get install -y libasound2-dev libpulse-dev libpipewire-0.3-dev
sudo apt-get install -y libvulkan-dev glslc glslang-tools
```

The game draws with Vulkan, so the PC also needs a working Vulkan driver (Mesa's `mesa-vulkan-drivers` on AMD
and Intel, NVIDIA's own driver on NVIDIA); `vulkaninfo --summary` (package `vulkan-tools`) shows whether one is
found.

**Sources.** The same as section 4, without the symlink fix (git on Linux checks symlinks out as symlinks):

```sh
mkdir -p ~/doa4 && cd ~/doa4
git clone -b main https://github.com/Dwreckgg/doa4-online doa4-online
cat doa4-online/netplay_sdk.txt
git clone --filter=blob:none https://github.com/Dwreckgg/rexglue-sdk rexglue-sdk
git -C rexglue-sdk checkout <the commit from netplay_sdk.txt>
git -C rexglue-sdk submodule update --init --recursive
```

**Build.** From the `doa4-online` folder:

```sh
cd ~/doa4/doa4-online
cmake --preset linux-amd64-release -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \
  "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DREXSDK_DIR=$HOME/doa4/rexglue-sdk
cmake --build --preset linux-amd64-release -- -j 2
```

- The compiler flags name clang 23 because the preset's default (`clang-20`) is not what the CI job uses.
- `-march=x86-64-v2` is required, as on Windows (section 6).
- `-j 2`: the largest recompiled files take several GB of memory each to compile. Two at a time is what the CI job
  runs on a 16 GB machine; raise it only with more memory. The first build takes a long time (the CI job allows up
  to four hours on two cores).
- The Linux build does not use the PGO profile in `pgo/` (that is part of the Windows build script only).

The result is `out/build/linux-amd64-release/dead_or_alive_4` with `librexruntime.so` beside it. Keep the two
together: the program looks for the library in its own folder. `ldd out/build/linux-amd64-release/dead_or_alive_4`
should show no "not found" line.

**Run.** Start `out/build/linux-amd64-release/dead_or_alive_4`. The first start shows the same setup screen as on
Windows (section 5); its file dialog is SDL3's, which uses your desktop's file chooser. Without a usable chooser,
extract the game by hand with `python3 tools/xdvdfs_extract.py "<image>.iso" ~/doa4/xex/DOA4` (section 5) and start
the game with `--game_data_root=$HOME/doa4/xex/DOA4`.

- Settings: `dead_or_alive_4.toml` beside the program.
- Logs: the `logs` folder beside the program.
- Saves and the shader cache: `$XDG_DATA_HOME/dead_or_alive_4`, or `~/.local/share/dead_or_alive_4` when
  `XDG_DATA_HOME` is not set; `--user_data_root=<folder>` overrides it.

**Host tests.** The host tests listed in `tests/host/posix_manifest.txt` run on Linux too:

```sh
cd ~/doa4/doa4-online
DOA4_CI=1 REXSDK_DIR=$HOME/doa4/rexglue-sdk python3 -I tools/run_host_tests_posix.py --cxx clang++-23
```
