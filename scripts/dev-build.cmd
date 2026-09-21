@echo off
rem Local build + every check CI runs, in one command.
rem   scripts\dev-build.cmd [Debug|Release]
setlocal enabledelayedexpansion

set ROOT=%~dp0..
for %%I in ("%ROOT%") do set ROOT=%%~fI

set CFG=%1
if "%CFG%"=="" set CFG=Debug

rem Find any VS 2022 edition, preferring vswhere when it is present.
set VSDEV=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDEV=%%I\Common7\Tools\VsDevCmd.bat
)
if not defined VSDEV (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat" (
      if not defined VSDEV set VSDEV=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat
    )
  )
)
if not defined VSDEV (
  echo Could not find a Visual Studio 2022 C++ toolchain.
  exit /b 1
)

call "%VSDEV%" -arch=x64 -host_arch=x64 >nul || exit /b 1

cmake -S "%ROOT%" -B "%ROOT%\build\x64-%CFG%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
cmake --build "%ROOT%\build\x64-%CFG%" || exit /b 1
ctest --test-dir "%ROOT%\build\x64-%CFG%" --output-on-failure || exit /b 1

echo.
echo === exe smoke ===
"%ROOT%\build\x64-%CFG%\mcp-winauth-bridge.exe" --version || exit /b 1

echo.
echo === mode (b): install the package, then link it from a separate project ===
cmake --install "%ROOT%\build\x64-%CFG%" --prefix "%ROOT%\build\stage" || exit /b 1
cmake -S "%ROOT%\examples\consumer" -B "%ROOT%\build\consumer" -G Ninja ^
      -DCMAKE_BUILD_TYPE=%CFG% -DCMAKE_PREFIX_PATH="%ROOT%\build\stage" || exit /b 1
cmake --build "%ROOT%\build\consumer" || exit /b 1
"%ROOT%\build\consumer\consumer.exe" || exit /b 1

echo.
echo === mode (c): the sources dropped into another project, no cmake ===
if not exist "%ROOT%\build\dropin" mkdir "%ROOT%\build\dropin"
pushd "%ROOT%\build\dropin"
cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /Zc:preprocessor ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I"%ROOT%\include" /I"%ROOT%\src" /Fedropin.exe ^
   "%ROOT%\src\*.cpp" "%ROOT%\app\main.cpp" winhttp.lib || (popd & exit /b 1)
.\dropin.exe --version || (popd & exit /b 1)
popd

echo.
echo === ALL CHECKS PASSED (%CFG%) ===
