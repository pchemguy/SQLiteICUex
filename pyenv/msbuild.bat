@echo off

:: ============================================================================
:: Activate the MSVC build environment for an x64 target in the current CMD.
:: Reuse a verified environment or find and call vcvars64.bat.
:: Set NOCOLOR for plain output. Caller delayed expansion is optional.
:: Returns 0 for a verified environment or 1 on failure.
:: See msbuild.md for configuration, discovery, and troubleshooting.
:: ============================================================================

call :COLOR_SCHEME

echo:
echo ====================================================================================
echo ====================================================================================
echo == %WARN%                                                            %WARN% ==
echo == %WARN%                    MS Build Tools Check                    %WARN% ==
echo == %WARN%                                                            %WARN% ==
echo == %WARN% CLI: "%~f0" %*
echo ====================================================================================
echo ====================================================================================
echo:

set "EXIT_STATUS=1"
set "_VCVARS="

:: --- 1. Check if already activated (x64 + cl.exe + MS link.exe) ---

echo %INFO% MSBuild: Checking if 64-bit environment is already activated...
if not defined VSINSTALLDIR (
    echo %INFO% MSBuild: VSINSTALLDIR is not defined. Proceeding with search...
    goto :MSBUILD_SEARCH
)

call :VERIFY_X64_ENV
if not "%ERRORLEVEL%"=="0" (
    echo %WARN% MSBuild: Existing VSINSTALLDIR environment is not an active x64 build shell. Proceeding with search...
    goto :MSBUILD_SEARCH
)

call :VERIFY_MS_LINKER
if not "%ERRORLEVEL%"=="0" (
    echo %ERROR% MSBuild: Aborting due to link.exe collision in active environment.
    set "EXIT_STATUS=1"
    goto :MSBUILD_ACTIVATED
)

echo %INFO% MSBuild: VSINSTALLDIR points to "%VSINSTALLDIR%".
echo %OKOK% MSBuild: 64-bit environment already active. Skipping activation.
set "EXIT_STATUS=0"
goto :MSBUILD_ACTIVATED

:MSBUILD_SEARCH

:: --- 2. Check explicit environment variables (VCVARS, BUILDTOOLS, MSBUILDTOOLS) ---

echo %INFO% MSBuild: Checking if VCVARS points to vcvarsall.bat or vcvars64.bat.
if not defined VCVARS goto :SKIP_VCVARS
call :TRY_FILE "%VCVARS:"=%"
if not "%ERRORLEVEL%"=="0" (
    echo %WARN% MSBuild: VCVARS points to "%VCVARS%", but vcvars64.bat was not found there.
)
echo %INFO% MSBuild: VCVARS points to "%VCVARS%". Attempting activation...
if defined _VCVARS goto :MSBUILD_ACTIVATION

:SKIP_VCVARS

echo %INFO% MSBuild: Checking if BUILDTOOLS or MSBUILDTOOLS point to the "BuildTools" directory.

if not defined BUILDTOOLS goto :SKIP_BUILDTOOLS
call :TRY_ROOT "%BUILDTOOLS:"=%"
if not "%ERRORLEVEL%"=="0" (
    echo %WARN% MSBuild: BUILDTOOLS points to "%BUILDTOOLS%", but vcvarsall.bat/vcvars64.bat were not found there.
)
if defined _VCVARS goto :MSBUILD_ACTIVATION

:SKIP_BUILDTOOLS

if not defined MSBUILDTOOLS goto :SKIP_MSBUILDTOOLS
call :TRY_ROOT "%MSBUILDTOOLS:"=%"
if not "%ERRORLEVEL%"=="0" (
    echo %WARN% MSBuild: MSBUILDTOOLS points to "%MSBUILDTOOLS:"=%", but vcvarsall.bat/vcvars64.bat were not found there.
)
if defined _VCVARS goto :MSBUILD_ACTIVATION

:SKIP_MSBUILDTOOLS

:: --- 3. Check custom locations and junctions in dev ---

set "_MSVS=%~dp0..\Microsoft Visual Studio"
echo %INFO% MSBuild: Checking for custom locations of VS editions in "%_MSVS%\{VERSION}\{KIND}".
call :SCAN_VS_ROOT "%_MSVS%"
if defined _VCVARS goto :MSBUILD_ACTIVATION

echo %INFO% MSBuild: Checking custom fallback locations for BuildTools.
for %%D in (
    "%~dp0..\MSBuildTools"
    "%~dp0..\BuildTools"
    "%~d0\dev\MSBuildTools"
    "%~d0\dev\BuildTools"
) do if not defined _VCVARS call :TRY_ROOT "%%~fD"
if defined _VCVARS goto :MSBUILD_ACTIVATION

