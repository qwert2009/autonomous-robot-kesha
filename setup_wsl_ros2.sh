#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
#  КЕША v7.0 — Установка ROS2 Jazzy + Nav2 + SLAM в WSL2
# ═══════════════════════════════════════════════════════════════════════
#
#  Запуск: В WSL Ubuntu терминале:
#    chmod +x setup_wsl_ros2.sh
#    ./setup_wsl_ros2.sh
#
#  Что делает:
#    1. Устанавливает ROS2 Jazzy (Ubuntu 24.04)
#    2. Nav2 — автономная навигация
#    3. SLAM Toolbox — построение карты
#    4. rosbridge — WebSocket мост (порт 9090)
#    5. micro-ROS Agent — связь с ESP32
#    6. Настраивает автозапуск
#
#  Требования:
#    - WSL2 с Ubuntu 24.04
#    - Установленный usbipd-win на Windows (для USB серийного порта)
#
# ═══════════════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  КЕША — ROS2 Jazzy Setup for WSL2       ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════╝${NC}"

# ── Проверка WSL2 ──
if ! grep -qi microsoft /proc/version 2>/dev/null; then
    echo -e "${YELLOW}⚠ Не обнаружен WSL. Скрипт рассчитан на WSL2 Ubuntu 24.04${NC}"
    echo "Продолжить? (y/n)"
    read -r answer
    if [ "$answer" != "y" ]; then exit 1; fi
fi

# ── Проверка Ubuntu 24.04 ──
. /etc/os-release 2>/dev/null || true
if [ "$VERSION_ID" != "24.04" ]; then
    echo -e "${YELLOW}⚠ Ожидается Ubuntu 24.04, обнаружено: ${VERSION_ID:-unknown}${NC}"
    echo "ROS2 Jazzy требует Ubuntu 24.04. Продолжить? (y/n)"
    read -r answer
    if [ "$answer" != "y" ]; then exit 1; fi
fi

echo ""
echo -e "${GREEN}[1/7] Обновление системы...${NC}"
sudo apt update && sudo apt upgrade -y

echo ""
echo -e "${GREEN}[2/7] Установка ROS2 Jazzy...${NC}"

# Локаль
sudo apt install -y locales
sudo locale-gen en_US en_US.UTF-8
sudo update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
export LANG=en_US.UTF-8

# Ключи репозитория
sudo apt install -y software-properties-common curl
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
    -o /usr/share/keyrings/ros-archive-keyring.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | \
    sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null

sudo apt update

# ROS2 Jazzy Desktop (включает rviz2, rqt, etc.)
sudo apt install -y ros-jazzy-desktop

# Colcon build tools
sudo apt install -y python3-colcon-common-extensions python3-rosdep python3-vcstool

# Инициализация rosdep
if [ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]; then
    sudo rosdep init
fi
rosdep update

echo ""
echo -e "${GREEN}[3/7] Установка Nav2 — автономная навигация...${NC}"
sudo apt install -y \
    ros-jazzy-navigation2 \
    ros-jazzy-nav2-bringup \
    ros-jazzy-slam-toolbox \
    ros-jazzy-robot-localization \
    ros-jazzy-tf2-ros \
    ros-jazzy-tf-transformations

echo ""
echo -e "${GREEN}[4/7] Установка rosbridge — WebSocket мост...${NC}"
sudo apt install -y \
    ros-jazzy-rosbridge-suite \
    ros-jazzy-rosbridge-server

echo ""
echo -e "${GREEN}[5/7] Установка micro-ROS Agent...${NC}"

# micro-ROS Agent — связь с ESP32 через Serial/WiFi
sudo apt install -y \
    ros-jazzy-micro-ros-agent \
    || {
        echo -e "${YELLOW}micro-ROS Agent не в стандартных пакетах. Собираем из исходников...${NC}"
        mkdir -p ~/microros_ws/src
        cd ~/microros_ws/src
        if [ ! -d "micro-ROS-Agent" ]; then
            git clone -b jazzy https://github.com/micro-ROS/micro-ROS-Agent.git
        fi
        cd ~/microros_ws
        source /opt/ros/jazzy/setup.bash
        rosdep install --from-paths src --ignore-src -y
        colcon build --packages-select micro_ros_agent
        echo "source ~/microros_ws/install/setup.bash" >> ~/.bashrc
    }

echo ""
echo -e "${GREEN}[6/7] Настройка Кеша-специфичных параметров...${NC}"

