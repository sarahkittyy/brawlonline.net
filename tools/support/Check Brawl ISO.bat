@echo off
setlocal
title Brawl Online disc check
rem Support tool: hashes a Brawl disc image three times with certutil. Different
rem hashes mean the PC does not read the same file the same way twice (unstable
rem RAM or CPU settings), which also breaks the launcher's disc check and causes
rem desyncs. A plain ISO is also compared with the NTSC-U Rev 1 / Rev 2 MD5s.
rem Drag the disc file onto this .bat, or double-click it and drag the file into the window.

set "GOOD2=52ce7160ced2505ad5e397477d0ea4fe"
set "GOOD1=d18726e6dfdc8bdbdad540b561051087"

set "ISO=%~1"
if not "%ISO%"=="" goto :check
echo.
echo  Drag your Brawl disc file (.iso, .rvz, .wbfs) into this window,
echo  then press Enter:
echo.
set /p "ISO= > "
if not defined ISO goto :nofile
set "ISO=%ISO:"=%"

:check
if not exist "%ISO%" goto :nofile

echo.
echo  Brawl Online disc check
echo  -----------------------
echo  File: "%ISO%"
for %%F in ("%ISO%") do echo  Size: %%~zF bytes
echo  Started: %date% %time%
echo.
echo  This reads the whole file 3 times. It takes a few minutes.
echo  Please wait, and do not close this window...
echo.

call :hash 1
set "H1=%H%"
call :hash 2
set "H2=%H%"
call :hash 3
set "H3=%H%"

echo.
if "%H1%"=="ERROR" goto :error
if not "%H1%"=="%H2%" goto :changed
if not "%H1%"=="%H3%" goto :changed
if /i "%H1%"=="%GOOD2%" goto :good
if /i "%H1%"=="%GOOD1%" goto :good
goto :stable

:good
color 2F
echo  ================================================================
echo   RESULT: GOOD
echo   Same result all 3 times, and this is the correct Brawl ISO.
echo  ================================================================
goto :end

:stable
color 1F
echo  ================================================================
echo   RESULT: STABLE
echo   Same result all 3 times, so your PC reads this file correctly.
echo   (It is not a standard unmodified Brawl .iso. That is normal
echo   for .rvz and .wbfs files.)
echo  ================================================================
goto :end

:changed
color 4F
echo  ================================================================
echo   RESULT: THE HASH CHANGED BETWEEN PASSES
echo   Your PC read the same file differently each time.
echo   This is usually unstable RAM settings (XMP / EXPO / DOCP)
echo   or a CPU undervolt / overclock.
echo  ================================================================
goto :end

:error
color 4F
echo  ================================================================
echo   RESULT: COULD NOT READ THE FILE
echo  ================================================================
goto :end

:nofile
echo.
echo  Could not find that file.
echo.
echo  Close this window, then drag your Brawl disc file
echo  and drop it onto "Check Brawl ISO.bat".
echo.
pause
exit /b 1

:end
echo.
echo  Finished: %date% %time%
echo.
echo  Please take a screenshot of this whole window and send it back.
echo.
pause
exit /b 0

rem Hashes %ISO% with certutil into H: the hash is the second line of its output
rem (in every language). Anything that is not 32 characters is an error.
:hash
set "H="
set "OUT=%TEMP%\brawl_iso_md5.txt"
certutil -hashfile "%ISO%" MD5 > "%OUT%" 2>&1
for /f "usebackq skip=1 delims=" %%a in ("%OUT%") do if not defined H set "H=%%a"
del "%OUT%" >nul 2>&1
if not defined H set "H=ERROR"
set "H=%H: =%"
if "%H:~31,1%"=="" set "H=ERROR"
if not "%H:~32,1%"=="" set "H=ERROR"
echo  Pass %1 of 3: %H%   (%time%)
exit /b 0
