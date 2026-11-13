@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "wait_at_end=1"
if /i "%~1"=="--no-pause" (
    set "wait_at_end=0"
    shift
)
set "serial=%~1"
set "device_list="

if exist "%~dp0adb.exe" (
    set "ADB=%~dp0adb.exe"
) else (
    where adb.exe >nul 2>&1
    if errorlevel 1 (
        echo adb.exe was not found beside this script or on PATH. 1>&2
        set "result=1"
        goto :finish
    )
    set "ADB=adb.exe"
)

setlocal EnableDelayedExpansion
if defined serial goto :reverse

set "device_list=!TEMP!\labyrinth-adb-!RANDOM!-!RANDOM!.txt"
"!ADB!" devices -l > "!device_list!"
set "result=!errorlevel!"
if not "!result!"=="0" goto :finish

set "device_count=0"
for /f "usebackq tokens=1,2,*" %%A in ("!device_list!") do (
    if "%%B"=="device" (
        set /a device_count+=1
        set "device[!device_count!]=%%A"
        set "details[!device_count!]=%%C"
    )
)

if !device_count! equ 0 (
    echo No ready ADB device. Start the emulator and enable ADB debugging. 1>&2
    type "!device_list!"
    set "result=1"
    goto :finish
)
if !device_count! equ 1 (
    set "serial=!device[1]!"
    goto :reverse
)

echo Connected ADB devices:
for /l %%I in (1,1,!device_count!) do echo   %%I. !device[%%I]!  !details[%%I]!
if "!wait_at_end!"=="0" (
    echo More than one device is ready. Pass its serial after --no-pause. 1>&2
    set "result=1"
    goto :finish
)

:choose
set "choice="
set /p "choice=Choose a device number (Enter to cancel): "
if not defined choice (
    echo Cancelled.
    set "result=1"
    goto :finish
)
for /l %%I in (1,1,!device_count!) do (
    if "!choice!"=="%%I" (
        set "serial=!device[%%I]!"
        goto :reverse
    )
)
echo Invalid choice. Enter a number from 1 to !device_count!.
goto :choose

:reverse
echo Using ADB device !serial!.
"!ADB!" -s "!serial!" reverse tcp:8081 tcp:8081
set "result=!errorlevel!"
if "!result!"=="0" (
    echo ADB reverse is ready: Android localhost:8081 to Windows localhost:8081.
) else (
    echo ADB reverse failed. Check the selected device and its ADB connection. 1>&2
    "!ADB!" devices -l
)

:finish
if defined device_list del /q "!device_list!" >nul 2>&1
if "%wait_at_end%"=="1" pause
exit /b %result%