# Создать рабочее пространство Кеши
mkdir -p ~/kesha_ws/src
cd ~/kesha_ws/src

# Создать launch-файл для Кеши
mkdir -p kesha_bringup/launch
cat > kesha_bringup/launch/kesha_nav.launch.py << 'LAUNCH_EOF'
"""
Кеша v7.0 — ROS2 Launch файл для WSL2
Запускает: rosbridge + SLAM + Nav2 + micro-ROS Agent
"""
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription, ExecuteProcess
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory
import os


def generate_launch_description():
    # Параметры робота Кеша
    robot_radius = 0.12  # 12 см
    max_vel = 0.3  # 30 см/с
    use_sim_time = LaunchConfiguration('use_sim_time', default='false')

    # ── rosbridge WebSocket (порт 9090) ──
    rosbridge = Node(
        package='rosbridge_server',
        executable='rosbridge_websocket',
        name='rosbridge_websocket',
        parameters=[{
            'port': 9090,
            'address': '0.0.0.0',
            'use_sim_time': use_sim_time,
        }],
        output='screen',
    )

    # ── SLAM Toolbox (Online Async) ──
    slam = Node(
        package='slam_toolbox',
        executable='async_slam_toolbox_node',
        name='slam_toolbox',
        parameters=[{
            'use_sim_time': use_sim_time,
            'resolution': 0.05,
            'max_laser_range': 3.5,
            'minimum_travel_distance': 0.1,
            'minimum_travel_heading': 0.2,
            'map_update_interval': 2.0,
        }],
        output='screen',
    )

    # ── Nav2 ──
    nav2_dir = get_package_share_directory('nav2_bringup')
    nav2_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(nav2_dir, 'launch', 'navigation_launch.py')
        ),
        launch_arguments={
            'use_sim_time': 'false',
            'autostart': 'true',
            'params_file': '',
        }.items(),
    )

    # ── micro-ROS Agent (WiFi UDP, порт 8888) ──
    micro_ros_agent = ExecuteProcess(
        cmd=[
            'ros2', 'run', 'micro_ros_agent', 'micro_ros_agent',
            'udp4', '--port', '8888',
        ],
        output='screen',
    )

    # ── Static TF: base_link → laser ──
    tf_base_laser = Node(
        package='tf2_ros',
        executable='static_transform_publisher',
        arguments=[
            '0', '0', '0.05', '0', '0', '0',
            'base_link', 'laser',
        ],
    )

    return LaunchDescription([
        DeclareLaunchArgument('use_sim_time', default_value='false'),
        rosbridge,
        slam,
        tf_base_laser,
        micro_ros_agent,
        # nav2_launch,  # Раскомментировать когда SLAM стабильно работает
    ])
LAUNCH_EOF

# Создать package.xml
cat > kesha_bringup/package.xml << 'PKG_EOF'
<?xml version="1.0"?>
<package format="3">
  <name>kesha_bringup</name>
  <version>7.0.0</version>
  <description>Кеша robot bringup — ROS2 launch files</description>
  <maintainer email="kesha@robot.local">Kesha Team</maintainer>
  <license>MIT</license>
  <exec_depend>rosbridge_server</exec_depend>
  <exec_depend>slam_toolbox</exec_depend>
  <exec_depend>navigation2</exec_depend>
  <exec_depend>nav2_bringup</exec_depend>
  <exec_depend>micro_ros_agent</exec_depend>
  <exec_depend>tf2_ros</exec_depend>
  <export>
    <build_type>ament_python</build_type>
  </export>
</package>
PKG_EOF

# Nav2 параметры для Кеши
mkdir -p ~/kesha_ws/config
cat > ~/kesha_ws/config/kesha_nav2_params.yaml << 'NAV2_EOF'
# Кеша v7.0 — Nav2 Parameters
# Робот: 4WD mecanum, радиус 12см, макс скорость 30см/с

bt_navigator:
  ros__parameters:
    use_sim_time: false
    global_frame: map
    robot_base_frame: base_link
    odom_topic: /odom
    bt_loop_duration: 10
    default_server_timeout: 20

