#!/bin/bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  УСТАНОВКА КЕШИ v3.0 — Скрипт для ПК (Ubuntu/Linux)        ║
# ║  RTX 3050 | 16GB RAM | 700GB SSD                            ║
# ╚══════════════════════════════════════════════════════════════╝
#
# Запуск:
#   chmod +x setup_kesha.sh
#   ./setup_kesha.sh

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════╗"
echo "║     🤖 УСТАНОВКА КЕШИ v3.0                  ║"
echo "║     Автономный домашний робот                ║"
echo "╚══════════════════════════════════════════════╝"
echo -e "${NC}"

KESHA_DIR="$HOME/kesha"

# ── 1. Создание рабочей папки ──
echo -e "${YELLOW}[1/8] Создаю рабочую папку...${NC}"
mkdir -p "$KESHA_DIR"/{models,voices,logs}

# ── 2. Python venv ──
echo -e "${YELLOW}[2/8] Создаю Python виртуальное окружение...${NC}"
if [ ! -d "$KESHA_DIR/venv" ]; then
    python3 -m venv "$KESHA_DIR/venv"
fi
source "$KESHA_DIR/venv/bin/activate"

# ── 3. Зависимости Python ──
echo -e "${YELLOW}[3/8] Устанавливаю Python зависимости...${NC}"
pip install --upgrade pip
pip install \
    fastapi \
    "uvicorn[standard]" \
    faster-whisper \
    ultralytics \
    python-multipart \
    httpx \
    pillow \
    numpy \
    yandex-music \
    feedparser

# ── 4. Ollama ──
echo -e "${YELLOW}[4/8] Проверяю Ollama...${NC}"
if ! command -v ollama &> /dev/null; then
    echo -e "${CYAN}Устанавливаю Ollama...${NC}"
    curl -fsSL https://ollama.com/install.sh | sh
else
    echo -e "${GREEN}Ollama уже установлена${NC}"
fi

# ── 5. Модель LLM ──
echo -e "${YELLOW}[5/8] Скачиваю модель Dolphin-Gemma2 9B...${NC}"
if ! ollama list 2>/dev/null | grep -q "dolphin-gemma2:9b-q4_K_M"; then
    echo -e "${CYAN}Скачивается ~5.5 GB...${NC}"
    ollama pull dolphin-gemma2:9b-q4_K_M
else
    echo -e "${GREEN}Модель уже скачана${NC}"
fi

# ── 6. Piper TTS (русский голос) ──
echo -e "${YELLOW}[6/8] Скачиваю Piper TTS и русский голос...${NC}"
PIPER_DIR="$KESHA_DIR/piper"
if [ ! -f "$PIPER_DIR/piper" ]; then
    mkdir -p "$PIPER_DIR"
    cd "$PIPER_DIR"

    # Определяем архитектуру
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ]; then
        PIPER_URL="https://github.com/rhasspy/piper/releases/latest/download/piper_linux_x86_64.tar.gz"
    elif [ "$ARCH" = "aarch64" ]; then
        PIPER_URL="https://github.com/rhasspy/piper/releases/latest/download/piper_linux_aarch64.tar.gz"
    fi

    echo "Скачиваю Piper ($ARCH)..."
    curl -L "$PIPER_URL" -o piper.tar.gz
    tar xzf piper.tar.gz --strip-components=1
    rm piper.tar.gz
    chmod +x piper
    cd -
else
    echo -e "${GREEN}Piper уже установлен${NC}"
fi

# Русский голос
VOICE_DIR="$KESHA_DIR/voices"
if [ ! -f "$VOICE_DIR/ru_RU-ruslan-medium.onnx" ]; then
    echo "Скачиваю русский голос (Ruslan)..."
    curl -L "https://huggingface.co/rhasspy/piper-voices/resolve/main/ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx" \
        -o "$VOICE_DIR/ru_RU-ruslan-medium.onnx"
    curl -L "https://huggingface.co/rhasspy/piper-voices/resolve/main/ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx.json" \
        -o "$VOICE_DIR/ru_RU-ruslan-medium.onnx.json"