:: --- 4. Check PATH for vcvarsall.bat or BuildTools ---

echo %INFO% MSBuild: Checking if vcvarsall.bat is on the PATH.
for /f "usebackq tokens=* delims=" %%A in (
    `where.exe vcvarsall.bat 2^>nul`
) do if not defined _VCVARS call :TRY_FILE "%%~fA"
if defined _VCVARS (
    echo %INFO% MSBuild: Found vcvars64.bat via PATH: "%_VCVARS%". Attempting activation...
    goto :MSBUILD_ACTIVATION
)

echo %INFO% MSBuild: Checking if "BuildTools" or its parent are on the PATH.
set "_PATH="
if defined _PATH set "_PATH="%_PATH:;=";"%""
for %%A in (%_PATH%) do if not "%%~A"=="" (
    if not defined _VCVARS call :TRY_ROOT "%%~A"
    if not defined _VCVARS call :TRY_ROOT "%%~A\BuildTools"
)
if defined _VCVARS goto :MSBUILD_ACTIVATION

:: --- 5. Query vswhere.exe if available ---

set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%_VSWHERE%" (
    echo %INFO% MSBuild: Querying vswhere.exe for latest Visual Studio with C++ tools.
    for /f "usebackq delims=" %%I in (
        `"%_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`
    ) do if not defined _VCVARS call :TRY_ROOT "%%~fI"
)
if defined _VCVARS goto :MSBUILD_ACTIVATION

:: --- 6. Check default locations of VS editions (64-bit and 32-bit Program Files) ---

for %%P in (
    "%ProgramFiles%\Microsoft Visual Studio" 
    "%ProgramFiles(x86)%\Microsoft Visual Studio"
) do if not defined _VCVARS (
    echo %INFO% MSBuild: Checking for default locations of VS editions in "%%~P\{VERSION}\{KIND}".
    call :SCAN_VS_ROOT "%%~P"
)
if defined _VCVARS goto :MSBUILD_ACTIVATION

:: --- MSBuildTools not found ---

set "EXIT_STATUS=1"
echo %ERROR% MSBuild: MS Build Tools 64-bit activation script (vcvars64.bat) not found.
echo %ERROR% MSBuild: If MS Build Tools are installed, either
echo %ERROR% MSBuild:   - start this script from a preactivated x64 MS Build Tools shell
echo %ERROR% MSBuild:   - see script notes above regarding checked locations and variables
echo %ERROR% MSBuild: See this accepted SO answer https://stackoverflow.com/a/64262038/17472988
echo %ERROR% MSBuild: regarding MS Build Tools installation.
echo:
goto :MSBUILD_ACTIVATED

:MSBUILD_ACTIVATION

:: --- MSBuildTools found ---

if not exist "%_VCVARS%" (
    echo %ERROR% MSBuild: 64-bit activation script not found at "%_VCVARS%".
    set "EXIT_STATUS=1"
    goto :MSBUILD_ACTIVATED
)

set "VSCMD_SKIP_SENDTELEMETRY=1"
echo:
echo %WARN% MSBuild: Activating MS Build Tools.
echo %INFO% MSBuild: Calling "%_VCVARS%"
echo:
call "%_VCVARS%"
if not "%ERRORLEVEL%"=="0" (
    echo:
    echo %ERROR% MSBuild: vcvars64.bat returned an error.
    set "EXIT_STATUS=1"
    goto :MSBUILD_ACTIVATED
)
echo:

set "EXIT_STATUS=1"
call :VERIFY_X64_ENV && call :VERIFY_MS_LINKER && set "EXIT_STATUS=0"
if not "%EXIT_STATUS%"=="0" (
    echo %ERROR% MSBuild: MS Build Tools activation failed verification.
    echo %ERROR% MSBuild: Used script "%_VCVARS%".
    goto :MSBUILD_ACTIVATED
)

set "EXIT_STATUS=0"
echo %OKOK% MSBuild: MS Build Tools activation succeeded.
echo %OKOK% MSBuild: VSINSTALLDIR points to "%VSINSTALLDIR%".

:MSBUILD_ACTIVATED

set  "INFO="
set  "OKOK="
set  "WARN="
set "ERROR="
set "_MSVS="
set "_VCVARS="
set "_PATH="
set "_VSWHERE="
set "_CL_EXE="
set "_CL_DIR="
set "_LINK_EXE="
set "_LINK_DIR="
set "EXIT_STATUS=" & exit /b %EXIT_STATUS%


:: ============================================================================ SUBROUTINES BEGIN
:: ============================================================================
:TRY_FILE
if "%~1"=="" exit /b 1
if not exist "%~1" exit /b 1
if not exist "%~dp1vcvars64.bat" exit /b 1
set "_VCVARS=%~dp1vcvars64.bat"
exit /b 0

