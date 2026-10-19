@echo off
rem Build the game if needed, then start it. Double-click this file, or run
rem   play.bat [game options]          for example: play.bat --fullscreen
setlocal
cd /d "%~dp0"
if not defined BUILD_DIR set "BUILD_DIR=build"
set "BONK_FROM_PLAY=1"
call "%~dp0build.bat" || goto failed
"%BUILD_DIR%\bonkheads.exe" %*
exit /b %errorlevel%

:failed
echo.
echo The build did not finish; see the messages above.
rem keep the window open when started by double-click
echo "%cmdcmdline%" | findstr /i /c:"%~nx0" >nul && pause
exit /b 1