else
    echo -e "${GREEN}Голос Ruslan уже скачан${NC}"
fi

# ── 7. Whisper модель (предзагрузка) ──
echo -e "${YELLOW}[7/8] Предзагрузка Whisper small (при первом запуске ~500MB)...${NC}"
python3 -c "
from faster_whisper import WhisperModel
print('Загружаю faster-whisper small...')
model = WhisperModel('small', device='cuda', compute_type='float16')
print('Whisper готов!')
" 2>/dev/null || echo -e "${YELLOW}Whisper скачается при первом запуске${NC}"

# ── 8. YOLOv8 модель ──
echo -e "${YELLOW}[8/8] Предзагрузка YOLOv8n...${NC}"
python3 -c "
from ultralytics import YOLO
print('Загружаю YOLOv8n...')
model = YOLO('yolov8n.pt')
print('YOLO готов!')
" 2>/dev/null || echo -e "${YELLOW}YOLOv8 скачается при первом запуске${NC}"

# ── Копируем скрипт мозга ──
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/robot_brain_v3.py" ]; then
    cp "$SCRIPT_DIR/robot_brain_v3.py" "$KESHA_DIR/robot_brain_v3.py"
    echo -e "${GREEN}robot_brain_v3.py → $KESHA_DIR/${NC}"
fi

# ── Создаём скрипт запуска ──
cat > "$KESHA_DIR/start_kesha.sh" << 'STARTEOF'
#!/bin/bash
# Запуск Кеши
echo "🤖 Запуск Кеши v3.0..."

KESHA_DIR="$HOME/kesha"
cd "$KESHA_DIR"

# Активируем venv
source "$KESHA_DIR/venv/bin/activate"

# Проверяем Ollama
if ! pgrep -x "ollama" > /dev/null; then
    echo "Запускаю Ollama..."
    ollama serve &
    sleep 3
fi

# Переменные окружения для Piper
export PIPER_PATH="$KESHA_DIR/piper/piper"
export PIPER_VOICE="$KESHA_DIR/voices/ru_RU-ruslan-medium.onnx"

# Запуск!
echo "Запускаю мозг..."
python3 "$KESHA_DIR/robot_brain_v3.py"
STARTEOF
chmod +x "$KESHA_DIR/start_kesha.sh"

# ── Создаём systemd сервис (опционально) ──
cat > "$KESHA_DIR/kesha.service" << SVCEOF
[Unit]
Description=Kesha Robot Brain v3
After=network.target ollama.service

[Service]
Type=simple
User=$USER
WorkingDirectory=$KESHA_DIR
Environment=PIPER_PATH=$KESHA_DIR/piper/piper
Environment=PIPER_VOICE=$KESHA_DIR/voices/ru_RU-ruslan-medium.onnx
ExecStart=$KESHA_DIR/venv/bin/python $KESHA_DIR/robot_brain_v3.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
SVCEOF

# ── Итоги ──
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════╗"
echo -e "║  ✅ УСТАНОВКА ЗАВЕРШЕНА!                      ║"
echo -e "╚══════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Рабочая папка:  ${CYAN}$KESHA_DIR${NC}"
echo -e "  Запуск:         ${CYAN}$KESHA_DIR/start_kesha.sh${NC}"
echo -e "  Веб-панель:     ${CYAN}http://localhost:8000/docs${NC}"
echo ""
echo -e "  ${YELLOW}Для автозапуска:${NC}"
echo -e "  sudo cp $KESHA_DIR/kesha.service /etc/systemd/system/"
echo -e "  sudo systemctl enable kesha"
echo -e "  sudo systemctl start kesha"
echo ""
echo -e "  ${YELLOW}Занято места:${NC}"
du -sh "$KESHA_DIR" 2>/dev/null || true
echo ""
echo -e "  ${YELLOW}VRAM:${NC} ~7.5 GB (LLM 6 + Whisper 1 + YOLO 0.5)"
echo -e "  ${YELLOW}RAM:${NC}  ~3-4 GB"
echo ""
