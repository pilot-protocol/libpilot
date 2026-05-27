# C-side integration harness for libpilot.
#
# `make` builds libpilot.{dylib,so} with `-cover -coverpkg=.`, compiles
# harness.c against the generated header, runs the harness with
# GOCOVERDIR set, then folds the C-driven counters into a textfmt
# coverage profile that `go tool cover -func` can read.
#
# `make clean` removes build artifacts.

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  EXT := dylib
  RPATH_FLAG := -Wl,-rpath,@loader_path/..
endif
ifeq ($(UNAME_S),Linux)
  EXT := so
  RPATH_FLAG := -Wl,-rpath,'$$ORIGIN/..'
endif

LIB := ../libpilot.$(EXT)
COVDIR := ./covdata
HARNESS := harness

.PHONY: build run cover clean

build: $(LIB) $(HARNESS)

$(LIB):
	cd .. && go build -tags coverflush -cover -covermode=atomic -coverpkg=. -buildmode=c-shared -o libpilot.$(EXT) .
ifeq ($(UNAME_S),Darwin)
	# Fix install_name so dyld resolves via rpath instead of CWD lookup.
	install_name_tool -id @rpath/libpilot.$(EXT) $(LIB)
endif

$(HARNESS): harness.c $(LIB)
	$(CC) -Wall -Wextra -O0 -g -o $(HARNESS) harness.c -L.. -lpilot $(RPATH_FLAG)

run: $(HARNESS)
	mkdir -p $(COVDIR)
ifeq ($(UNAME_S),Darwin)
	DYLD_LIBRARY_PATH=.. GOCOVERDIR=$(COVDIR) ./$(HARNESS)
else
	LD_LIBRARY_PATH=.. GOCOVERDIR=$(COVDIR) ./$(HARNESS)
endif

cover: run
	go tool covdata textfmt -i=$(COVDIR) -o=coverage.out
	go tool cover -func=coverage.out | tail -5
	@printf "\nTotal lines covered by C harness:\n"
	@go tool cover -func=coverage.out | grep "^total:" || true

clean:
	rm -rf $(COVDIR) $(HARNESS) coverage.out
	rm -f ../libpilot.$(EXT) ../libpilot.h
