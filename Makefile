# Makefile for ALSA Audio Processor
CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -pthread
LDFLAGS = -lasound -pthread

# Debug flags
DEBUG_FLAGS = -g -DDEBUG -O0
RELEASE_FLAGS = -O3 -DNDEBUG -march=native

TARGET = audio_processor
SOURCE = audio_processor.cpp

# Default build
all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET)

# Release build
release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

# Clean
clean:
	rm -f $(TARGET)

# Install ALSA development libraries
install-deps:
	@echo "Installing ALSA development dependencies..."
	sudo apt-get update
	sudo apt-get install -y libasound2-dev build-essential

# List audio devices
list-devices:
	@echo "=== Audio Capture Devices ==="
	arecord -l
	@echo ""
	@echo "=== Audio Playback Devices ==="
	aplay -l
	@echo ""
	@echo "=== ALSA Cards ==="
	cat /proc/asound/cards

# Test audio setup
test-audio:
	@echo "Testing audio capture (5 seconds)..."
	timeout 5 arecord -f cd -t raw | aplay -f cd -
	@echo "Audio test completed"

# Run with default devices
run: $(TARGET)
	./$(TARGET)

# Run with specific devices (example)
run-hw: $(TARGET)
	./$(TARGET) hw:0,0 hw:0,0

# Run with USB audio devices
run-usb: $(TARGET)
	./$(TARGET) hw:1,0 hw:1,0

# Show ALSA configuration
show-config:
	@echo "=== ALSA Configuration ==="
	@echo "Default PCM device:"
	@cat ~/.asoundrc 2>/dev/null || echo "No user ALSA config found"
	@echo ""
	@echo "System ALSA config:"
	@cat /etc/asound.conf 2>/dev/null || echo "No system ALSA config found"
	@echo ""
	@echo "Available PCM devices:"
	@aplay -L | head -20

# Configure ALSA for low latency
configure-lowlatency:
	@echo "Configuring ALSA for low latency..."
	@echo "pcm.!default {" > ~/.asoundrc
	@echo "    type asym" >> ~/.asoundrc
	@echo "    playback.pcm \"playback\"" >> ~/.asoundrc
	@echo "    capture.pcm \"capture\"" >> ~/.asoundrc
	@echo "}" >> ~/.asoundrc
	@echo "" >> ~/.asoundrc
	@echo "pcm.playback {" >> ~/.asoundrc
	@echo "    type plug" >> ~/.asoundrc
	@echo "    slave {" >> ~/.asoundrc
	@echo "        pcm \"hw:0,0\"" >> ~/.asoundrc
	@echo "        period_time 5805" >> ~/.asoundrc
	@echo "        buffer_time 23220" >> ~/.asoundrc
	@echo "    }" >> ~/.asoundrc
	@echo "}" >> ~/.asoundrc
	@echo "" >> ~/.asoundrc
	@echo "pcm.capture {" >> ~/.asoundrc
	@echo "    type plug" >> ~/.asoundrc
	@echo "    slave {" >> ~/.asoundrc
	@echo "        pcm \"hw:0,0\"" >> ~/.asoundrc
	@echo "        period_time 5805" >> ~/.asoundrc
	@echo "        buffer_time 23220" >> ~/.asoundrc
	@echo "    }" >> ~/.asoundrc
	@echo "}" >> ~/.asoundrc
	@echo "Low latency ALSA configuration created in ~/.asoundrc"

# Monitor system resources
monitor:
	@echo "Monitoring system resources (Ctrl+C to stop)..."
	@echo "CPU usage, Memory usage, Audio processes:"
	watch -n 1 'ps aux | grep -E "(alsa|audio|$(TARGET))" | grep -v grep; echo ""; free -h | head -2; echo ""; uptime'

.PHONY: all debug release clean install-deps list-devices test-audio run run-hw run-usb show-config configure-lowlatency monitor