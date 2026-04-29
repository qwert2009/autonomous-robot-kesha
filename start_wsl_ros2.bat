@echo off
chcp 65001 >nul
title Кеша v7.0 — ROS2 WSL2

echo ══════════════════════════════════════════════
echo   Кеша v7.0 — Запуск ROS2 в WSL2
echo ══════════════════════════════════════════════
echo.

:: Проверка WSL
wsl -l -v | findstr /i "Ubuntu" >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] Ubuntu WSL не найден!
    echo     Запустите setup_wsl.bat для установки.
    pause
    exit /b 1
)

echo Выберите режим:
echo.
echo   [1] Полный запуск (rosbridge + SLAM + micro-ROS)
echo   [2] Только rosbridge (WebSocket мост)
echo   [3] rosbridge + SLAM
echo   [4] Подключить ESP32 USB
echo   [5] Терминал WSL
echo.
set /p choice="Выбор (1-5): "

if "%choice%"=="1" goto full
if "%choice%"=="2" goto bridge
if "%choice%"=="3" goto bridge_slam
if "%choice%"=="4" goto usb
if "%choice%"=="5" goto terminal
goto full

:full
echo.
echo Запускаю полный стек ROS2...
echo (rosbridge:9090 + SLAM + micro-ROS:8888)
echo.
echo Для остановки: Ctrl+C
echo.
wsl -d Ubuntu-24.04 -- bash -c "source /opt/ros/jazzy/setup.bash && source ~/kesha_ws/install/setup.bash 2>/dev/null; echo '=== rosbridge (9090) + SLAM + micro-ROS (8888) ===' && ros2 launch kesha_bringup kesha_nav.launch.py"
goto end

:bridge
echo.
echo Запускаю rosbridge WebSocket на порту 9090...
echo.
wsl -d Ubuntu-24.04 -- bash -c "source /opt/ros/jazzy/setup.bash && ros2 launch rosbridge_server rosbridge_websocket_launch.xml port:=9090 address:=0.0.0.0"
goto end

:bridge_slam
echo.
echo Запускаю rosbridge + SLAM...
echo.
start "rosbridge" cmd /c "wsl -d Ubuntu-24.04 -- bash -c \"source /opt/ros/jazzy/setup.bash && ros2 launch rosbridge_server rosbridge_websocket_launch.xml port:=9090 address:=0.0.0.0\""
timeout /t 3 >nul
start "SLAM" cmd /c "wsl -d Ubuntu-24.04 -- bash -c \"source /opt/ros/jazzy/setup.bash && ros2 launch slam_toolbox online_async_launch.py\""
echo.
echo rosbridge и SLAM запущены в отдельных окнах.
echo.
pause
goto end

:usb
echo.
echo === Подключение ESP32 USB к WSL2 ===
echo.
echo Доступные USB устройства:
usbipd list
echo.
set /p busid="Введите BUSID устройства ESP32 (например 1-3): "
echo.
echo Привязываю устройство %busid%...
usbipd bind --busid %busid% 2>nul
echo Подключаю к WSL...
usbipd attach --wsl --busid %busid%
if %errorLevel% equ 0 (
    echo.
    echo ESP32 подключён к WSL! ✓
    echo В WSL устройство: /dev/ttyUSB0 или /dev/ttyACM0
    echo.
    echo Запуск micro-ROS Agent через Serial:
    echo   wsl -d Ubuntu-24.04
    echo   ros2 run micro_ros_agent micro_ros_agent serial --dev /dev/ttyUSB0
) else (
    echo [!] Ошибка подключения. Проверьте BUSID.
)
echo.
pause
goto end

:terminal
echo.
echo Открываю WSL Ubuntu...
wsl -d Ubuntu-24.04
goto end

:end
