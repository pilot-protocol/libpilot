.PHONY: build clean test

# Detect platform → extension
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  EXT := dylib
endif
ifeq ($(UNAME_S),Linux)
  EXT := so
endif
ifeq ($(OS),Windows_NT)
  EXT := dll
endif

LIB := libpilot.$(EXT)

build:
	go build -buildmode=c-shared -o $(LIB) .
	@echo "built $(LIB) + libpilot.h"

clean:
	rm -f libpilot.* libpilot

test:
	go test ./...
