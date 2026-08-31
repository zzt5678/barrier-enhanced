@echo off

REM defaults - override them by creating a build_env.bat file
set B_BUILD_TYPE=Debug
set B_QT_ROOT=C:\Qt
set B_QT_VER=5.11.1
set B_QT_MSVC=msvc2017_64
set B_BONJOUR=C:\Program Files\Bonjour SDK

set savedir=%cd%
cd /d %~dp0

REM cmake generator name for the target build system
if "%VisualStudioVersion%"=="17.0" (
    set cmake_gen=Visual Studio 17 2022
) else if "%VisualStudioVersion%"=="16.0" (
    set cmake_gen=Visual Studio 16 2019
) else if "%VisualStudioVersion%"=="15.0" (
    set cmake_gen=Visual Studio 15 2017
) else (
    echo Visual Studio version was not detected.
    echo Did you forget to run inside a VS developer prompt?
    echo Using the Visual Studio 2022 CMake generator.
    set cmake_gen=Visual Studio 17 2022
)

if exist build_env.bat call build_env.bat

REM needed by cmake to set bonjour include dir
set BONJOUR_SDK_HOME=%B_BONJOUR%

REM full path to Qt stuff we need
set B_QT_FULLPATH=%B_QT_ROOT%\%B_QT_VER%\%B_QT_MSVC%

echo Bonjour: %BONJOUR_SDK_HOME%
echo Qt: %B_QT_FULLPATH%

git submodule update --init --recursive

rmdir /q /s build
mkdir build
if ERRORLEVEL 1 goto failed
cd build
set B_GTEST_CMAKE_ARG=-D BARRIER_USE_EXTERNAL_GTEST=ON
if /I "%B_BUILD_TYPE%"=="Debug" set B_GTEST_CMAKE_ARG=-D CMAKE_DISABLE_FIND_PACKAGE_GTest=TRUE
cmake -G "%cmake_gen%" -A x64 -D CMAKE_BUILD_TYPE=%B_BUILD_TYPE% %B_GTEST_CMAKE_ARG% -D CMAKE_PREFIX_PATH="%B_QT_FULLPATH%" -D DNSSD_LIB="%B_BONJOUR%\Lib\x64\dnssd.lib" -D QT_VERSION=%B_QT_VER% ..
if ERRORLEVEL 1 goto failed
cmake --build . --config %B_BUILD_TYPE%
if ERRORLEVEL 1 goto failed

echo Build completed successfully
set BUILD_FAILED=0
goto done

:failed
set BUILD_FAILED=%ERRORLEVEL%
echo Build failed

:done
cd /d %savedir%

set B_BUILD_TYPE=
set B_QT_ROOT=
set B_QT_VER=
set B_QT_MSVC=
set B_BONJOUR=
set BONJOUR_SDK_HOME=
set B_QT_FULLPATH=
set B_GTEST_CMAKE_ARG=
set savedir=
set cmake_gen=

EXIT /B %BUILD_FAILED%
