@echo off
REM Builds the debug APK, including the aidj-audio native module (CMake/NDK).
REM First run takes ~20 min because it compiles Oboe; later runs are incremental.
call "%~dp0android-env.bat"
cd /d "%~dp0..\apps\mobile\android"
call gradlew.bat assembleDebug %*
