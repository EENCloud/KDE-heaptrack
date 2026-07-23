.DEFAULT_GOAL := default
ifdef PRESET
	preset ?= $(PRESET)
else
	preset ?= release
endif
ifdef JOBS
	jobs ?= $(JOBS)
else
	jobs ?= 3
endif
user ?= $(USER)
ci_lock ?= false
een_ports_rev := $(shell jq -r '.["$$een_ports_revision"]' vcpkg.json)

build_dir = $(CURDIR)/build

.PHONY: help
help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

##@ Build

default: ## Build (default preset)
	$(MAKE) build

deps: ## Install/update vcpkg dependencies for the current preset
	install_deps --triplet-preset $(preset) --cache --auto-upgrade --ci-lock=$(ci_lock) --color

.PHONY: deps

config: ## Scan for new source files and regenerate the CMake build system
	@elevate chown -R $(user):$(user) $(build_dir) 2>/dev/null || true
	@cmake --preset linux --fresh

.PHONY: config

build: ## Build all targets (Ctrl+C kills ninja immediately)
	cmake --build --preset $(preset) -j$(jobs)

.PHONY: build

heaptrack_preload: ## Build only the heaptrack_preload target (the LD_PRELOAD tracker)
	@cmake --build --preset $(preset) -j$(jobs) --target heaptrack_preload

.PHONY: heaptrack_preload

clean: ## Remove build artifacts for the current preset
	@cmake --build --preset $(preset) -j$(jobs) --target clean

.PHONY: clean

install: ## Install build artifacts to system prefix
	elevate cmake --install $(build_dir) --config $(preset)

.PHONY: install
