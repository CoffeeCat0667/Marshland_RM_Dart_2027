# Focused incremental ARM checks while the root CMake assembly is unavailable.
# Run from the repository root inside WSL. No clean target is provided.
CXX := aarch64-linux-gnu-g++
BUILD_DIR ?= cmake-build-ubuntu-arm/atom02-check

ifeq ($(filter aarch64% arm%,$(shell $(CXX) -dumpmachine)),)
$(error An ARM-targeting compiler is required)
endif

CPPFLAGS := -I. -IAtoms/Public -IAtoms/ATOM-02_State_Arbitration/Inc -isystem ThirdParty
CXXFLAGS := -std=c++23 -g -O0 -Wall -Wextra -Wpedantic -Werror -pthread -MMD -MP
LDLIBS := -pthread

.PHONY: all
all: $(BUILD_DIR)/state_machine_tests $(BUILD_DIR)/entry_smoke $(BUILD_DIR)/state_entry_tests \
     $(BUILD_DIR)/http.o $(BUILD_DIR)/control.o $(BUILD_DIR)/example.o

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/state.o: Atoms/ATOM-02_State_Arbitration/Src/StateArbitration.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/runtime.o: Atoms/ATOM-01_Runtime/Src/RuntimeCoordinator.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -IAtoms/ATOM-01_Runtime/Inc $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests.o: Tests/StateMachineTests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/entry.o: Entry.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/entry_tests.o: Tests/StateMachineEntryTests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/http.o: Atoms/ATOM-16_HTTP_Web/Src/HttpServer.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/control.o: Atoms/ATOM-08_Control/Src/control_loop.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -IAtoms/ATOM-08_Control/Inc $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/example.o: Atoms/ATOM-01_Runtime/Example_Integration.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -IAtoms/ATOM-01_Runtime/Inc $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/state_machine_tests: $(BUILD_DIR)/tests.o $(BUILD_DIR)/state.o $(BUILD_DIR)/runtime.o
	$(CXX) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/entry_smoke: $(BUILD_DIR)/entry.o $(BUILD_DIR)/state.o $(BUILD_DIR)/runtime.o
	$(CXX) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/state_entry_tests: $(BUILD_DIR)/entry_tests.o $(BUILD_DIR)/state.o $(BUILD_DIR)/runtime.o
	$(CXX) $^ $(LDLIBS) -o $@

-include $(wildcard $(BUILD_DIR)/*.d)
