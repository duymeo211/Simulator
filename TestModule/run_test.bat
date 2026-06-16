@echo off
setlocal EnableDelayedExpansion

REM ============================================================================
REM CAN Integration Test — automated batch runner
REM
REM Usage:
REM   run_test.bat                        run ALL two test suites in sequence
REM   run_test.bat original               run test_original.yaml only
REM   run_test.bat runtime                run test_runtime.yaml only
REM   run_test.bat --config my.yaml       run a custom yaml file
REM   run_test.bat --parse-only           parse existing log without running
REM   run_test.bat original --duration 20 override duration for a suite
REM
REM Exit code:  0 = all PASS,  1 = one or more FAIL
REM ============================================================================

REM ---- resolve script directory so this bat works from any CWD ----
set "SCRIPT_DIR=%~dp0"
set "PYTHON=C:/Users/DuyNX13/AppData/Local/Python/bin/python.exe"
set "RUNNER=%SCRIPT_DIR%run_test.py"

REM ---- map short suite aliases to yaml file names ----
set "SUITE_original=%SCRIPT_DIR%/Testcases/test_original.yaml"
set "SUITE_runtime=%SCRIPT_DIR%/Testcases/test_runtime.yaml"

REM ---- check for explicit --config / --parse-only (pass straight through) ----
set "FIRST_ARG=%~1"
if /i "%FIRST_ARG%"=="--config"     goto :passthrough
if /i "%FIRST_ARG%"=="--parse-only" goto :passthrough
if /i "%FIRST_ARG%"=="--duration"   goto :passthrough

REM ---- check for short suite alias ----
if /i "%FIRST_ARG%"=="original" (
    set "CONFIG=!SUITE_original!"
    shift
    goto :run_single
)
if /i "%FIRST_ARG%"=="runtime" (
    set "CONFIG=!SUITE_runtime!"
    shift
    goto :run_single
)

REM ---- no first arg (or unrecognised) → run ALL suites ----
if "%FIRST_ARG%"=="" goto :run_all
goto :run_all

REM ===========================================================================
:run_all
REM Run every suite in sequence.  Track overall exit code.
REM ===========================================================================
echo.
echo ============================================================
echo  CAN Integration Test -- FULL AUTOMATED RUN
echo  Running: original  /  runtime
echo ============================================================
echo.

set "OVERALL=0"

echo [1/2] test_original.yaml
echo -------------------------------------------------------
%PYTHON% "%RUNNER%" --config "%SUITE_original%"
if errorlevel 1 set "OVERALL=1"
echo.

echo [2/2] test_runtime.yaml
echo -------------------------------------------------------
%PYTHON% "%RUNNER%" --config "%SUITE_runtime%" --duration 20
if errorlevel 1 set "OVERALL=1"
echo.

echo ============================================================
if "%OVERALL%"=="0" (
    echo  RESULT: ALL SUITES PASSED
) else (
    echo  RESULT: ONE OR MORE SUITES FAILED
)
echo ============================================================
echo.
pause
exit /b %OVERALL%

REM ===========================================================================
:run_single
REM Run one named suite; remaining args (e.g. --duration) are passed through.
REM ===========================================================================
echo.
echo ============================================================
echo  CAN Integration Test -- %CONFIG%
echo ============================================================
echo.

%PYTHON% "%RUNNER%" --config "%CONFIG%" %1 %2 %3 %4 %5
set "RC=%errorlevel%"

echo.
echo ============================================================
if "%RC%"=="0" (
    echo  RESULT: PASSED
) else (
    echo  RESULT: FAILED
)
echo ============================================================
echo.
pause
exit /b %RC%

REM ===========================================================================
:passthrough
REM Unknown first arg — pass everything straight to Python as-is.
REM ===========================================================================
echo.
%PYTHON% "%RUNNER%" %*
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%
