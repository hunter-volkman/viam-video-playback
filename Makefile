# Variables
MODULE_NAME = video-playback-module
BUILD_DIR = build
ARCHIVE_NAME = module.tar.gz
INSTALL_DIR = $(BUILD_DIR)/install

# Default target
all: build

# Build the module using CMake
build:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Release
	$(MAKE) -C $(BUILD_DIR) -j$(shell sysctl -n hw.ncpu)

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