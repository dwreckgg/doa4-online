@echo off
rem Builds and runs the colour generator host test (the Alpha-152 recolour, doa4_colourgen_alpha.cpp; the Spartan-458
rem repaint, doa4_colourgen_spartan.cpp; DXT1 and tiling, doa4_colourgen_dxt.cpp) with clang-cl (no CMake), under the
rem converter's floating-point flags for every translation unit (x86-64-v3 with FMA switched off, no contraction, no
rem fast-math, no errno). The generator objects and the converter objects they link (num, tpr) go to
rem %TEMP%\doa4_colourgen_test\obj\, where suitconv_fp_scan.py scans them before the test runs: the scan's folder search
rem picks only doa4_suitconv_* objects, so every doa4_colourgen_*.obj found there is passed to it by name (a wildcard
rem loop, after every doa4_colourgen_*.obj was deleted at the start: a stale object is neither scanned nor linked); the
rem test's own object and the other sources it links (gamefiles for SHA-1, xdvdfs; colour_costumes for the slot table
rem with what it needs: suits_manifest, tweaks, afs_overlay, costumes) go to ...\testobj\, are deleted at the start
rem like the others, and are not scanned.
rem DOA4_COLOURGEN_OUT, if set, replaces that output folder (the mutation probes build their copies elsewhere).
rem DOA4_SUITS_GAME_DIR, if set, is the game folder whose chrthree.afs the real_files section reads (read only);
rem DOA4_COLOURGEN_APPROVED_DIR, if set, adds diagnosis lines to a real_files SHA-1 miss.
rem Prints "PASSED: N checks, 0 failures" (or FAILED) as its last line.
setlocal
if not defined DOA4_VSDEVCMD set "DOA4_VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
call "%DOA4_VSDEVCMD%" -arch=amd64 -host_arch=amd64 >nul 2>&1
if not defined DOA4_LLVM_BIN set "DOA4_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%DOA4_LLVM_BIN%;%PATH%"
set "OUT=%TEMP%\doa4_colourgen_test"
if defined DOA4_COLOURGEN_OUT set "OUT=%DOA4_COLOURGEN_OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\testobj" mkdir "%OUT%\testobj"
set "SRC=%~dp0..\..\src"
set FPFLAGS=/clang:-march=x86-64-v3 /clang:-mno-fma /clang:-ffp-contract=off /clang:-fno-fast-math /clang:-fno-math-errno
set CXXFLAGS=/nologo /std:c++latest /EHsc /O2 %FPFLAGS%
set "LOG=%OUT%\colourgen_build.log"
for %%f in (doa4_suitconv_num doa4_suitconv_tpr) do if exist "%OUT%\obj\%%f.obj" del /q "%OUT%\obj\%%f.obj"
if exist "%OUT%\obj\doa4_colourgen_*.obj" del /q "%OUT%\obj\doa4_colourgen_*.obj"
for %%f in (doa4_gamefiles doa4_xdvdfs doa4_colour_costumes doa4_suits_manifest doa4_tweaks doa4_afs_overlay doa4_costumes colourgen_host_test) do if exist "%OUT%\testobj\%%f.obj" del /q "%OUT%\testobj\%%f.obj"
if exist "%OUT%\colourgen_host_test.exe" del /q "%OUT%\colourgen_host_test.exe"
type nul > "%LOG%"
for %%f in (doa4_suitconv_num doa4_suitconv_tpr doa4_colourgen_alpha doa4_colourgen_spartan doa4_colourgen_dxt) do (
  clang-cl %CXXFLAGS% /c "%SRC%\%%f.cpp" /Fo"%OUT%\obj\%%f.obj" >> "%LOG%" 2>&1 || goto failed
)
clang-cl %CXXFLAGS% /c "%SRC%\doa4_gamefiles.cpp" /Fo"%OUT%\testobj\doa4_gamefiles.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
clang-cl %CXXFLAGS% /c "%SRC%\doa4_xdvdfs.cpp" /Fo"%OUT%\testobj\doa4_xdvdfs.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
for %%f in (doa4_colour_costumes doa4_suits_manifest doa4_tweaks doa4_afs_overlay doa4_costumes) do (
  clang-cl %CXXFLAGS% /c "%SRC%\%%f.cpp" /Fo"%OUT%\testobj\%%f.obj" >> "%LOG%" 2>&1 || goto failed
)
clang-cl %CXXFLAGS% /c "%~dp0colourgen_host_test.cpp" /Fo"%OUT%\testobj\colourgen_host_test.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
rem every generator object in obj\ (all of them were compiled above), linked and scanned by name
set "CGOBJS="
for %%o in ("%OUT%\obj\doa4_colourgen_*.obj") do call :addobj "%%~fo"
clang-cl /nologo "%OUT%\testobj\colourgen_host_test.obj" "%OUT%\obj\doa4_suitconv_num.obj" "%OUT%\obj\doa4_suitconv_tpr.obj" %CGOBJS% "%OUT%\testobj\doa4_gamefiles.obj" "%OUT%\testobj\doa4_xdvdfs.obj" "%OUT%\testobj\doa4_colour_costumes.obj" "%OUT%\testobj\doa4_suits_manifest.obj" "%OUT%\testobj\doa4_tweaks.obj" "%OUT%\testobj\doa4_afs_overlay.obj" "%OUT%\testobj\doa4_costumes.obj" bcrypt.lib /Fe"%OUT%\colourgen_host_test.exe" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
rem The FP scan of every generator and converter object comes first: any FMA instruction or C runtime math call (or a
rem scan that cannot run) fails the runner with the default summary shape, as one failed check, and the test does not run.
py -3 -I "%~dp0suitconv_fp_scan.py" "%OUT%\obj" %CGOBJS% > "%OUT%\colourgen_scan.log" 2>&1
if errorlevel 1 goto scanfailed
type "%OUT%\colourgen_scan.log"
"%OUT%\colourgen_host_test.exe"
exit /b %errorlevel%
:scanfailed
type "%OUT%\colourgen_scan.log"
echo FP SCAN FAILED: a generator or converter object has an FMA instruction or a C runtime math call, or the scan could not run
echo FAILED: 1 checks, 1 failures
exit /b 1
:failed
type "%LOG%"
echo BUILD FAILED
exit /b 1
:addobj
set CGOBJS=%CGOBJS% %1
exit /b 0
