# Keyloom — one-word targets for the build, like the sibling projects.
#
#   make             build (Release) and run the test suite
#   make build       build Release only, no test suite
#   make help        the full list
#
# Variables: JOBS=8  PREFIX=/usr or /usr/local  BUILD=build/release

BUILD   ?= build/release
JOBS    ?= $(shell nproc)
CMAKE   ?= cmake
PREFIX  ?= /usr/local

.PHONY: all help build debug test check install package clean

all: check

build:
	$(CMAKE) --preset release -DBUILD_TESTING=OFF
	$(CMAKE) --build $(BUILD) -j $(JOBS)

debug:
	$(CMAKE) --preset debug -DBUILD_TESTING=ON
	$(CMAKE) --build build/debug -j $(JOBS)

test:
	$(CMAKE) --preset release -DBUILD_TESTING=ON
	$(CMAKE) --build $(BUILD) -j $(JOBS)
	ctest --preset release

check: test

install: build
	$(CMAKE) --install $(BUILD) --prefix $(PREFIX)

package:
	cd packaging/arch && makepkg -s

# Delete the whole build directory.
clean:
	rm -rf build

help:
	@printf '%s\n' \
	  'Keyloom — make targets' \
	  '' \
	  '  make              build (Release) and run the test suite' \
	  '  make build        configure and build Release only, no test suite' \
	  '  make debug        configure and build Debug, with the test suite' \
	  '  make test         configure, build Release and run the test suite' \
	  '  make check        same as make test' \
	  '  make install      install into PREFIX (default /usr/local)' \
	  '  make package      build the Arch package with makepkg' \
	  '  make clean        delete the build directory' \
	  '' \
	  'Variables: JOBS=8  PREFIX=/usr/local  BUILD=build/release'
