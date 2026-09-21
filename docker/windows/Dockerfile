# escape=`
#
# Hermetic, offline-reproducible build of mcp-winauth-bridge.exe.
#
# This needs Docker in WINDOWS CONTAINERS mode. It is the slowest of the three
# build paths (30-60 minutes cold, and the build stage is around 20 GB); CI is
# the day-to-day path and a GitHub Release is the day-to-day download. Use this
# when you must build from source on a machine you control.
#
#   docker build --isolation=hyperv -t mcpwa-build -f docker\windows\Dockerfile .
#   $c = docker create mcpwa-build
#   docker cp "${c}:C:\mcp-winauth-bridge.exe" .\dist\
#   docker rm $c
#
# Process isolation needs the host kernel to match the base image. On a Windows
# 11 26xxx host either pass --isolation=hyperv as above, or change the ltsc2022
# tags to ones matching the host.

FROM mcr.microsoft.com/windows/servercore:ltsc2022 AS build
SHELL ["powershell", "-NoProfile", "-Command", "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue';"]

# VC.CMake.Project brings CMake and Ninja, so nothing else has to be fetched.
ADD https://aka.ms/vs/17/release/vs_buildtools.exe C:\TEMP\vs_buildtools.exe
RUN Start-Process -Wait -FilePath C:\TEMP\vs_buildtools.exe -ArgumentList `
      '--quiet', '--wait', '--norestart', '--nocache', `
      '--installPath', 'C:\BuildTools', `
      '--add', 'Microsoft.VisualStudio.Workload.VCTools', `
      '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', `
      '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621', `
      '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project'; `
    Remove-Item C:\TEMP\vs_buildtools.exe

WORKDIR C:\src
COPY . .

# MCPWA_STATIC_RUNTIME so the exe runs on a machine with no VC++ redist.
SHELL ["cmd", "/S", "/C"]
RUN C:\BuildTools\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && `
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMCPWA_STATIC_RUNTIME=ON && `
    cmake --build build && `
    ctest --test-dir build --output-on-failure

# Nanoserver carries nothing but the binary, so `docker cp` out of it is cheap.
FROM mcr.microsoft.com/windows/nanoserver:ltsc2022 AS export
COPY --from=build C:\src\build\mcp-winauth-bridge.exe C:\mcp-winauth-bridge.exe
