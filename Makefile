# Building needs only HCNetSDK.h from Hikvision's SDK: put the unpacked SDK under sdk/ or pass SDK_INC=<dir>.
SDK_INC ?= $(firstword $(wildcard sdk/*/incEn))
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -I$(SDK_INC) -DHIKVIEW_VERSION='"$(VERSION)"'
LDFLAGS += -static-libstdc++ -static-libgcc
LDLIBS = -lX11 -ldl

SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:src/%.cpp=build/%.o)
DIST = hikview-$(VERSION)-linux-x86_64

build/hikview: $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp $(wildcard src/*.h) | check-sdk
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Rebuild main.o every time so the version string stays current.
build/main.o: .FORCE

check-sdk:
	@test -f "$(SDK_INC)/HCNetSDK.h" || { echo "HCNetSDK.h not found: unpack the Hikvision SDK zip into sdk/ or pass SDK_INC=<dir with HCNetSDK.h>"; exit 1; }

dist: build/hikview
	rm -rf dist/$(DIST) && mkdir -p dist/$(DIST)
	cp build/hikview README.md LICENSE dist/$(DIST)/
	strip dist/$(DIST)/hikview
	tar -C dist -czf dist/$(DIST).tar.gz $(DIST)
	@echo dist/$(DIST).tar.gz

clean:
	rm -rf build dist

.PHONY: check-sdk dist clean .FORCE
