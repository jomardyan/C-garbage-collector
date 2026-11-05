# CMake 3.20+, Git and a C++20 compiler; installation stays local by default.
CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
BUILD_TYPE ?= Release
DIST_DIR ?= dist
PREFIX ?= $(CURDIR)/.local
JOBS ?= 2
CMAKE_ARGS ?=
.DEFAULT_GOAL := help
.NOTPARALLEL:
.PHONY: help configure install dev test lint check build clean package guard-build guard-dist
help: ## Show supported commands without configuring a build
	$(info make configure/build/test/lint/check/package/install/dev/clean)
	$(info configure/build: Configure CMake and compile the library, examples and benchmarks.)
	$(info test/check: Compile public-header checks and run CTest including installed consumers.)
	$(info package: Stage installed files, project documentation and copyright in dist/GCLib.tar.gz.)
	$(info install/dev/clean: Explicit local installation, separate Debug build, or guarded output cleanup.)
	$(info Override CMAKE, CTEST, BUILD_DIR, BUILD_TYPE, DIST_DIR, PREFIX, JOBS and CMAKE_ARGS.)
guard-build:
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(BUILD_DIR)" -P cmake/MakeOutputs.cmake
guard-dist:
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(DIST_DIR)" -P cmake/MakeOutputs.cmake
configure: guard-build ## Configure the library, examples, tests and benchmarks
	"$(CMAKE)" -S . -B "$(BUILD_DIR)" "-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)" -DGC_BUILD_BENCHMARKS=ON $(CMAKE_ARGS)
build: configure ## Compile the C++ library and configured targets
	"$(CMAKE)" --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --parallel "$(JOBS)"
install: build ## Install to PREFIX, which defaults to the repository-local .local
	"$(CMAKE)" --install "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --prefix "$(PREFIX)"
dev: ## Build a separate Debug tree using recursive GNU Make
	"$(MAKE)" BUILD_DIR=build-debug BUILD_TYPE=Debug build
test: build ## Run CTest, including the installed-package consumer checks
	"$(CTEST)" --test-dir "$(BUILD_DIR)" -C "$(BUILD_TYPE)" --output-on-failure
# Building also compiles every public header and enables compiler warnings.
lint: build ## Compile public-header checks with the project warning settings
check: lint test ## Build and run all configured tests in order
package: build guard-dist ## Stage installed library, project documentation and copyright in a tar.gz
	"$(CMAKE)" -E make_directory "$(DIST_DIR)"
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(DIST_DIR)/GCLib" -DCLEAN=ON -P cmake/MakeOutputs.cmake
	"$(CMAKE)" --install "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --prefix "$(abspath $(DIST_DIR))/GCLib"
	"$(CMAKE)" -E copy README.md COPYRIGHT.md "$(DIST_DIR)/GCLib/"
	"$(CMAKE)" -E chdir "$(DIST_DIR)" "$(CMAKE)" -E tar cfvz GCLib.tar.gz --format=gnutar GCLib
clean: ## Remove only the guarded selected build tree and distribution directory
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(BUILD_DIR)" -P cmake/MakeOutputs.cmake
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(DIST_DIR)" -P cmake/MakeOutputs.cmake
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(BUILD_DIR)" -DCLEAN=ON -P cmake/MakeOutputs.cmake
	"$(CMAKE)" "-DROOT=$(CURDIR)" "-DOUTPUT=$(DIST_DIR)" -DCLEAN=ON -P cmake/MakeOutputs.cmake
