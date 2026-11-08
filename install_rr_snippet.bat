@echo off
rem Installs the snippet of DLSS Ray Reconstruction, nvngx_dlssd.dll, next to the NGX bridge, in
rem reframework\rr\. The release does not include it, since it belongs to NVIDIA, under the NVIDIA RTX SDKs
rem License: this file downloads it from NVIDIA's own repository, NVIDIA/DLSS on GitHub, at the version that the
rem bridge is built with. It keeps the download only if its SHA-256 is the expected one, and it never replaces an
rem nvngx_dlssd.dll that is already there.
rem
rem Keep it in the game's folder, where the zip of the release was extracted, and run it from there. It only uses
rem tools that come with Windows 10 1803 and later: curl.exe, certutil.exe and findstr.exe. To do the same by
rem hand, download the file at URL, check that certutil -hashfile nvngx_dlssd.dll SHA256 gives SHA256, and put it
rem in reframework\rr\.
setlocal EnableExtensions DisableDelayedExpansion

set "URL=https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlssd.dll"
set "SHA256=b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee"
set "CURL=%SystemRoot%\System32\curl.exe"
set "CERTUTIL=%SystemRoot%\System32\certutil.exe"
set "FINDSTR=%SystemRoot%\System32\findstr.exe"

rem The folder of this file, not the current one, so that it also works from another folder.
set "RR=%~dp0reframework\rr"
set "SNIPPET=nvngx_dlssd.dll"
set "PART=nvngx_dlssd.dll.part"

if exist "%RR%\rr_bridge.dll" goto in_game_folder
echo rr_bridge.dll is not in "%RR%".
echo This file has to be in the game's folder, where the zip of the release was extracted, next to dinput8.dll.
set "CODE=1"
goto end

:in_game_folder
rem From here on the files go by their names in reframework\rr\: a call or a pipe would expand a percent sign
rem or a caret of the game's path a second time. The end of setlocal gives back the current folder.
cd /d "%RR%"
if not exist %SNIPPET% goto download
call :matches %SNIPPET%
if errorlevel 1 goto another_snippet
echo nvngx_dlssd.dll is in "%RR%" already, with the expected SHA-256: nothing to do.
set "CODE=0"
goto end

:another_snippet
echo There is another nvngx_dlssd.dll in "%RR%".
echo Its SHA-256 is not %SHA256%, the one of the snippet that RR was tested with.
echo It was left as it is. To replace it, delete it and run this file again.
set "CODE=1"
goto end

:download
if exist "%CURL%" goto curl_found
echo This Windows has no "%CURL%", which comes with Windows 10 1803 and later.
goto by_hand

:curl_found
echo Downloading nvngx_dlssd.dll, the snippet of DLSS Ray Reconstruction, from
echo %URL%
rem --location follows GitHub's redirect to raw.githubusercontent.com, and --fail keeps an error page out of the
rem file. The download goes to a name of its own, which nothing loads, until its SHA-256 is known.
"%CURL%" --fail --location --show-error --progress-bar --connect-timeout 30 --output %PART% "%URL%"
if errorlevel 1 goto download_failed
call :matches %PART%
if errorlevel 1 goto wrong_sha256
move %PART% %SNIPPET% >nul
if errorlevel 1 goto move_failed
echo(
echo Done: nvngx_dlssd.dll is in "%RR%".
set "CODE=0"
goto end

:download_failed
echo(
echo The download failed: curl says why, above.
goto by_hand

:wrong_sha256
echo(
echo The downloaded file is not the expected one, since its SHA-256 differs, so it is deleted.
goto by_hand

:move_failed
echo(
echo The download could not be renamed to nvngx_dlssd.dll, so it is deleted.
goto by_hand

:by_hand
if exist %PART% del /f /q %PART%
echo(
echo Nothing was installed. To install the snippet by hand:
echo   1. Download nvngx_dlssd.dll from
echo      %URL%
echo   2. Check that its SHA-256 is %SHA256%,
echo      for instance with: certutil -hashfile nvngx_dlssd.dll SHA256
echo   3. Put it in "%RR%".
set "CODE=1"
goto end

:end
echo(
pause
exit /b %CODE%

rem Whether the file of the first argument, in the current folder, has the SHA-256 SHA256, as errorlevel 0.
rem certutil gives the hash on a line of its own, in lowercase hexadecimal since Windows 10.
:matches
"%CERTUTIL%" -hashfile %1 SHA256 2>nul | "%FINDSTR%" /i /x /c:"%SHA256%" >nul
exit /b
