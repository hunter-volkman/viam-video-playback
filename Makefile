# Makefile for Viam C++ Module

# Variables
MODULE_NAME = video-playback-module
BUILD_DIR = build
ARCHIVE_NAME = module.tar.gz
INSTALL_DIR = $(BUILD_DIR)/install

# OS-specific commands
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    NPROC_CMD = nproc
endif
ifeq ($(UNAME_S),Darwin)
    NPROC_CMD = sysctl -n hw.ncpu
endif

# Default target
all: build

# Build the module using CMake
build:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Release
	$(MAKE) -C $(BUILD_DIR) -j$(shell $(NPROC_CMD))

# Create the distributable .tar.gz archive
dist: build
	mkdir -p $(INSTALL_DIR)/bin
	cp $(BUILD_DIR)/$(MODULE_NAME) $(INSTALL_DIR)/bin/
	cp run.sh $(INSTALL_DIR)/
	cd $(INSTALL_DIR) && tar -czvf ../../$(ARCHIVE_NAME) .
	@echo "Created distribution package: $(ARCHIVE_NAME)"

# Clean the build artifacts
clean:
	rm -rf $(BUILD_DIR) $(ARCHIVE_NAME)

.PHONY: all build dist clean
