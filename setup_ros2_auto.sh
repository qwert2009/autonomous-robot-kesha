#!/bin/bash
# Кеша v7.0 — Автоматическая установка ROS2 Jazzy + rosbridge в WSL2
# Запуск: wsl -d Ubuntu-24.04 -- bash /mnt/c/Desktop/robot/autonomous-robot-kesha-main/setup_ros2_auto.sh
set -e

export DEBIAN_FRONTEND=noninteractive
export LANG=en_US.UTF-8

echo "══════════════════════════════════════════"
echo "  КЕША — ROS2 Jazzy Auto-Setup"
echo "══════════════════════════════════════════"

# 1. Locale
echo "[1/6] Locale..."
sudo apt-get update -qq
sudo apt-get install -y -qq locales > /dev/null 2>&1
sudo locale-gen en_US en_US.UTF-8 > /dev/null 2>&1
sudo update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8

# 2. ROS2 repo
echo "[2/6] ROS2 repository..."
sudo apt-get install -y -qq software-properties-common curl > /dev/null 2>&1
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
    -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | \
    sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null
sudo apt-get update -qq

# 3. ROS2 Jazzy (base — без GUI для WSL, экономим место)
echo "[3/6] Installing ROS2 Jazzy (ros-base)... это займёт несколько минут"
sudo apt-get install -y -qq ros-jazzy-ros-base \
    python3-colcon-common-extensions \
    python3-rosdep \
    python3-vcstool > /dev/null 2>&1

# 4. Nav2 + SLAM + rosbridge
echo "[4/6] Nav2 + SLAM + rosbridge..."
sudo apt-get install -y -qq \
    ros-jazzy-navigation2 \
    ros-jazzy-nav2-bringup \
    ros-jazzy-slam-toolbox \
    ros-jazzy-robot-localization \
    ros-jazzy-tf2-ros \
    ros-jazzy-tf-transformations \
    ros-jazzy-rosbridge-suite \
    ros-jazzy-rosbridge-server > /dev/null 2>&1

# 5. rosdep init
echo "[5/6] rosdep..."
if [ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]; then
    sudo rosdep init 2>/dev/null || true
fi
rosdep update 2>/dev/null || true

# 6. bashrc setup
echo "[6/6] Configuring bashrc..."
grep -q "source /opt/ros/jazzy/setup.bash" ~/.bashrc 2>/dev/null || \
    echo "source /opt/ros/jazzy/setup.bash" >> ~/.bashrc

grep -q "KESHA_ROS2" ~/.bashrc 2>/dev/null || cat >> ~/.bashrc << 'EOF'

# ── Кеша ROS2 ──
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
alias kesha_bridge='ros2 launch rosbridge_server rosbridge_websocket_launch.xml port:=9090 address:=0.0.0.0'
alias kesha_slam='ros2 launch slam_toolbox online_async_launch.py'
alias kesha_nav='ros2 launch nav2_bringup navigation_launch.py use_sim_time:=false'
# KESHA_ROS2
EOF

echo ""
echo "══════════════════════════════════════════"
echo "  ROS2 Jazzy установлен!"
echo "  Команды:"
echo "    kesha_bridge  — запуск rosbridge (порт 9090)"
echo "    kesha_slam    — запуск SLAM"
echo "    kesha_nav     — запуск Nav2"
echo "══════════════════════════════════════════"
