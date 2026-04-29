@echo off
chcp 65001 >nul
title Кеша v7.0 — WSL2 Setup + ROS2

echo ══════════════════════════════════════════════
echo   Кеша v7.0 — Установка WSL2 + ROS2 Jazzy
echo ══════════════════════════════════════════════
echo.

:: ── Проверка прав администратора ──
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] Нужны права администратора!
    echo     Правый клик → Запустить от админа
    pause
    exit /b 1
)

:: ── Шаг 1: Установка WSL2 ──
echo [1/4] Установка WSL2 с Ubuntu 24.04...
echo.

wsl --status >nul 2>&1
if %errorLevel% neq 0 (
    echo Устанавливаю WSL2...
    wsl --install -d Ubuntu-24.04
    echo.
    echo [!] WSL2 установлен. ПЕРЕЗАГРУЗИТЕ компьютер!
    echo     После перезагрузки запустите этот скрипт снова.
    pause
    exit /b 0
) else (
    echo WSL2 уже установлен ✓
)

:: Проверить что Ubuntu есть
wsl -l -v | findstr /i "Ubuntu" >nul 2>&1
if %errorLevel% neq 0 (
    echo Устанавливаю Ubuntu 24.04...
    wsl --install -d Ubuntu-24.04
    echo.
    echo [!] Ubuntu установлена. Создайте пользователя в открывшемся окне.
    echo     Затем запустите этот скрипт снова.
    pause
    exit /b 0
) else (
    echo Ubuntu 24.04 есть ✓
)

:: ── Шаг 2: Установка usbipd-win (для ESP32 USB) ──
echo.
echo [2/4] Проверка usbipd-win (для ESP32 USB в WSL)...

where usbipd >nul 2>&1
if %errorLevel% neq 0 (
    echo Устанавливаю usbipd-win через winget...
    winget install --id dorssel.usbipd-win --accept-source-agreements --accept-package-agreements
    if %errorLevel% neq 0 (
        echo [!] Не удалось установить автоматически.
        echo     Скачайте вручную: https://github.com/dorssel/usbipd-win/releases
    ) else (
        echo usbipd-win установлен ✓
    )
) else (
    echo usbipd-win уже установлен ✓
)

:: ── Шаг 3: Копировать скрипт установки ROS2 в WSL ──
echo.
echo [3/4] Копирую скрипт ROS2 в WSL...

set SCRIPT_DIR=%~dp0
wsl -d Ubuntu-24.04 -- bash -c "cp '/mnt/c/Desktop/robot/autonomous-robot-kesha-main/setup_wsl_ros2.sh' ~/setup_wsl_ros2.sh && chmod +x ~/setup_wsl_ros2.sh"
if %errorLevel% equ 0 (
    echo Скрипт скопирован ✓
) else (
    echo [!] Не удалось скопировать. Скопируйте вручную:
    echo     wsl cp /mnt/c/Desktop/robot/autonomous-robot-kesha-main/setup_wsl_ros2.sh ~/
)

:: ── Шаг 4: Запуск установки ROS2 ──
echo.
echo [4/4] Запускаю установку ROS2 в WSL...
echo       (Это займёт 10-20 минут)
echo.

wsl -d Ubuntu-24.04 -- bash -c "cd ~ && ./setup_wsl_ros2.sh"

echo.
echo ══════════════════════════════════════════════
echo   Установка завершена!
echo ══════════════════════════════════════════════
echo.
echo БЫСТРЫЙ СТАРТ:
echo.
echo   1. Открыть WSL:
echo      wsl -d Ubuntu-24.04
echo.
echo   2. Запустить ROS2:
echo      kesha-bridge     (rosbridge WebSocket)
echo      kesha-slam       (SLAM Toolbox)
echo      kesha-microros   (micro-ROS Agent)
echo.
echo   3. Запустить Кешу (Windows):
echo      start_kesha.bat
echo.
echo   ESP32 USB:
echo      usbipd list
echo      usbipd bind --busid X-X
echo      usbipd attach --wsl --busid X-X
echo.
pause
