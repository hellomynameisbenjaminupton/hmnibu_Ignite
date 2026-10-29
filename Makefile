# Behringer IGNITE (BE 0716-AAF) Custom Firmware Makefile
# SPDX-License-Identifier: AGPL-3.0-or-later

CXX      ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O2 -I. -ISource/Application/CustomSequencer -IAudIO/CustomEngines -IComProtocol/PktMgr
BUILD_DIR := build/tests

PHASE1_BIN := $(BUILD_DIR)/test_ignite_phase1
PHASE2_BIN := $(BUILD_DIR)/test_ignite_phase2
PHASE3_BIN := $(BUILD_DIR)/test_ignite_phase3
PHASE4_BIN := $(BUILD_DIR)/test_ignite_phase4

.PHONY: all test clean phase1 phase2 phase3 phase4

all: test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PHASE1_BIN): tests/test_ignite_phase1.cpp Source/Application/CustomSequencer/ignite_sequencer_core.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) tests/test_ignite_phase1.cpp -o $(PHASE1_BIN)

$(PHASE2_BIN): tests/test_ignite_phase2.cpp Source/Application/CustomSequencer/ignite_sequencer_core.hpp Source/Application/CustomSequencer/ignite_algorithmic_engines.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) tests/test_ignite_phase2.cpp -o $(PHASE2_BIN)

$(PHASE3_BIN): tests/test_ignite_phase3.cpp Source/Application/CustomSequencer/ignite_hardware_and_cv.hpp ComProtocol/PktMgr/PktMgr.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-unused-variable -Wno-unused-parameter -Wno-switch tests/test_ignite_phase3.cpp ComProtocol/PktMgr/PktMgr.cpp -o $(PHASE3_BIN)

$(PHASE4_BIN): tests/test_ignite_phase4.cpp Source/Application/CustomSequencer/ignite_firmware_app.hpp AudIO/CustomEngines/ignite_internal_synth.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) tests/test_ignite_phase4.cpp -o $(PHASE4_BIN)

phase1: $(PHASE1_BIN)
	./$(PHASE1_BIN)

phase2: $(PHASE2_BIN)
	./$(PHASE2_BIN)

phase3: $(PHASE3_BIN)
	./$(PHASE3_BIN)

phase4: $(PHASE4_BIN)
	./$(PHASE4_BIN)

test: phase1 phase2 phase3 phase4

clean:
	rm -rf $(BUILD_DIR)
