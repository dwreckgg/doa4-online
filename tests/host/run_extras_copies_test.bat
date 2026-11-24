@echo off
rem Builds and runs the extras-copies host test (tests/host/extras_copies_host_test.cpp: the start-up builder
rem src/doa4_extras_copies.cpp, which reads the three costumes through a callback, checks their SHA-1, runs the slim core
rem and checks the size limits; on synthetic models from tests/host/slim_synth.h, with a test costume table whose
rem SHA-1s the test computes; needs no disc, no game file and no environment variable) with clang-cl (no CMake), under
rem the converter's floating-point flags for every translation unit (x86-64-v2 with FMA switched off, no contraction,
rem no fast-math, no errno), as run_suitconv_slim_test.bat builds the slim core. The core's objects go to
rem %TEMP%\doa4_suitconv_test\obj\ (named after their source), where suitconv_fp_scan.py scans them before the test
rem runs; the test's own object, doa4_extras_copies.obj (not a converter file: the core runs under its own FP
rem environment inside SlimCopies) and the archive, game-file and disc readers it links (doa4_afs_overlay,
rem doa4_gamefiles, doa4_xdvdfs) go to ...\testobj\, with doa4_colour_costumes and what it needs (doa4_suits_manifest,
rem doa4_tweaks, doa4_costumes): the install_order group runs InstallOrder over the real colour_costumes::Decide.
rem DOA4_SUITCONV_OUT, if set, replaces that output folder (the mutation probes build their copies elsewhere).
rem DOA4_EXTRAS_GAME_DIR, if set, is a game folder holding chrtwo.afs (opened for reading only): the "extras real
rem archive" group then runs Build on it and expects all three costumes built with G0's SHA-1s. Unset, it prints
rem "extras real archive: SKIPPED (DOA4_EXTRAS_GAME_DIR unset)" and counts nothing. DOA4_EXTRAS_TEST_FLIP=1 (with it)
rem flips one byte of entry 46 as read: the group must then fail on hak03 alone (the positive control).
rem Prints one "<group>: N checks, M failures" line per group, then "PASSED: N checks, 0 failures" (or FAILED) as its
rem last line.
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
set "OUT=%TEMP%\doa4_suitconv_test"
if defined DOA4_SUITCONV_OUT set "OUT=%DOA4_SUITCONV_OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\testobj" mkdir "%OUT%\testobj"
set "SRC=%~dp0..\..\src"
set FPFLAGS=/clang:-march=x86-64-v2 /clang:-mno-fma /clang:-ffp-contract=off /clang:-fno-fast-math /clang:-fno-math-errno
rem The standard library's hardening mode is on for every translation unit (bounds-checked operator[] and the like): an
rem out-of-range read in the archive reader then stops the run instead of reading whatever lies past the TOC (the
rem mutation probe of its count guard survived without it). It adds checks only; the FP flags are unchanged.
set CXXFLAGS=/nologo /std:c++latest /EHsc /O2 /D_MSVC_STL_HARDENING=1 %FPFLAGS%
set "LOG=%OUT%\extras_build.log"
for %%f in (suitconv_num suitconv_tpr suitconv_core suitconv_slimread suitconv_slimgrid suitconv_slimscan suitconv_slimwrite) do if exist "%OUT%\obj\doa4_%%f.obj" del /q "%OUT%\obj\doa4_%%f.obj"
for %%f in (extras_copies_host_test doa4_extras_copies doa4_afs_overlay doa4_gamefiles doa4_xdvdfs doa4_colour_costumes doa4_suits_manifest doa4_tweaks doa4_costumes) do if exist "%OUT%\testobj\%%f.obj" del /q "%OUT%\testobj\%%f.obj"
if exist "%OUT%\extras_copies_host_test.exe" del /q "%OUT%\extras_copies_host_test.exe"
type nul > "%LOG%"
for %%f in (suitconv_num suitconv_tpr suitconv_core suitconv_slimread suitconv_slimgrid suitconv_slimscan suitconv_slimwrite) do (
  clang-cl %CXXFLAGS% /c "%SRC%\doa4_%%f.cpp" /Fo"%OUT%\obj\doa4_%%f.obj" >> "%LOG%" 2>&1 || goto failed
)
for %%f in (doa4_extras_copies doa4_afs_overlay doa4_gamefiles doa4_xdvdfs doa4_colour_costumes doa4_suits_manifest doa4_tweaks doa4_costumes) do (
  clang-cl %CXXFLAGS% /c "%SRC%\%%f.cpp" /Fo"%OUT%\testobj\%%f.obj" >> "%LOG%" 2>&1 || goto failed
)
clang-cl %CXXFLAGS% /c "%~dp0extras_copies_host_test.cpp" /Fo"%OUT%\testobj\extras_copies_host_test.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
clang-cl /nologo "%OUT%\testobj\extras_copies_host_test.obj" "%OUT%\obj\doa4_suitconv_num.obj" "%OUT%\obj\doa4_suitconv_tpr.obj" "%OUT%\obj\doa4_suitconv_core.obj" "%OUT%\obj\doa4_suitconv_slimread.obj" "%OUT%\obj\doa4_suitconv_slimgrid.obj" "%OUT%\obj\doa4_suitconv_slimscan.obj" "%OUT%\obj\doa4_suitconv_slimwrite.obj" "%OUT%\testobj\doa4_extras_copies.obj" "%OUT%\testobj\doa4_afs_overlay.obj" "%OUT%\testobj\doa4_gamefiles.obj" "%OUT%\testobj\doa4_xdvdfs.obj" "%OUT%\testobj\doa4_colour_costumes.obj" "%OUT%\testobj\doa4_suits_manifest.obj" "%OUT%\testobj\doa4_tweaks.obj" "%OUT%\testobj\doa4_costumes.obj" bcrypt.lib /Fe"%OUT%\extras_copies_host_test.exe" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
rem The FP scan of every converter object comes first: any FMA instruction or C runtime math call (or a scan that
rem cannot run) fails the runner with the default summary shape, as one failed check, and the test does not run.
py -3 -I "%~dp0suitconv_fp_scan.py" "%OUT%\obj" > "%OUT%\extras_scan.log" 2>&1
if errorlevel 1 goto scanfailed
type "%OUT%\extras_scan.log"
"%OUT%\extras_copies_host_test.exe" %*
exit /b %errorlevel%
:scanfailed
type "%OUT%\extras_scan.log"
echo FP SCAN FAILED: a converter object has an FMA instruction or a C runtime math call, or the scan could not run
echo FAILED: 1 checks, 1 failures
exit /b 1
:failed
type "%LOG%"
echo BUILD FAILED
exit /b 1
