BUILD ?= build
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CXX_COMPILER ?= clang++
else
CXX_COMPILER ?= g++
endif

.PHONY: all test clean

all:
	cmake -S . -B $(BUILD) -DCMAKE_CXX_COMPILER=$(CXX_COMPILER)
	cmake --build $(BUILD) -j

test: all
	./$(BUILD)/11dim-ai-test --self-test

clean:
	rm -rf $(BUILD)
