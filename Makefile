# a3kpacker - unpack / repack Yamaha A3000 .a3k archives.
#
# Two independent implementations are provided and round-trip tested:
#   * a3kpacker.py    (Python)
#   * a3kpack-cli     (C++, built from a3kpack-cli.cpp + a3kPack.cpp)
#
# The original .a3k files live in ./originals and are never touched; all work
# happens in ./packLab (Python) and ./packLab-cli (C++).
#
#   make unpack-all   -> unpack every originals/*.a3k with the Python tool
#   make repack-all   -> repack every unpacked dir with the Python tool
#   make verify       -> Python round-trip check (unpack(repack) vs original)
#   make cli          -> build the C++ a3kpack-cli binary
#   make cli-unpack-all / cli-repack-all / cli-verify  (C++ equivalent)
#   make all          -> run both Python and C++ round-trip suites
#   make clean        -> remove packLab, packLab-cli and the CLI binary
#
# Note: filenames may contain spaces, so the recipes use shell globs
# (`for f in originals/*.a3k`) rather than make's whitespace-splitting
# $(wildcard).

PY       := python3
TOOL     := a3kpacker.py
PACKLAB  := packLab

CXX      ?= c++
CXXFLAGS := -std=c++20 -O2
CLI      := a3kpack-cli
CLI_SRC  := a3kpack-cli.cpp
A3KPACK  := a3kPack.cpp
A3KPACK_H:= a3kPack.h
A3KPACK_INC := .
CLIPACK  := packLab-cli

.PHONY: all cli unpack-all repack-all verify \
        cli-unpack-all cli-repack-all cli-verify clean

all: unpack-all repack-all verify cli-unpack-all cli-repack-all cli-verify

# ---------------------------------------------------------------------------
# C++ CLI
# ---------------------------------------------------------------------------

$(CLI): $(CLI_SRC) $(A3KPACK) $(A3KPACK_H)
	$(CXX) $(CXXFLAGS) -I$(A3KPACK_INC) $(CLI_SRC) $(A3KPACK) -o $(CLI)

cli: $(CLI)

# ---------------------------------------------------------------------------
# Python round-trip
# ---------------------------------------------------------------------------

unpack-all:
	@mkdir -p $(PACKLAB)
	@for f in originals/*.a3k; do \
		base="$$(basename "$$f" .a3k)"; \
		echo "== unpack $$f"; \
		$(PY) $(TOOL) unpack "$$f" "$(PACKLAB)/$$base.unpacked"; \
	done

repack-all:
	@mkdir -p $(PACKLAB)/repacked
	@for d in $(PACKLAB)/*.unpacked; do \
		[ -d "$$d" ] || continue; \
		base="$${d%.unpacked}"; \
		name="$${base##*/}"; \
		echo "== repack $$name"; \
		$(PY) $(TOOL) pack "$$d" "$(PACKLAB)/repacked/$$name.a3k"; \
	done

verify: unpack-all repack-all
	@fail=0; \
	for f in originals/*.a3k; do \
		base="$$(basename "$$f" .a3k)"; \
		$(PY) $(TOOL) unpack "$(PACKLAB)/repacked/$$base.a3k" "$(PACKLAB)/$$base.verify" >/dev/null 2>&1; \
		if diff -r "$(PACKLAB)/$$base.unpacked" "$(PACKLAB)/$$base.verify" >/dev/null 2>&1; then \
			echo "OK   : $$base"; \
		else \
			echo "FAIL : $$base"; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL ARCHIVES ROUND-TRIP OK"; else echo "ROUND-TRIP FAILURES DETECTED"; exit 1; fi

# ---------------------------------------------------------------------------
# C++ round-trip
# ---------------------------------------------------------------------------

cli-unpack-all: $(CLI)
	@mkdir -p $(CLIPACK)
	@for f in originals/*.a3k; do \
		base="$$(basename "$$f" .a3k)"; \
		echo "== cli unpack $$f"; \
		./$(CLI) unpack "$$f" "$(CLIPACK)/$$base.unpacked" >/dev/null; \
	done

cli-repack-all: $(CLI)
	@mkdir -p $(CLIPACK)/repacked
	@for d in $(CLIPACK)/*.unpacked; do \
		[ -d "$$d" ] || continue; \
		base="$${d%.unpacked}"; \
		name="$${base##*/}"; \
		echo "== cli repack $$name"; \
		./$(CLI) pack "$$d" "$(CLIPACK)/repacked/$$name.a3k" >/dev/null; \
	done

cli-verify: cli-unpack-all cli-repack-all
	@fail=0; \
	for f in originals/*.a3k; do \
		base="$$(basename "$$f" .a3k)"; \
		./$(CLI) unpack "$(CLIPACK)/repacked/$$base.a3k" "$(CLIPACK)/$$base.verify" >/dev/null 2>&1; \
		if diff -r "$(CLIPACK)/$$base.unpacked" "$(CLIPACK)/$$base.verify" >/dev/null 2>&1; then \
			echo "OK   : $$base"; \
		else \
			echo "FAIL : $$base"; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL ARCHIVES C++ ROUND-TRIP OK"; else echo "C++ ROUND-TRIP FAILURES DETECTED"; exit 1; fi

clean:
	rm -rf $(PACKLAB) $(CLIPACK) $(CLI)
