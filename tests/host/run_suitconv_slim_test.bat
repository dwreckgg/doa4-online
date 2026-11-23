@echo off
rem Builds and runs the slim-copies host test (tests/host/suitconv_slim_host_test.cpp: the container layer of the slim
rem core, src/doa4_suitconv_slimread.cpp, and its grid merge, src/doa4_suitconv_slimgrid.cpp, on synthetic models whose expected values come from the Python tool through
rem tests/host/slim_expect.py; needs no disc, no game file and no environment variable) with clang-cl (no CMake), under
rem the converter's floating-point flags for every translation unit (x86-64-v2 with FMA switched off, no contraction,
rem no fast-math, no errno). Converter objects go to %TEMP%\doa4_suitconv_test\obj\ (named after their source), where
rem suitconv_fp_scan.py scans them before the test runs; the test's own object goes to ...\testobj\.
rem DOA4_SUITCONV_OUT, if set, replaces that output folder (the mutation probes build their copies elsewhere).
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
set CXXFLAGS=/nologo /std:c++latest /EHsc /O2 %FPFLAGS%
set "LOG=%OUT%\slim_build.log"
for %%f in (num tpr slimread slimgrid) do if exist "%OUT%\obj\doa4_suitconv_%%f.obj" del /q "%OUT%\obj\doa4_suitconv_%%f.obj"
if exist "%OUT%\testobj\suitconv_slim_host_test.obj" del /q "%OUT%\testobj\suitconv_slim_host_test.obj"
if exist "%OUT%\suitconv_slim_host_test.exe" del /q "%OUT%\suitconv_slim_host_test.exe"
type nul > "%LOG%"
for %%f in (num tpr slimread slimgrid) do (
  clang-cl %CXXFLAGS% /c "%SRC%\doa4_suitconv_%%f.cpp" /Fo"%OUT%\obj\doa4_suitconv_%%f.obj" >> "%LOG%" 2>&1 || goto failed
)
clang-cl %CXXFLAGS% /c "%~dp0suitconv_slim_host_test.cpp" /Fo"%OUT%\testobj\suitconv_slim_host_test.obj" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
clang-cl /nologo "%OUT%\testobj\suitconv_slim_host_test.obj" "%OUT%\obj\doa4_suitconv_num.obj" "%OUT%\obj\doa4_suitconv_tpr.obj" "%OUT%\obj\doa4_suitconv_slimread.obj" "%OUT%\obj\doa4_suitconv_slimgrid.obj" /Fe"%OUT%\suitconv_slim_host_test.exe" >> "%LOG%" 2>&1
if errorlevel 1 goto failed
rem The FP scan of every converter object comes first: any FMA instruction or C runtime math call (or a scan that
rem cannot run) fails the runner with the default summary shape, as one failed check, and the test does not run.
py -3 -I "%~dp0suitconv_fp_scan.py" "%OUT%\obj" > "%OUT%\slim_scan.log" 2>&1
if errorlevel 1 goto scanfailed
type "%OUT%\slim_scan.log"
"%OUT%\suitconv_slim_host_test.exe" %*
exit /b %errorlevel%
:scanfailed
type "%OUT%\slim_scan.log"
echo FP SCAN FAILED: a converter object has an FMA instruction or a C runtime math call, or the scan could not run
echo FAILED: 1 checks, 1 failures
exit /b 1
:failed
type "%LOG%"
echo BUILD FAILED
exit /b 1
