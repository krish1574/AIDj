@echo off
REM Toolchain locations for the Android build.
REM The machine SDK at "C:\Program Files (x86)\Android\android-sdk" is not writable,
REM so we use a user-owned SDK root that junctions to it and holds the NDK + CMake.
set JAVA_HOME=C:\Users\Krish\dev-tools\jdk-17.0.20+8
set ANDROID_HOME=C:\Users\Krish\dev-tools\android-sdk
set ANDROID_SDK_ROOT=%ANDROID_HOME%
set PATH=%JAVA_HOME%\bin;%ANDROID_HOME%\platform-tools;%PATH%
