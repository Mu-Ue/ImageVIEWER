@echo off
rem Next to the exe, as the build copies it, or in the source folder, where the exe is in build\.
set "EXE=%~dp0ImageVIEWER.exe"
if not exist "%EXE%" set "EXE=%~dp0build\ImageVIEWER.exe"
if not exist "%EXE%" (
    echo ImageVIEWER.exe isn't next to this file or in build\. Run build.bat first.
    pause
    exit /b 1
)
start "" "%EXE%" --register
