@echo off
setlocal

rem Builds MiniDB when needed and opens the local Web demonstration service.
set "ROOT=%~dp0"
set "CPP_DIR=%ROOT%DBcompiler-main"
set "JAVA_DIR=%ROOT%minidb-engine"
set "BUILD_DIR=%CPP_DIR%\build"
set "JAR=%JAVA_DIR%\target\minidb-engine-1.0.0.jar"
set "PLAN_EXPORTER=%BUILD_DIR%\Debug\minisql_plan_json.exe"
set "CMAKE_EXE=cmake"
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [1/3] Configuring the C++ compiler...
    "%CMAKE_EXE%" -S "%CPP_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 exit /b 1
)

echo [1/3] Building the C++ SQL compiler...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Debug --parallel 1
if errorlevel 1 exit /b 1

echo [2/3] Building the Java Web service...
pushd "%JAVA_DIR%"
call mvn "-Dmaven.repo.local=target\maven-repo" "-Dmaven.test.skip=true" package
if errorlevel 1 (
    popd
    exit /b 1
)
popd

echo [3/3] Starting MiniDB Web at http://localhost:8080
echo [INFO] The Web service remains running until you press Ctrl+C in this window.
java --add-modules jdk.httpserver "-Dminidb.compiler.path=%PLAN_EXPORTER%" -Dminidb.web.openBrowser=true -jar "%JAR%" web