:TRY_ROOT
if "%~1"=="" exit /b 1
if not exist "%~1\VC\Auxiliary\Build\vcvarsall.bat" exit /b 1
if not exist "%~1\VC\Auxiliary\Build\vcvars64.bat" exit /b 1
set "_VCVARS=%~1\VC\Auxiliary\Build\vcvars64.bat"
exit /b 0

:SCAN_VS_ROOT
if "%~1"=="" exit /b 1
if not exist "%~1" exit /b 1
for /f "usebackq delims=" %%Y in (`dir /b /ad /O:-N "%~1" 2^>nul`) do (
    for /d %%E in ("%~1\%%Y\*") do if not defined _VCVARS call :TRY_ROOT "%%~fE"
)
if defined _VCVARS exit /b 0
exit /b 1

:VERIFY_X64_ENV
if not defined VSINSTALLDIR (
    echo %ERROR% MSBuild: VSINSTALLDIR is not defined.
    exit /b 1
)
call :VERIFY_VSDIR "%VSINSTALLDIR:"=%"
if not "%ERRORLEVEL%"=="0" (
    exit /b 1
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo %ERROR% MSBuild: VSCMD_ARG_TGT_ARCH is "%VSCMD_ARG_TGT_ARCH%" -- expected "x64".
    exit /b 1
)
where.exe /q cl.exe
if not "%ERRORLEVEL%"=="0" (
    echo %ERROR% MSBuild: Compiler cl.exe not found on PATH.
    exit /b 1
)
exit /b 0

:VERIFY_MS_LINKER
where.exe /q link.exe
if not "%ERRORLEVEL%"=="0" (
    echo %ERROR% MSBuild: Linker link.exe not found on PATH.
    exit /b 1
)

set "_CL_EXE="
set "_CL_DIR="
for /f "usebackq delims=" %%C in (`where.exe cl.exe 2^>nul`) do if not defined _CL_EXE (
    set "_CL_EXE=%%~fC"
    set "_CL_DIR=%%~dpC"
)

set "_LINK_EXE="
set "_LINK_DIR="
for /f "usebackq delims=" %%L in (`where.exe link.exe 2^>nul`) do if not defined _LINK_EXE (
    set "_LINK_EXE=%%~fL"
    set "_LINK_DIR=%%~dpL"
)

link.exe /? 2>&1 | findstr.exe /i /c:"Microsoft (R) Incremental Linker" /c:"/INCREMENTAL" >nul
if not "%ERRORLEVEL%"=="0" (
    echo %ERROR% MSBuild: Linker collision detected: link.exe is not the Microsoft Incremental Linker.
    echo %ERROR% MSBuild: Executing link.exe resolved to "%_LINK_EXE%" -- likely MSYS/Git/GNU link.exe.
    echo %ERROR% MSBuild: Expected MSVC toolset directory: "%_CL_DIR%".
    exit /b 1
)

if /i not "%_LINK_DIR%"=="%_CL_DIR%" (
    echo %ERROR% MSBuild: Linker collision detected: link.exe directory does not match active cl.exe directory.
    echo %ERROR% MSBuild: Resolved link.exe: "%_LINK_EXE%"
    echo %ERROR% MSBuild: Active cl.exe:     "%_CL_EXE%"
    exit /b 1
)

echo %INFO% MSBuild: Verified Microsoft linker: "%_LINK_EXE%".
exit /b 0

:VERIFY_VSDIR
if "%~1"=="" (
    echo %ERROR% MSBuild: VSINSTALLDIR is empty.
    exit /b 1
)
if not exist "%~1\VC\Auxiliary\Build\vcvarsall.bat" (
    echo %ERROR% MSBuild: vcvarsall.bat not found under "%~1".
    exit /b 1
)
if not exist "%~1\VC\Auxiliary\Build\vcvars64.bat" (
    echo %ERROR% MSBuild: vcvars64.bat not found under "%~1".
    exit /b 1
)
exit /b 0
:: ============================================================================
:: ============================================================================ SUBROUTINES END


:: ============================================================================ COLOR_SCHEME BEGIN
:: ============================================================================
:COLOR_SCHEME
if defined NOCOLOR (
    set  "INFO= [INFO]  "
    set  "OKOK= [-OK-]  "
    set  "WARN= [WARN]  "
    set "ERROR= [ERROR] "
) else (
    set  "INFO=[100;92m [INFO]  [0m"
    set  "OKOK=[103;94m [-OK-]  [0m"
    set  "WARN=[106;35m [WARN]  [0m"
    set "ERROR=[105;34m [ERROR] [0m"
)
exit /b 0
:: ============================================================================
:: ============================================================================ COLOR_SCHEME END
