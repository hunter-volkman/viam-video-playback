# Viam Video Playback Module Makefile

# Module configuration
MODULE_NAME = video-playback-module
BUILD_DIR = build
ARCHIVE_NAME = module.tar.gz
INSTALL_DIR = $(BUILD_DIR)/install

# Detect OS
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    NPROC_CMD = nproc
    CMAKE_PLATFORM_FLAGS = -DBoost_INCLUDE_DIR=/usr/local/include \
                          -DBoost_LIBRARY_DIR=/usr/local/lib
else ifeq ($(UNAME_S),Darwin)
    NPROC_CMD = sysctl -n hw.ncpu
    CMAKE_PLATFORM_FLAGS = 
endif

# Build types
.PHONY: all build debug release dist clean test help

# Default target
all: build

# Help target
help:
	@echo "Viam Video Playback Module Build System"
	@echo "======================================="
	@echo "Targets:"
	@echo "  build    - Build module in release mode (default)"
	@echo "  debug    - Build module in debug mode"
	@echo "  release  - Build module in release mode with optimizations"
	@echo "  dist     - Create distribution archive"
	@echo "  clean    - Remove all build artifacts"
	@echo "  test     - Run tests"
	@echo ""
	@echo "Environment variables:"
	@echo "  VIAM_CPP_SDK_HOME - Path to Viam C++ SDK (required)"

# Check prerequisites
check-env:
ifndef VIAM_CPP_SDK_HOME
	$(error VIAM_CPP_SDK_HOME is not set. Please set it to your Viam C++ SDK path)
endif

# Standard build (release)
build: check-env
	@echo "Building module for $(UNAME_S) $(UNAME_M)..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$(VIAM_CPP_SDK_HOME)" \
		$(CMAKE_PLATFORM_FLAGS)
	@$(MAKE) -C $(BUILD_DIR) -j$(shell $(NPROC_CMD))
	@echo "Build complete: $(BUILD_DIR)/$(MODULE_NAME)"

# Debug build
debug: check-env
	@echo "Building module in debug mode..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_PREFIX_PATH="$(VIAM_CPP_SDK_HOME)" \
		$(CMAKE_PLATFORM_FLAGS)
	@$(MAKE) -C $(BUILD_DIR) -j$(shell $(NPROC_CMD))
	@echo "Debug build complete: $(BUILD_DIR)/$(MODULE_NAME)"

# Create distribution package
dist: build
	@echo "Creating distribution package..."
	@mkdir -p $(INSTALL_DIR)/bin
	@mkdir -p $(INSTALL_DIR)/lib
	
	# Copy main executable
	@cp $(BUILD_DIR)/$(MODULE_NAME) $(INSTALL_DIR)/bin/
	@cp run.sh $(INSTALL_DIR)/
	@chmod +x $(INSTALL_DIR)/run.sh
	@cp meta.json $(INSTALL_DIR)/
	
	# Platform-specific library bundling
ifeq ($(UNAME_S),Darwin)
	@echo "Bundling core libraries for macOS..."
	# Copy Viam SDK library
	@if [ -f "$(VIAM_CPP_SDK_HOME)/lib/libviamsdk.dylib" ]; then \
		cp "$(VIAM_CPP_SDK_HOME)/lib/libviamsdk.dylib" $(INSTALL_DIR)/lib/; \
	elif [ -f "$(VIAM_CPP_SDK_HOME)/lib/libviamsdk.noabi.dylib" ]; then \
		cp "$(VIAM_CPP_SDK_HOME)/lib/libviamsdk.noabi.dylib" $(INSTALL_DIR)/lib/; \
	fi
	# Update library paths
	@install_name_tool -add_rpath @loader_path/../lib $(INSTALL_DIR)/bin/$(MODULE_NAME) 2>/dev/null || true
else ifeq ($(UNAME_S),Linux)
	@echo "Bundling core libraries for Linux..."
	# Copy all Viam SDK libraries and create symlinks
	@cp $(VIAM_CPP_SDK_HOME)/lib/libviamsdk.so* $(INSTALL_DIR)/lib/ 2>/dev/null || true
	@cp $(VIAM_CPP_SDK_HOME)/lib/libviamapi.so* $(INSTALL_DIR)/lib/ 2>/dev/null || true
	# Create necessary symlinks
	@cd $(INSTALL_DIR)/lib && \
		if [ -f libviamsdk.so.0.17.0 ]; then \
			ln -sf libviamsdk.so.0.17.0 libviamsdk.so.noabi; \
			ln -sf libviamsdk.so.0.17.0 libviamsdk.so; \
		fi
	# Also check for libviamapi symlinks if needed
	@cd $(INSTALL_DIR)/lib && \
		if [ -f libviamapi.so.0.17.0 ]; then \
			ln -sf libviamapi.so.0.17.0 libviamapi.so; \
		fi

	# Bundle FFmpeg libraries
	@echo "Bundling FFmpeg libraries..."
	@for lib in avformat avcodec avutil swscale swresample avfilter; do \
		find /usr/lib /usr/local/lib -name "lib$$lib.so*" 2>/dev/null | head -1 | xargs -I {} cp {} $(INSTALL_DIR)/lib/ 2>/dev/null || true; \
	done

	# Bundle gRPC and protobuf libraries
	@echo "Bundling gRPC and protobuf libraries..."
	@for lib in grpc++ grpc++_reflection protobuf; do \
		find /usr/lib /usr/local/lib -name "lib$$lib.so*" 2>/dev/null | head -1 | xargs -I {} cp {} $(INSTALL_DIR)/lib/ 2>/dev/null || true; \
	done
endif
	
	# Create archive
	@cd $(INSTALL_DIR) && tar -czf ../../$(ARCHIVE_NAME) .
	@echo "Distribution package created: $(ARCHIVE_NAME)"
	@echo "Package size: $$(du -h $(ARCHIVE_NAME) | cut -f1)"
	@echo ""
	@echo "Archive contents:"
	@tar -tzf $(ARCHIVE_NAME) | head -20

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(ARCHIVE_NAME) bin/ lib/
	@echo "Clean complete"

# Install locally for testing
install-local: build
	@echo "Installing locally for testing..."
	@mkdir -p ~/viam-modules/video-playback
	@cp -r $(BUILD_DIR)/$(MODULE_NAME) ~/viam-modules/video-playback/
	@cp meta.json ~/viam-modules/video-playback/
	@echo "Installed to ~/viam-modules/video-playback"