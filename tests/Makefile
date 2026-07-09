# Host-side unit tests. No hardware or PlatformIO toolchain needed — these
# compile the shared, platform-independent code with the system C++ compiler.
#
#   make            # build + run everything
#   make rs485      # build + run just the RS485 codec tests
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O1 -g
HMI_SRC   := ../esp32-hmi/src
BUILD     := build

.PHONY: all rs485 clean
all: rs485

rs485: $(BUILD)/test_rs485_frame
	$(BUILD)/test_rs485_frame

$(BUILD)/test_rs485_frame: rs485/test_rs485_frame.cpp \
                           $(HMI_SRC)/rs485/rs485_frame.cpp \
                           $(HMI_SRC)/rs485/crc16.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(HMI_SRC) $^ -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