controller_server:
  ros__parameters:
    use_sim_time: false
    controller_frequency: 10.0
    min_x_velocity_threshold: 0.01
    min_theta_velocity_threshold: 0.05
    FollowPath:
      plugin: "dwb_core::DWBLocalPlanner"
      max_vel_x: 0.3
      min_vel_x: -0.1
      max_vel_y: 0.3       # Mecanum: движение вбок!
      min_vel_y: -0.3
      max_vel_theta: 1.5
      min_speed_xy: 0.0
      max_speed_xy: 0.3
      min_speed_theta: 0.0
      acc_lim_x: 0.5
      acc_lim_y: 0.5
      acc_lim_theta: 2.0
      decel_lim_x: -0.5
      decel_lim_y: -0.5
      decel_lim_theta: -2.0
      xy_goal_tolerance: 0.15
      yaw_goal_tolerance: 0.2
      transform_tolerance: 0.5

planner_server:
  ros__parameters:
    use_sim_time: false
    planner_plugins: ["GridBased"]
    GridBased:
      plugin: "nav2_navfn_planner/NavfnPlanner"
      tolerance: 0.2
      use_astar: true
      allow_unknown: true

local_costmap:
  local_costmap:
    ros__parameters:
      update_frequency: 5.0
      publish_frequency: 2.0
      global_frame: odom
      robot_base_frame: base_link
      rolling_window: true
      width: 3
      height: 3
      resolution: 0.05
      robot_radius: 0.12
      inflation_radius: 0.25

global_costmap:
  global_costmap:
    ros__parameters:
      update_frequency: 1.0
      publish_frequency: 1.0
      global_frame: map
      robot_base_frame: base_link
      robot_radius: 0.12
      resolution: 0.05
      track_unknown_space: true
      inflation_radius: 0.25
NAV2_EOF

echo ""
echo -e "${GREEN}[7/7] Настройка .bashrc...${NC}"

# Добавить source в .bashrc (если ещё нет)
BASHRC_MARKER="# KESHA ROS2 SETUP"
if ! grep -q "$BASHRC_MARKER" ~/.bashrc; then
    cat >> ~/.bashrc << 'BASHRC_EOF'

# KESHA ROS2 SETUP
source /opt/ros/jazzy/setup.bash
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
# WSL2: разрешить подключения с Windows
export ROS_LOCALHOST_ONLY=0

# Кеша workspace
if [ -d ~/kesha_ws/install ]; then
    source ~/kesha_ws/install/setup.bash
fi

# Алиасы для быстрого запуска
alias kesha-start='ros2 launch kesha_bringup kesha_nav.launch.py'
alias kesha-bridge='ros2 launch rosbridge_server rosbridge_websocket_launch.xml port:=9090 address:=0.0.0.0'
alias kesha-slam='ros2 launch slam_toolbox online_async_launch.py'
alias kesha-microros='ros2 run micro_ros_agent micro_ros_agent udp4 --port 8888'
alias kesha-topics='ros2 topic list'
alias kesha-nodes='ros2 node list'
BASHRC_EOF
    echo -e "${GREEN}✓ .bashrc обновлён${NC}"
else
    echo -e "${YELLOW}✓ .bashrc уже настроен${NC}"
fi

# Собрать workspace
cd ~/kesha_ws
source /opt/ros/jazzy/setup.bash
# colcon build  # Раскомментировать после первого полного запуска

echo ""
echo -e "${GREEN}═══════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✓ ROS2 Jazzy установлен!${NC}"
echo -e "${GREEN}  ✓ Nav2 + SLAM + rosbridge + micro-ROS${NC}"
echo -e "${GREEN}═══════════════════════════════════════════${NC}"
echo ""
echo -e "${YELLOW}СЛЕДУЮЩИЕ ШАГИ:${NC}"
echo ""
echo "1. Перезапустить WSL:"
echo "   (в PowerShell): wsl --shutdown"
echo "   Затем открыть Ubuntu заново"
echo ""
echo "2. Запустить ROS2 для Кеши:"
echo "   kesha-bridge    # WebSocket мост (порт 9090)"
echo "   kesha-slam      # SLAM (отдельный терминал)"
echo "   kesha-microros  # micro-ROS Agent (отдельный терминал)"
echo ""
echo "3. USB для ESP32 (в PowerShell от админа):"
echo "   usbipd list                    # найти ESP32"
echo "   usbipd bind --busid <BUSID>    # привязать"
echo "   usbipd attach --wsl --busid <BUSID>  # подключить к WSL"
echo ""
echo "4. В Кеше v7.0 установить:"
echo "   ROS2_UBUNTU_IP=localhost  (или WSL IP)"
echo "   ROS2_BRIDGE_PORT=9090"
echo ""
echo -e "${GREEN}Готово! 🤖${NC}"
