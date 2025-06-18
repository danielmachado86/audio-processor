# Makefile para el procesador de audio en Raspberry Pi
CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -pthread
LDFLAGS = -lasound -pthread -lm

TARGET = audio_processor
SOURCE = audio_processor.cpp

# Directorios
SRCDIR = src
OBJDIR = obj
BINDIR = bin

# Crear directorios si no existen
$(shell mkdir -p $(OBJDIR) $(BINDIR))

# Regla principal
$(BINDIR)/$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Regla de limpieza
clean:
	rm -rf $(OBJDIR) $(BINDIR)

# Regla para instalar dependencias en Raspberry Pi
install-deps:
	sudo apt-get update
	sudo apt-get install -y libasound2-dev build-essential

# Regla para configurar audio en Raspberry Pi
setup-audio:
	@echo "Configurando audio en Raspberry Pi..."
	@echo "1. Verificando dispositivos de audio disponibles:"
	aplay -l
	@echo ""
	@echo "2. Configurando ALSA..."
	sudo modprobe snd-bcm2835
	@echo "3. Configuración completada"

# Regla para ejecutar con privilegios de audio
run: $(BINDIR)/$(TARGET)
	@echo "Iniciando procesador de audio..."
	@echo "Asegúrate de tener un micrófono y altavoces conectados"
	sudo $(BINDIR)/$(TARGET)

# Regla para debug
debug: CXXFLAGS += -g -DDEBUG
debug: $(BINDIR)/$(TARGET)

# Regla para optimización máxima
release: CXXFLAGS += -O3 -DNDEBUG -march=native
release: $(BINDIR)/$(TARGET)

# Regla para verificar la configuración del sistema
check-system:
	@echo "=== Verificación del sistema ==="
	@echo "Kernel: $(shell uname -r)"
	@echo "Procesador: $(shell cat /proc/cpuinfo | grep 'model name' | head -1 | cut -d':' -f2)"
	@echo "Memoria: $(shell free -h | grep Mem | awk '{print $$2}')"
	@echo ""
	@echo "=== Configuración de audio ==="
	@echo "Dispositivos de reproducción:"
	@aplay -l 2>/dev/null || echo "No se encontraron dispositivos de reproducción"
	@echo ""
	@echo "Dispositivos de captura:"
	@arecord -l 2>/dev/null || echo "No se encontraron dispositivos de captura"
	@echo ""
	@echo "=== Configuración ALSA ==="
	@cat /proc/asound/cards 2>/dev/null || echo "No se encontró información de tarjetas de sonido"

.PHONY: clean install-deps setup-audio run debug release check-system