# CMake 3.20+ and a C++20 compiler are required; installation is local by default.
CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= $(CURDIR)/.local
.DEFAULT_GOAL := help
.PHONY: help configure install dev test lint build clean package
help:
	@echo "make build test lint package install dev clean; override BUILD_DIR BUILD_TYPE PREFIX"
configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DGC_BUILD_BENCHMARKS=ON
build: configure
	$(CMAKE) --build $(BUILD_DIR) --config $(BUILD_TYPE) --parallel
install: build
	$(CMAKE) --install $(BUILD_DIR) --config $(BUILD_TYPE) --prefix "$(PREFIX)"
dev:
	$(MAKE) BUILD_DIR=build-debug BUILD_TYPE=Debug build
test: build
	$(CTEST) --test-dir $(BUILD_DIR) -C $(BUILD_TYPE) --output-on-failure
# The normal build compiles each public header and enables compiler warnings.
lint: build
package: build
	$(CMAKE) --install $(BUILD_DIR) --config $(BUILD_TYPE) --prefix "$(CURDIR)/dist/GCLib"
	$(CMAKE) -E tar cfvz dist/GCLib.tar.gz --format=gnutar dist/GCLib
clean:
	$(CMAKE) --build $(BUILD_DIR) --config $(BUILD_TYPE) --target clean
