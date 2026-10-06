# Which build this is. Every other library in the suite defines this; cutil did
# not, so its platform block below set the whole path and the release/debug
# distinction never reached the build tree at all.
BUILD ?= release

# The optimization level, and the one thing that should distinguish the two
# builds' compile flags. `release` is what gets installed and what every
# library above this one actually runs; `debug` is compiled for stepping
# through. -g stays in both, because a release build that cannot be read in a
# debugger is a release build nobody can diagnose, and symbols cost only file
# size.
#
# Before this existed, BUILD=debug renamed the artifact -- appending -debug to
# BRANCH and VERSION_STRING, below -- and changed nothing about how the code
# was compiled, so `make BUILD=debug` produced an -O3 library carrying a debug
# name: a debug build you cannot step through. The two blocks stay separate
# because this one has to precede CFLAGS and that one has to follow BRANCH.
#
# -O2 for the library. It was -O3, pre-existing and unmeasured, and the figure
# that was owed for it came out against it on 2026-09-23.
#
# Nineteen of the twenty-eight translation units compile to byte-identical
# .text at the two levels, so -O3 could only ever act on nine: array, file,
# hash, path, pool, sequencer, subprocess, thread, utf. What it does there,
# measured out-of-line through the shared library (21 paired trials each,
# interleaved and pinned, with callgrind instruction counts beside the clock):
#
#   gcu_path_join        -60%  time, -59.0% Ir      6 consumer call sites, all
#                                                   one-shot: leap-second file,
#                                                   timezone database paths
#   utf8/16 round trip   -36%  time, -32.5% Ir      no callers at all on Linux;
#                                                   every internal use is
#                                                   inside _WIN32, and the
#                                                   Linux .so contains zero
#                                                   calls to them
#   gcu_array_append           , -14.1% Ir          15 call sites, some hot
#   gcu_hash64 set+get         ,  +2.4% Ir          slower at -O3
#   gcu_path_basename    +7.8% time,   0.0% Ir      slower at -O3 for pure
#                                                   layout reasons: 6,919,730,671
#                                                   instructions against
#                                                   6,919,730,808
#
# Those are ceilings -- tight loops in which the called function is the whole
# workload. End to end, on programs that use this library for real:
#
#   model, 2.4 MB Stanford bunny OBJ    cutil 11.5% of instructions    -0.444%
#   ctang, 20k-iteration template       cutil  9.3% of instructions    -0.041%
#   compress, LZW benchmark             cutil below the 100-instruction
#                                       threshold in 107 billion       n/a
#
# ctang shows why the ceiling does not arrive: its cutil time is almost all
# vector.template.c, one of the nineteen TUs that are byte-identical. And two
# consumers cannot be affected at all -- image and regex make no call into any
# of the nine.
#
# Against that, -O3 costs +16.3% .text and +13.4% on the shipped .so (433,168
# to 491,224 bytes), on the one library every other one links. A tenth of a
# percent is not worth that, and it is not worth a function that gets slower
# because the code around it moved.
#
# Anything moving *to* -O3 needs a figure, and the figure has to be end to end
# in a consumer, per path, naming any path that crosses a library boundary. A
# microbenchmark of an exported function will say -60% and mean -0.4%.
#
# The tests get -O2 in release, and that is a measured choice rather than a
# copy of the library's. Roughly half of this library compiles into its
# *caller* -- seventeen inline functions across memory.h and safemath.h, plus
# the function-like macros in macros.h and mutex.h -- so the level the tests
# are built at is the level that half is tested at, and test-memory-inline.cpp
# says in its own file comment that it exists to pin "what the library itself
# and every consumer are built with". Every consumer is now -O2 or -O3, and
# those inlines were being tested at -O1. Compiling a translation unit that
# uses nothing but them:
#
#   -O1  60 instructions
#   -O2  77 instructions      <- -O1 was genuinely testing other code
#   -O3  77 instructions      <- byte-identical to -O2
#
# -O3 would cost about 30% more wall time on every `make test` and cannot
# change a single instruction of the code under test, because these are scalar
# wrappers and overflow checks with nothing for -O3's extra loop and
# vectorisation work to act on. -O2 is where the real change is.
#
# The sanitizer builds are pinned rather than left to follow the library, and
# that reverses what this comment said until the claim was measured. The
# argument for inheriting was that UB the optimizer only exploits at a high -O
# would be invisible to a gate pinned lower. Plausible, and false: eleven
# planted defects, one per program so that halting at the first cannot mask a
# later one, each built at -O0/-O1/-O2/-O3 under both compilers.
#
#   use-after-free, heap and stack overflow, use-after-return,
#   leak, signed overflow, bad shift, float-cast overflow    caught at all four
#   zero offset from a null pointer                          clang only, all four
#   strict aliasing                                          caught at NONE
#   a planted data race, under TSan                          caught at all four
#
# Not one class was detected at a high level and missed at a low one. Strict
# aliasing was the named hazard the inheriting argument rested on, and no
# sanitizer here detects it at any level, so the whole case was for a benefit
# that does not exist. (Measure leaks carefully: a first probe read as "lost
# above -O0" and was an elided allocation -- zero calls to malloc in the
# binary -- not a blind sanitizer.)
#
# Detection being level-independent, the level is chosen for diagnosis and
# speed instead: -O1 inlines less, so a stack trace names the frame you want.
# Pinning also stops the gate moving silently the next time the release level
# does, which is how this question arose across the suite in the first place.
# -O0 still available deliberately through BUILD=debug, for the run after the
# gate has told you there is something to look at.
#
# `make coverage` appends its own -O0 through EXTRA_CFLAGS and wins over the
# library level; the sanitizer flags come after that again, so test-asan is
# pinned against EXTRA_CFLAGS too. Do not copy a level from a sibling
# Makefile: measure it here.
ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
OPT_CXXFLAGS := -O0
SAN_OPT_CFLAGS := -O0
else
OPT_CFLAGS := -O2
OPT_CXXFLAGS := -O2
SAN_OPT_CFLAGS := -O1
endif

CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 $(OPT_CXXFLAGS) -g $(EXTRA_CXXFLAGS)
CC := cc
# GHOTIIO_CUTIL_BUILD enables DLL export on Windows (checked by GCU_API).
# GHOTIIO_CUTIL_TEST_BUILD would export internals for testing (checked by
# GCU_INTERNAL_API); it is deliberately not set here, so the shipped library
# exports its public API and nothing else.
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c17 $(OPT_CFLAGS) -g -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD $(EXTRA_CFLAGS)
# -DGHOTIIO_CUTIL_ENABLE_MEMORY_DEBUG
LDFLAGS := -L /usr/lib -lstdc++ -lm $(EXTRA_LDFLAGS)


SUITE := ghoti.io
PROJECT := cutil

# The version of this library. MINOR_VERSION carries the minor and the patch as
# one dotted string; the two are split out below for the places that need three
# separate integers. See CONVENTIONS.md section 4.
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
# Substituted into the .pc file; an empty Version: field makes every
# pkg-config version constraint fail.
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. It defaults to the major version, so an ordinary build
# of 1.x is "-1" and two majors cannot be loaded into one process by mistake.
# Override it for a build that wants its own identity:  make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

# What the library reports as its version. The branch is appended only when it
# is not the default, so an ordinary build says "1.2.3" and an overridden one
# says "1.2.3-dev". Computed before BUILD=debug rewrites BRANCH below.
ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# If BUILD is debug, append -debug.
#
# "override" because BRANCH may have come from the command line, and a
# command-line variable otherwise wins over a plain assignment here: without it
# `make BRANCH=-dev BUILD=debug` produced a debug build carrying the release
# token, whose symbols collide with the release build's.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

# The symbol namespace token. Derived from BRANCH so that the token inside every
# exported symbol is the same one that names the .pc file, the install directory
# and the shared library: "-dev" -> ghotiio_cutil_dev, "-1.0" -> ghotiio_cutil_1_0.
# Hand-writing it is the failure mode: every version then exports identical
# symbols, and two of them cannot be loaded into one process.
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')

BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
BASE_NAME := $(BASE_NAME_PREFIX).so
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)


# PC_INSTALL_PATH names where this project's own .pc file is installed.
# PKG_CONFIG_PATH is the environment's and is never assigned here: make exports
# an inherited variable with whatever value the makefile last gave it, so
# overwriting it handed every sub-make a different PKG_CONFIG_PATH from the
# parent's. The sub-make then derived different test flags, found the flag
# stamp changed, and rebuilt everything - which check-rebuild reports as a
# settled tree that will not settle. It showed first under MSYS2, whose login
# shell exports PKG_CONFIG_PATH, and happens on Linux whenever the exported
# value is not exactly the install location.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	override BUILD := linux/$(BUILD)
	LIB_EXTENSION := so
	OS_SPECIFIC_COMPILE_FLAGS := -fPIC
	OS_SPECIFIC_LINK_FLAGS := -shared -fPIC
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	override BUILD := mac/$(BUILD)
	LIB_EXTENSION := dylib
	OS_SPECIFIC_COMPILE_FLAGS :=
	OS_SPECIFIC_LINK_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	override BUILD := win32/$(BUILD)
	LIB_EXTENSION := dll
	OS_SPECIFIC_COMPILE_FLAGS :=
	OS_SPECIFIC_LINK_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	# Windows paths for .pc so gcc invoked by mingw32-make can resolve -I/-L (lazy: only when install runs)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	override BUILD := win64/$(BUILD)
	LIB_EXTENSION := dll
	OS_SPECIFIC_COMPILE_FLAGS :=
	OS_SPECIFIC_LINK_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	# Windows paths for .pc so gcc invoked by mingw32-make can resolve -I/-L (lazy: only when install runs)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))

else
    $(error Unsupported OS: $(UNAME_S))

endif
ifeq ($(OS_NAME), Windows)
# path.c asks GetUserProfileDirectoryW (userenv) for the home directory when
# USERPROFILE is not set, and temporary names come from BCryptGenRandom
# (bcrypt).  socket.c and loop.c are Winsock (ws2_32), and loop.c's AcceptEx
# lives in mswsock.
# Placed after the OS block because OS_NAME is not known before it.
LDFLAGS += -luserenv -lbcrypt -lws2_32 -lmswsock
endif

# ---------------------------------------------------------------------------
# Installation prefix
#
# Defaults to the system location chosen above. Override it to install
# somewhere else - the suite's bootstrap installs every library into a local
# prefix so that each build resolves its dependencies through pkg-config,
# exactly as a consumer would, rather than through a second code path that
# only in-tree builds exercise. See CONVENTIONS.md section 1.
#
#     make install PREFIX=/path/to/prefix
# ---------------------------------------------------------------------------
ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
# A non-system prefix has no /etc/ld.so.conf.d, and writing to it would need
# root anyway. Everything built here carries an rpath to the prefix instead.
LDCONF_INSTALL_PATH :=
endif

# Dependencies are looked up along the inherited PKG_CONFIG_PATH as well as the
# install location chosen above, so that exporting PKG_CONFIG_PATH works as the
# errors below say it does. The inherited value comes first: it is an explicit
# request for this build, where the install location may be only a default.
PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)

ifdef PREFIX
# So that a library, a test or an example finds its Ghoti.io dependencies in the
# prefix at run time without LD_LIBRARY_PATH.
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
endif


# Defined after the platform block above, which prepends the OS segment to
# BUILD. Defining it earlier expands $(BUILD) before that happens and silently
# drops the segment, putting every build in one directory per OS.
BUILD_DIR := ./build/$(BUILD)

OBJ_DIR := $(BUILD_DIR)/objects
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps

INCLUDE := -I include/ -I $(BUILD_DIR)/include/

# The flag set this tree's objects were built with, rewritten only when it
# changes so its mtime moves on a flag change and on nothing else. The object
# rules below depend on it.
#
# `Makefile` used to serve this purpose and was wrong in both directions: too
# broad, because a comment-only edit recompiled everything, and too narrow,
# because a command-line override such as `make EXTRA_CFLAGS=-O2` changes no
# file's mtime and so was invisible. The flag string sees both.
FLAGS_STAMP := $(OBJ_DIR)/.flags
LIBOBJECTS := \
  $(OBJ_DIR)/allocator.o \
	$(OBJ_DIR)/array.o \
	$(OBJ_DIR)/atomic.o \
	$(OBJ_DIR)/barrier.o \
	$(OBJ_DIR)/cond.o \
	$(OBJ_DIR)/dir.o \
	$(OBJ_DIR)/error.o \
	$(OBJ_DIR)/env.o \
	$(OBJ_DIR)/fiber.o \
	$(OBJ_DIR)/file.o \
	$(OBJ_DIR)/filelock.o \
	$(OBJ_DIR)/hash.o \
	$(OBJ_DIR)/library.o \
	$(OBJ_DIR)/loop.o \
	$(OBJ_DIR)/memory.o \
	$(OBJ_DIR)/mmap.o \
	$(OBJ_DIR)/once.o \
	$(OBJ_DIR)/path.o \
	$(OBJ_DIR)/pool.o \
	$(OBJ_DIR)/random.o \
	$(OBJ_DIR)/rwlock.o \
	$(OBJ_DIR)/semaphore.o \
	$(OBJ_DIR)/sequencer.o \
	$(OBJ_DIR)/socket.o \
	$(OBJ_DIR)/string.o \
	$(OBJ_DIR)/subprocess.o \
	$(OBJ_DIR)/thread.o \
	$(OBJ_DIR)/tls.o \
	$(OBJ_DIR)/type.o \
	$(OBJ_DIR)/utf.o \
	$(OBJ_DIR)/vector.o

TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`

# The checks `make test` runs besides the tests themselves. Named in a
# variable so that a build which cannot satisfy them can clear it: the
# coverage target does, because --coverage links the gcov runtime, whose
# mangle_path check-symbols is right to reject in a shipping library and
# wrong to reject in an instrumented one. Spelled as text's TEST_GATES is.
TEST_GATES ?= check-symbols check-win32-parse check-win32-linkage check-clang check-rebuild check-stamps check-fiber-defects check-loop-defects

# Used by check-clang. Empty when clang is not installed, which that
# target reports rather than failing over.
CLANG := $(shell command -v clang 2>/dev/null)

# Sources whose #ifdef _WIN32 bodies are parse-checked. Add a file here in
# the same commit that gives it a Windows branch, or the branch ships
# untokenised.
WIN32_PARSE_SOURCES := src/cond.c src/once.c src/rwlock.c src/error.c src/tls.c src/env.c src/library.c src/filelock.c src/mmap.c src/subprocess.c src/fiber.c src/socket.c src/loop.c



CUTILLIBRARY := -L $(APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)


all: $(APP_DIR)/$(TARGET) ## Build the shared library

####################################################################
# Dependency Inclusion
####################################################################
# Compiler-generated .d files (see -MMD -MP -MF in compile commands).
TEST_NAMES := test-macros test-type test-cond test-once test-rwlock test-error test-utf test-tls test-env test-library test-filelock test-atomic test-barrier test-mmap test-subprocess test-memory test-memory-inline test-hash test-mutex test-random test-semaphore test-string test-thread test-vector test-array test-allocator test-safemath test-safemath-portable test-pool test-sequencer test-fiber test-socket test-loop test-path test-file test-dir
TEST_BINARIES := $(foreach t,$(TEST_NAMES),$(APP_DIR)/$(t)$(EXE_EXTENSION))
TEST_DEPFILES := $(addprefix $(APP_DIR)/,$(TEST_NAMES:%=%.d))
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_DEPFILES)
-include $(DEPFILES)

####################################################################
# Floating Point Type Identification
####################################################################
FLOAT_IDENTIFIER := $(APP_DIR)/float_identifier$(EXE_EXTENSION)
# Built with $(CFLAGS), and the type names it prints become float.h, so a flag
# change has to rebuild it like any other object.
$(FLOAT_IDENTIFIER): \
		src/float_identifier.c \
		src/float.h.template \
		$(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/include/$(SUITE)/$(PROJECT):
	@mkdir -p $@

# The generated half of libver.h: the namespace token and version string.
# libver_gen.h is regenerated on every build and rewritten only when its content
# changes, so a variable given on the command line - make MAJOR_VERSION=2, or
# make BRANCH=-dev - takes effect. Keying the rule on the Makefile's timestamp
# alone left the previous token and version baked into the build, and nothing
# said so.
.PHONY: force-libver
force-libver:

$(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h: \
		force-libver \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace, and two\n" >&2; \
		printf "versions of this library could not be loaded into one process.\n" >&2; \
		exit 1; \
	fi
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTIIO_CUTIL_LIBVER_GEN_H' \
		'#define GHOTIIO_CUTIL_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_CUTIL_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_CUTIL_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_CUTIL_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_CUTIL_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_CUTIL_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTIIO_CUTIL_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# The directory is an order-only prerequisite, and that is not a style choice.
# A directory's mtime moves whenever a file is created or removed inside it, and
# the libver_gen.h rule above creates and removes $@.tmp there on every single
# build. As a normal prerequisite the directory therefore made float.h out of
# date on builds where nothing had changed, and because the recipe wrote $@
# unconditionally its mtime moved too -- so type.h's six dependants, the library
# and five tests recompiled. Measured 2026-09-23: with nothing edited at all,
# consecutive `make test` runs came out 6, 0, 6, 6 objects; creating and
# removing one unrelated file in that directory recompiled six on its own.
# Writing through $@.tmp and keeping the old file when the content matches is
# the second half of the fix: even when the rule does re-run, an unchanged
# float.h must not get a new mtime.
#
# The two halves shadow each other and both stay. Measured: reverting either
# one on its own leaves check-rebuild green, and only reverting both makes it
# report twelve targets -- so the gate cannot tell you which one is carrying
# the symptom, and finding that you cannot make one of them fail is not
# evidence that it is redundant. They answer different questions. The
# order-only prerequisite is what makes the rule's dependency list true; the
# guard is what keeps an unchanged float.h from getting a new mtime when the
# rule re-runs for a reason that is real, which it now can, since
# $(FLOAT_IDENTIFIER) is rebuilt on every flag change and prints the same two
# type names almost every time.
$(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h: \
		src/float.h.template \
		$(FLOAT_IDENTIFIER) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)
	@f32="$$($(FLOAT_IDENTIFIER) 32)"; f64="$$($(FLOAT_IDENTIFIER) 64)"; \
	if [ -z "$$f32" ] || [ -z "$$f64" ]; then \
		printf "### $(FLOAT_IDENTIFIER) produced no type name ###\n" >&2; \
		printf "Without it float.h defines GCU_float32_t/GCU_float64_t as nothing,\n" >&2; \
		printf "and the first file to include type.h fails with a syntax error that\n" >&2; \
		printf "says nothing about this step. Delete $(FLOAT_IDENTIFIER) and rebuild.\n" >&2; \
		exit 1; \
	fi; \
	sed "s/FLOAT32/$$f32/; s/FLOAT64/$$f64/" src/float.h.template > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

####################################################################
# Object Files
####################################################################

# Pattern rule: compile .c to .o and generate dependency file (compiler tracks headers).
# float.h is generated; ensure it exists before compiling any .c that may include it (e.g. type.h).
# $(FLAGS_STAMP) is a real prerequisite, not decoration: every flag these
# objects were built with is in that string, and `make` otherwise sees a .o
# newer than its .c and reuses it after a flag change. That is silent and it is
# specifically dangerous for the instrumented trees, where the flags *are* the
# semantics -- a sanitizer arm rebuilt without the flag you just added reports
# clean because the check was never compiled in. Two sessions in this
# workspace measured "no hazard" that way on 2026-09-22 before noticing they
# were comparing a binary with itself.
$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@ $(OS_SPECIFIC_COMPILE_FLAGS)

# Extra source dependencies not seen by the compiler (included via macros).
$(OBJ_DIR)/hash.o: src/hash.template.c
$(OBJ_DIR)/vector.o: src/vector.template.c

####################################################################
# Shared Library
####################################################################

# The stamp is redundant here and named anyway. Every object already carries
# it, and the link takes $^, so a flag change reaches this rule transitively --
# measured: a change to LDFLAGS alone rebuilt all 28 objects, relinked, and put
# BIND_NOW in the artifact. But "covered by something else" is the state that
# stops being true quietly, and an audit that has to special-case three rules
# grows an allowlist, which is how the audit rots.
#
# The three link recipes name their object list instead of $^ because of this.
# $^ is every prerequisite, so the first attempt handed .flags to the linker:
# "file format not recognized; treating as linker script". Adding a stamp to a
# rule whose recipe uses $^ or $+ breaks it, and check-stamps cannot see that
# -- it reads prerequisite lists, not recipes, and stayed green while the link
# was broken. `make test` is what caught it.
$(APP_DIR)/$(TARGET): \
		$(LIBOBJECTS) \
		$(FLAGS_STAMP)
	@printf "\n### Compiling Ghoti.io CUtil Shared Library ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OS_SPECIFIC_LINK_FLAGS) -o $@ $(LIBOBJECTS) $(LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Unit Tests
####################################################################

# Test executables: compile with -MMD -MP -MF so dependency files are generated and -included.
# The plugin the dynamic-loading tests load.  Built as its own shared object
# with default visibility -- not linked into anything -- so that those tests
# open a real library with known symbols instead of reopening cutil, which is
# already in the process and would let a broken loader look like it worked.
#
# One per tree.  The ASan tree gets its own rather than borrowing the ordinary
# one, so the object the loader opens was built with the same flags as the
# process opening it.
$(APP_DIR)/libtest-plugin.$(LIB_EXTENSION): test/test-plugin.c $(FLAGS_STAMP)
	@printf "\n### Compiling Test Plugin ###\n"
	@mkdir -p $(@D)
	$(CC) $(filter-out -fvisibility=hidden,$(CFLAGS)) -shared -fPIC -o $@ $<

ifeq ($(OS_NAME), Windows)
# A PE image cannot leave a symbol for the loader to find by name alone: every
# import names the DLL it comes from, so the link needs an import library. This
# one is made from a .def file and describes a DLL that is never built, which
# gives LoadLibrary the same thing RTLD_NOW refuses on ELF - an import nothing
# can satisfy - and Windows resolves imports at load time unconditionally.
$(APP_DIR)/libtest-plugin-broken.$(LIB_EXTENSION): test/test-plugin-broken.c $(FLAGS_STAMP)
	@printf "\n### Compiling Test Plugin (unresolvable) ###\n"
	@mkdir -p $(@D)
	printf 'LIBRARY gcu-test-plugin-absent.dll\nEXPORTS\n  gcu_test_plugin_no_such_function\n' > $(@D)/test-plugin-absent.def
	dlltool -d $(@D)/test-plugin-absent.def -l $(@D)/libtest-plugin-absent.dll.a
	$(CC) $(filter-out -fvisibility=hidden,$(CFLAGS)) -shared -o $@ $< $(@D)/libtest-plugin-absent.dll.a
else
$(APP_DIR)/libtest-plugin-broken.$(LIB_EXTENSION): test/test-plugin-broken.c $(FLAGS_STAMP)
	@printf "\n### Compiling Test Plugin (unresolvable) ###\n"
	@mkdir -p $(@D)
	$(CC) $(filter-out -fvisibility=hidden,$(CFLAGS)) -shared -fPIC -o $@ $<
endif

# Extra flags for one test, looked up by name.  Spelled this way because the
# ASan and TSan binaries are built from a generated rule that cannot carry a
# per-test flag any other way -- and a test needing a define in one tree and
# not the others is how test-library first built clean and then failed only
# under ASan.  Adding a test here covers every tree at once.
TEST_CPPFLAGS_test-library = \
	-DGCU_TEST_PLUGIN_PATH='"$(1)/libtest-plugin.$(LIB_EXTENSION)"' \
	-DGCU_TEST_BROKEN_PLUGIN_PATH='"$(1)/libtest-plugin-broken.$(LIB_EXTENSION)"'
TEST_PREREQS_test-library  = $(1)/libtest-plugin.$(LIB_EXTENSION) \
	$(1)/libtest-plugin-broken.$(LIB_EXTENSION)

# The lock tests create files; they go in the build tree, not the source tree.
TEST_CPPFLAGS_test-filelock = -DGCU_TEST_LOCK_DIR='"$(1)"'
TEST_CPPFLAGS_test-mmap     = -DGCU_TEST_MMAP_DIR='"$(1)"'
TEST_CPPFLAGS_test-subprocess = -DGCU_TEST_SUBPROCESS_DIR='"$(1)"'

$(APP_DIR)/test-library$(EXE_EXTENSION): test/test-library.cpp $(FLAGS_STAMP) \
		$(call TEST_PREREQS_test-library,$(APP_DIR)) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Library Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) $(call TEST_CPPFLAGS_test-library,$(APP_DIR)) \
		-MMD -MP -MF $(APP_DIR)/test-library.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-mmap$(EXE_EXTENSION): test/test-mmap.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Mmap Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) $(call TEST_CPPFLAGS_test-mmap,$(APP_DIR)) \
		-MMD -MP -MF $(APP_DIR)/test-mmap.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-subprocess$(EXE_EXTENSION): test/test-subprocess.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Subprocess Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) $(call TEST_CPPFLAGS_test-subprocess,$(APP_DIR)) \
		-MMD -MP -MF $(APP_DIR)/test-subprocess.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-barrier$(EXE_EXTENSION): test/test-barrier.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Barrier Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-barrier.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-atomic$(EXE_EXTENSION): test/test-atomic.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Atomic Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-atomic.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-filelock$(EXE_EXTENSION): test/test-filelock.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling File Lock Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) $(call TEST_CPPFLAGS_test-filelock,$(APP_DIR)) \
		-MMD -MP -MF $(APP_DIR)/test-filelock.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-env$(EXE_EXTENSION): test/test-env.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Env Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-env.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-tls$(EXE_EXTENSION): test/test-tls.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling TLS Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-tls.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-utf$(EXE_EXTENSION): test/test-utf.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling UTF Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-utf.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-error$(EXE_EXTENSION): test/test-error.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Error Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-error.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-rwlock$(EXE_EXTENSION): test/test-rwlock.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling RWLock Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-rwlock.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-once$(EXE_EXTENSION): test/test-once.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Once Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-once.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-cond$(EXE_EXTENSION): test/test-cond.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Condition Variable Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-cond.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-macros$(EXE_EXTENSION): test/test-macros.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Macros Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-macros.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-type$(EXE_EXTENSION): test/test-type.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Types Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-type.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-memory$(EXE_EXTENSION): test/test-memory.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Memory Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-memory.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-memory-inline$(EXE_EXTENSION): test/test-memory-inline.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Inline Memory Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-memory-inline.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-hash$(EXE_EXTENSION): test/test-hash.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Hash Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-hash.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-mutex$(EXE_EXTENSION): test/test-mutex.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Mutex Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-mutex.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-pool$(EXE_EXTENSION): test/test-pool.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Pool Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-pool.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-sequencer$(EXE_EXTENSION): test/test-sequencer.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Sequencer Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-sequencer.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-fiber$(EXE_EXTENSION): test/test-fiber.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Fiber Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-fiber.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-socket$(EXE_EXTENSION): test/test-socket.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Socket Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-socket.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-loop$(EXE_EXTENSION): test/test-loop.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Loop Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-loop.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-file$(EXE_EXTENSION): test/test-file.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling File Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-file.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-dir$(EXE_EXTENSION): test/test-dir.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Directory Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-dir.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-path$(EXE_EXTENSION): test/test-path.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Path Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-path.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-random$(EXE_EXTENSION): test/test-random.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Random Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-random.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-semaphore$(EXE_EXTENSION): test/test-semaphore.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Semaphore Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-semaphore.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-string$(EXE_EXTENSION): test/test-string.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling String Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-string.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-thread$(EXE_EXTENSION): test/test-thread.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Thread Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-thread.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-vector$(EXE_EXTENSION): test/test-vector.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Vector Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-vector.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-array$(EXE_EXTENSION): test/test-array.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Array Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-array.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-allocator$(EXE_EXTENSION): test/test-allocator.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Allocator Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-allocator.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

$(APP_DIR)/test-safemath$(EXE_EXTENSION): test/test-safemath.cpp $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Safe Math Test ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/test-safemath.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

# The same cases against the portable body of each operation. It includes
# test-safemath.cpp, so -I test/ is needed to find it, and it has to be
# rebuilt when that file changes.
$(APP_DIR)/test-safemath-portable$(EXE_EXTENSION): test/test-safemath-portable.cpp $(FLAGS_STAMP) \
		test/test-safemath.cpp | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Safe Math Test (portable body) ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -I test/ -MMD -MP -MF $(APP_DIR)/test-safemath-portable.d -o $@ $< $(LDFLAGS) $(TESTFLAGS) $(CUTILLIBRARY)

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf coverage check-symbols check-win32-parse check-clang test-tsan
.PHONY: check-rebuild check-stamps
# Release build commands
.PHONY: all install test test-asan test-ubsan test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-watch-debug uninstall-debug watch-debug

watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make all; \
		printf "\033[0;32m"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m"; \
		inotifywait -qr -e modify -e create -e delete -e move src include test Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make test; \
		printf "\033[0;32m"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m"; \
		inotifywait -qr -e modify -e create -e delete -e move src include test Makefile --exclude '/\.'; \
		done

####################################################################
# Symbol namespace check
####################################################################

check-symbols: ## Fail if any exported symbol lacks the version namespace
check-symbols: $(APP_DIR)/$(TARGET)
ifeq ($(OS_NAME), Linux)
# mangle_path is gcov's, not ours: a --coverage build links it into the library
# and it is the only symbol libgcov exports whose name does not begin with an
# underscore, so it is the only one the '^_' filter above misses. Without this
# line `make coverage` fails here - after the instrumented build and before the
# clean that would undo it - leaving instrumented objects that a later plain
# `make` silently links.
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' \
		| grep -v '^_' \
		| grep -v '^mangle_path$$' || true); \
	if [ -n "$$leaked" ]; then \
		printf "\033[0;31m\n### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\033[0m\n" >&2; \
		printf "%s\n" "$$leaked" >&2; \
		printf "\nEach needs a '#define <name> GHOTIIO_CUTIL(<name>)' line in the header\n" >&2; \
		printf "that declares it. Without one, two versions of this library cannot be\n" >&2; \
		printf "loaded into the same process. See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@unexported=$$(awk '/^#if DOXYGEN/{d=1} d==0 && /^[a-z_][A-Za-z0-9_ ]*\**[[:space:]]*gcu_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$$0} /^#endif/{d=0}' \
		include/$(SUITE)/$(PROJECT)/*.h | grep -vE 'typedef|static inline' || true); \
	if [ -n "$$unexported" ]; then \
		printf "\033[0;31m\n### Public declarations without GCU_API ###\033[0m\n" >&2; \
		printf "%s\n" "$$unexported" >&2; \
		printf "\nThe library builds with -fvisibility=hidden, so these are not exported\n" >&2; \
		printf "and a consumer linking the .so gets an undefined reference.\n" >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf "\033[0;31m\n### Renamed but undefined - a split symbol ###\033[0m\n" >&2; \
		printf "%s\n" "$$split" >&2; \
		printf "\nA translation unit referenced the namespaced name while the one that\n" >&2; \
		printf "defines it did not see the rename - usually an internal header that\n" >&2; \
		printf "declares or defines something without including macros.h first.\n" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/cutil/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf "\033[0;31m\n### Headers that do not include macros.h ###\033[0m\n" >&2; \
		printf "%s\n" "$$nomacros" >&2; \
		printf "\nEvery header must include <ghoti.io/cutil/macros.h> before it declares\n" >&2; \
		printf "anything, so that the renames in namespace.h are already in effect. A\n" >&2; \
		printf "header that skips it can name a type before that type has been renamed,\n" >&2; \
		printf "producing two different types under one spelling.\n" >&2; \
		printf "See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GCU_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf "\033[0;31m\n### Include guards with the wrong prefix ###\033[0m\n" >&2; \
		printf "%s\n" "$$badguards" >&2; \
		printf "\nGuards mirror the path: GHOTI_IO_GCU_<PATH>_H. A guard without the\n" >&2; \
		printf "library token is one rename away from colliding with another library's.\n" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf "\033[0;31m\n### Headers sharing an include guard ###\033[0m\n" >&2; \
		printf "%s\n" "$$dupguards" >&2; \
		printf "\nTwo headers with one guard means whichever is included second is\n" >&2; \
		printf "silently empty. Guards mirror the path: GHOTI_IO_GCU_<PATH>_H.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery exported symbol carries the $(LIBVER_SYMBOL)_ namespace.\033[0m\n"
	@printf "\033[0;32mEvery public declaration carries GCU_API.\033[0m\n"
	@printf "\033[0;32mEvery header includes macros.h.\033[0m\n"
	@printf "\033[0;32mEvery include guard is unique and correctly prefixed.\033[0m\n"
else
	@printf "check-symbols: skipped (Linux only)\n"
endif

# Parse the headers' `#ifdef _WIN32` branches on this compiler.
#
# Those branches are never tokenised by a Linux build, so a syntax error in
# one survives indefinitely -- GCU_MAYBE_UNUSED's Windows arm was a syntax
# error from the initial commit until it was written down. This compiles a TU
# that *uses* every Windows-only macro against stub declarations; it catches
# the syntax-error class and says nothing about semantics.
# Compile everything with clang as well as gcc.
#
# Not about supporting a second compiler for its own sake. clang's UBSan
# diagnoses things gcc's does not -- `&data[0]` on a null pointer is undefined
# and only clang reports it, with no gcc flag that turns it on
# (-fsanitize=undefined and -fsanitize=pointer-overflow were both measured
# silent) -- and libFuzzer needs clang too. A library that has quietly stopped
# compiling under it cannot be instrumented at all without local patches,
# which is exactly how two such defects sat in the hash template while every
# run of this suite executed the line.
#
# -fsyntax-only rather than a real compile: it is half a second, and it still
# sees the driver-level diagnostics, including a link flag that has found its
# way onto a compile line.
check-clang: | $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
check-clang: ## Check that the library still compiles under clang
	@printf "\n### Compiling under clang ###\n"
ifeq ($(CLANG),)
	@printf "check-clang: skipped (clang is not installed)\n"
else
	$(CLANG) -fsyntax-only $(CFLAGS) $(INCLUDE) $(OS_SPECIFIC_COMPILE_FLAGS) \
		$(patsubst $(OBJ_DIR)/%.o,src/%.c,$(LIBOBJECTS))
endif

# A build that recompiles when nothing changed is not a cosmetic annoyance. It
# is the same failure as a build that *doesn't* recompile when something did,
# seen from the other side: in both cases the tree's mtimes have stopped
# meaning what the rules say they mean, and you cannot tell from outside which
# of the two you have. It also hides itself, because the visible symptom is
# only "the build is a bit slow".
#
# The gate perturbs the tree the way the build itself does before it looks.
# Simply re-running make here does not work and was tried: by the time the
# gate runs, this invocation has already absorbed whatever moved, so the tree
# is settled and the check passes against its own bug. What it has to do is
# reproduce the event -- the libver_gen.h rule writes and removes $@.tmp in
# the generated-include directory on every build, which moves that directory's
# mtime, and any rule naming the directory as a normal prerequisite is then
# out of date for a reason that is not in the dependency graph. Creating and
# removing a scratch file there is the same event.
#
# The sub-make runs on its own recipe line so that the leading `+` reaches
# make and shares the jobserver. Written inside a $$(...) the `+` goes to the
# shell instead, which has no such command -- the sub-make then never runs at
# all, the captured output is an error message, and the gate passes against
# every bug there is. That is how the first version of this rule behaved.
GEN_INCLUDE_DIR := $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)
check-rebuild: $(APP_DIR)/$(TARGET) $(TEST_BINARIES)
	@printf "\n### Checking that a settled tree rebuilds nothing ###\n"
	@: > $(GEN_INCLUDE_DIR)/.rebuild-probe; rm -f $(GEN_INCLUDE_DIR)/.rebuild-probe
	+@$(MAKE) --no-print-directory $(APP_DIR)/$(TARGET) $(TEST_BINARIES) \
		> $(BUILD_DIR)/.rebuild-check 2>&1 || true
	@again=$$(grep -c '### Compiling ' $(BUILD_DIR)/.rebuild-check || true); \
	if [ "$$again" -ne 0 ]; then \
		printf "### %s targets recompiled with nothing changed ###\n" "$$again" >&2; \
		grep '### Compiling ' $(BUILD_DIR)/.rebuild-check >&2; \
		printf "A prerequisite moved on its own. The usual cause is a rule whose\n" >&2; \
		printf "prerequisite list names a directory: a directory's mtime changes\n" >&2; \
		printf "whenever any file is created or removed inside it, so it is never\n" >&2; \
		printf "stable. Make it order-only, with a | in front of it.\n" >&2; \
		exit 1; \
	fi

# Every rule that compiles or links has to name the flag stamp for the tree its
# output lands in. A rule that names none builds with whatever flags are in
# force and is then never rebuilt when they change, which is indistinguishable
# from a correct incremental build; a rule copied between trees that keeps the
# source tree's stamp is worse, because it misses exactly the flag changes it
# was put there to catch.
#
# The pattern rules are never the problem -- they get edited as a group. It is
# the hand-written one-off rules, written once and then left alone. cutil's
# float_identifier was missed that way when the stamp replaced the Makefile
# prerequisite, and it is the rule whose output *is* a generated header.
#
# This reads the makefile's TEXT rather than `make -p`, and that is the whole
# point: a rule inside an ifeq whose condition is false is not in make's rule
# database at all, so a database audit reports it as covered. cutil's ASan and
# TSan rules all live inside `ifeq ($(OS_NAME), Linux)`.
check-stamps: ## Check that every compile rule names its tree's flag stamp
	@printf "\n### Checking flag stamps on compile rules ###\n"
	@awk -f test/stamp-audit.awk Makefile \
	|| { printf "### A compile rule is not guarded by its flag stamp ###\n" >&2; \
	     printf "Such a rule builds with whatever flags are in force and is then\n" >&2; \
	     printf "never rebuilt when they change. See test/stamp-audit.awk for what\n" >&2; \
	     printf "each arm means and how to fix it.\n" >&2; \
	     exit 1; }

check-win32-parse: ## Parse-check the headers' Windows branches
	@printf "\n### Parse-checking Windows branches ###\n"
ifeq ($(OS_NAME), Windows)
	@printf "check-win32-parse: skipped (this is Windows; the real build compiled those branches)\n"
else
	$(CC) -fsyntax-only $(filter-out -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD,$(CFLAGS)) \
		-include test/win32-stubs/force.h -I test/win32-stubs $(INCLUDE) \
		test/win32-stubs/parse-check.c
	@printf "### Parse-checking Windows sources ###\n"
	$(CC) -fsyntax-only -D_WIN32 $(filter-out -fvisibility=hidden,$(CFLAGS)) \
		-include test/win32-stubs/force.h -I test/win32-stubs $(INCLUDE) \
		$(WIN32_PARSE_SOURCES)
endif

check-win32-linkage: ## Check GCU_API's three Windows states
	@printf "\n### Checking the Windows linkage states ###\n"
	@# Three compilations of one file, one per state, because a translation
	@# unit can only be in one of them.  Built into the scratch area rather
	@# than $(OBJ_DIR) so that nothing here can be linked into the library by
	@# mistake, and with -DGHOTIIO_CUTIL_BUILD filtered out of CFLAGS so that
	@# the state is set only by the -D each line adds.
	@# Three preprocessings of one file, one per state, because a translation
	@# unit can only be in one of them.  -fsyntax-only throughout: the check is
	@# entirely in `#if`, so it emits nothing that could go stale against a
	@# flag change -- which is also why the stamp audit does not ask this rule
	@# for a flag stamp.  -DGHOTIIO_CUTIL_BUILD is filtered out of CFLAGS so
	@# that the state is set only by the -D each pass adds.
	@for state in GHOTIIO_CUTIL_BUILD GHOTIIO_CUTIL_STATIC CONSUMER_OF_THE_DLL; do \
		printf "  %s\n" "$$state"; \
		$(CC) -fsyntax-only \
			$(filter-out -fvisibility=hidden -DGHOTIIO_CUTIL_BUILD,$(CFLAGS)) \
			-D$$state -I test/win32-stubs $(INCLUDE) \
			test/win32-stubs/api-linkage-check.c \
		|| exit 1; \
	done

test: ## Make and run the Unit tests
# Both the prerequisites and the run lines are derived from TEST_NAMES, so
# adding a test to that list is all it takes to have it built and executed.
# They used to be hand-maintained in parallel, which made it possible to add a
# test that was compiled but never run.
test: $(APP_DIR)/$(TARGET) $(TEST_BINARIES) $(TEST_GATES)
	@printf "\033[0;32m"
	@printf "############################\n"
	@printf "### Running normal tests ###\n"
	@printf "############################\n"
	@printf "\033[0m"
	@for t in $(TEST_BINARIES); do \
		printf "\n--- $$t ---\n"; \
		env LD_LIBRARY_PATH="$(APP_DIR)" $$t --gtest_brief=1 || exit 1; \
	done

####################################################################
# Sanitizer builds (ASan + UBSan)
####################################################################
#
# This library had no sanitizer target at all, which meant its memory
# behaviour was only ever asserted by tests that pass or fail on values -
# and a use-after-free reads back plausible values. The sibling compress
# library, which did have one, turned out to have three of them the moment
# the target was made to run.
#
# The instrumented objects live in their own tree so that an ordinary `make`
# can never link them by mistake.

# One list, used twice.  Two things were wrong here, and the second is the one
# that made the first invisible:
#
#   1. -fno-sanitize-recover was absent entirely, so UBSan printed its
#      diagnostic and returned 0.  The target reported the bug and passed.
#      Measured: a signed-overflow and a float-cast-overflow in one program
#      both printed and the process exited 0.
#   2. gcc's `undefined` group does not include float-cast-overflow (clang's
#      does), so `(int)1e30` was not even diagnosed.  Naming the check in
#      -fsanitize= and forgetting it in -fno-sanitize-recover= reproduces
#      failure 1 for that one check, which is why both flags read one variable
#      rather than two lists that can drift apart.
#
# Deliberately NOT here: float-divide-by-zero, which IEEE defines and which
# would fire on correct code that records an inf.  bounds-strict and
# pointer-overflow are already in gcc's `undefined` and add nothing.
UBSAN_CHECKS := undefined,float-cast-overflow

ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) \
	-fno-sanitize-recover=$(UBSAN_CHECKS) \
	-fno-omit-frame-pointer -g $(SAN_OPT_CFLAGS)

ASAN_BUILD_DIR := $(BUILD_DIR)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.so

ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)

# The flag set this tree's objects were built with, rewritten only when it
# changes so its mtime moves on a flag change and on nothing else. The object
# rules below depend on it.
#
# `Makefile` used to serve this purpose and was wrong in both directions: too
# broad, because a comment-only edit recompiled everything, and too narrow,
# because a command-line override such as `make EXTRA_CFLAGS=-O2` changes no
# file's mtime and so was invisible. The flag string sees both.
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_LIBOBJECTS := $(patsubst $(OBJ_DIR)/%,$(ASAN_OBJ_DIR)/%,$(LIBOBJECTS))
ASAN_TEST_BINARIES := \
	$(foreach t,$(TEST_NAMES),$(ASAN_APP_DIR)/$(t)$(EXE_EXTENSION))
ASAN_CUTILLIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

# The ASan runtime insists on being initialised before anything it has to
# intercept, and refuses to start rather than run uninstrumented if some other
# library got there first:
#
#   ASan runtime does not come first in initial library list
#
# A desktop session that sets LD_PRELOAD for its own reasons is enough to
# trigger that, and every test then aborts before gtest gets control. Naming
# the runtime here replaces whatever was inherited and puts it first.
#
# Only under gcc, though. clang links its own runtime into the executable, and
# preloading gcc's on top of that gets "Your application is linked against
# incompatible ASan runtimes" on the first binary -- which is what
# `make test-asan CC=clang` used to do. An empty preload is right there: it
# still replaces whatever the desktop inherited, which is the entire purpose
# of setting it, and clang's runtime is already first in the executable's own
# NEEDED list.
#
# The compiler has to be identified by asking it, not by looking at the answer
# to -print-file-name: clang on this distribution resolves libasan.so through
# the gcc installation and hands back gcc's absolute path, so the two replies
# are byte-identical and a path test cannot tell them apart.
ASAN_CC_IS_CLANG := $(shell $(CC) -dM -E -x c /dev/null 2>/dev/null | grep -c __clang__)
ifeq ($(ASAN_CC_IS_CLANG),0)
ASAN_RUNTIME := $(shell $(CC) -print-file-name=libasan.so)
else
ASAN_RUNTIME :=
endif

# float.h and libver_gen.h are plain generated headers, so the instrumented
# build reuses the ones the ordinary build made rather than building a second
# copy of the generator. That also keeps the generator itself uninstrumented:
# it is a build tool that runs at build time, and instrumenting it only made
# it inherit the startup problem described above.
$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling (ASan+UBSan): $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -o $@ $(OS_SPECIFIC_COMPILE_FLAGS)

$(ASAN_OBJ_DIR)/hash.o: src/hash.template.c
$(ASAN_OBJ_DIR)/vector.o: src/vector.template.c

$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS) $(ASAN_FLAGS_STAMP)
	@printf "\n### Linking (ASan+UBSan) $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(OS_SPECIFIC_LINK_FLAGS) -o $@ $(ASAN_LIBOBJECTS) $(ASAN_LDFLAGS)

# One rule per test, generated from TEST_NAMES for the same reason the ordinary
# test list is: a hand-maintained second list is a list that can silently omit
# a test.
$(ASAN_APP_DIR)/libtest-plugin.$(LIB_EXTENSION): test/test-plugin.c $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling (ASan+UBSan) Test Plugin ###\n"
	@mkdir -p $(@D)
	$(CC) $(filter-out -fvisibility=hidden,$(ASAN_CFLAGS)) -shared -fPIC -o $@ $<

$(ASAN_APP_DIR)/libtest-plugin-broken.$(LIB_EXTENSION): test/test-plugin-broken.c $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling (ASan+UBSan) Test Plugin (unresolvable) ###\n"
	@mkdir -p $(@D)
	$(CC) $(filter-out -fvisibility=hidden,$(ASAN_CFLAGS)) -shared -fPIC -o $@ $<

define ASAN_TEST_RULE
$(ASAN_APP_DIR)/$(1)$(EXE_EXTENSION): test/$(1).cpp $(ASAN_FLAGS_STAMP) \
		$(call TEST_PREREQS_$(1),$(ASAN_APP_DIR)) \
		| $(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Compiling (ASan+UBSan) $$@ ###\n"
	@mkdir -p $$(@D)
	$$(CXX) $$(ASAN_CXXFLAGS) $$(INCLUDE) -I test/ \
		$(call TEST_CPPFLAGS_$(1),$(ASAN_APP_DIR)) \
		-o $$@ $$< $$(ASAN_LDFLAGS) $$(TESTFLAGS) $$(ASAN_CUTILLIBRARY)
endef
$(foreach t,$(TEST_NAMES),$(eval $(call ASAN_TEST_RULE,$(t))))

test-asan: ## Make and run the Unit tests under AddressSanitizer + UBSan
test-asan: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_BINARIES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m"
	@printf "#######################################\n"
	@printf "### Running tests with ASan + UBSan ###\n"
	@printf "#######################################\n"
	@printf "\033[0m"
	@for t in $(ASAN_TEST_BINARIES); do \
		printf "\n--- $$t ---\n"; \
		env LD_LIBRARY_PATH="$(ASAN_APP_DIR)" LD_PRELOAD="$(ASAN_RUNTIME)" \
			ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
			UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
			$$t --gtest_brief=1 || exit 1; \
	done
# The fibers again with use-after-return detection off.  In the default mode
# a frame's locals live on a heap-backed fake stack, so the real fiber stack
# is never poisoned and the switch annotations are exercised through the fake
# stack only.  With it off, the poison sits on the real stack, which is where
# a switch that did not announce the new stack leaves it behind.  Each mode
# reaches code the other cannot.
	@printf "\n--- $(ASAN_APP_DIR)/test-fiber (detect_stack_use_after_return=0) ---\n"
	@env LD_LIBRARY_PATH="$(ASAN_APP_DIR)" LD_PRELOAD="$(ASAN_RUNTIME)" \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=0 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		$(ASAN_APP_DIR)/test-fiber --gtest_brief=1
	@printf "\033[0;32m\nAll tests passed with ASan + UBSan.\033[0m\n"
else
	@printf "\033[0;31mSanitizer builds are currently only supported on Linux.\033[0m\n"
	@exit 1
endif

test-ubsan: ## Alias for test-asan (ASan and UBSan run together)
test-ubsan: test-asan

# ---------------------------------------------------------------------------
# ThreadSanitizer
#
# ASan and UBSan assert what a single thread does with memory. They say
# nothing about two threads reaching the same memory without a lock between
# them, which is the defect this library's concurrent modules are most likely
# to have: the sibling compress library's thread pool shared its shutdown flag
# across threads as a plain bool, and nothing in its test suite could see
# that. (That pool has since been replaced by this one, and its job queue by
# this library's sequencer, which is why both are on the list below.)
#
# TSan cannot be combined with ASan, so it gets its own tree, built the same
# way and kept beside the ordinary and instrumented ones.
#
# It is driven by its own list rather than by TEST_NAMES. hash and vector ship
# a mutex that the README describes as the caller's responsibility to use, so
# their tests may race deliberately, and auditing them should not gate a run
# that is here to watch the synchronisation primitives themselves. Add a name
# here once its test is expected to be clean under TSan.

TSAN_TEST_NAMES := test-mutex test-cond test-barrier test-once test-rwlock test-tls test-atomic test-semaphore test-thread test-pool test-sequencer test-fiber test-socket test-loop

TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g $(SAN_OPT_CFLAGS)

TSAN_BUILD_DIR := $(BUILD_DIR)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps
TSAN_TARGET := $(BASE_NAME_PREFIX)-tsan.so

TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS)
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)

# The flag set this tree's objects were built with, rewritten only when it
# changes so its mtime moves on a flag change and on nothing else. The object
# rules below depend on it.
#
# `Makefile` used to serve this purpose and was wrong in both directions: too
# broad, because a comment-only edit recompiled everything, and too narrow,
# because a command-line override such as `make EXTRA_CFLAGS=-O2` changes no
# file's mtime and so was invisible. The flag string sees both.
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_LIBOBJECTS := $(patsubst $(OBJ_DIR)/%,$(TSAN_OBJ_DIR)/%,$(LIBOBJECTS))
TSAN_TEST_BINARIES := \
	$(foreach t,$(TSAN_TEST_NAMES),$(TSAN_APP_DIR)/$(t)$(EXE_EXTENSION))
TSAN_CUTILLIBRARY := -L $(TSAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-tsan

# Deliberately NOT preloaded, unlike the ASan runtime.
#
# The two sanitizers look alike here but are not. ASan checks its own
# initialisation order and refuses to start if anything got in front of it, so
# an LD_PRELOAD inherited from a desktop session has to be replaced; that is
# what the ASAN_RUNTIME comment describes. TSan makes no such check. Being
# first in the executable's own NEEDED list is enough, and it stays first
# whatever the environment preloads - verified by planting a race and watching
# TSan still report it with an unrelated .so preloaded.
#
# Preloading it is therefore not merely redundant, it is harmful: the shell
# that system() spawns inherits the preload into an uninstrumented binary and
# dies of SIGSEGV with no diagnostic at all, so system() returns a raw wait
# status of 11 whatever it was asked to run. Nothing in cutil's suite execs
# today - test/test-thread.cpp forks and _exit()s without one - so this is a
# trap laid for the first test that shells out rather than a present failure.
# compress hit it for real: 135 failures, none of them races (compress e035f44).
#
# LD_PRELOAD is cleared rather than left unset so that a value inherited from
# the environment cannot reintroduce the problem.

$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling (TSan): $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -o $@ $(OS_SPECIFIC_COMPILE_FLAGS)

$(TSAN_OBJ_DIR)/hash.o: src/hash.template.c
$(TSAN_OBJ_DIR)/vector.o: src/vector.template.c

$(TSAN_APP_DIR)/$(TSAN_TARGET): $(TSAN_LIBOBJECTS) $(TSAN_FLAGS_STAMP)
	@printf "\n### Linking (TSan) $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(OS_SPECIFIC_LINK_FLAGS) -o $@ $(TSAN_LIBOBJECTS) $(TSAN_LDFLAGS)

define TSAN_TEST_RULE
$(TSAN_APP_DIR)/$(1)$(EXE_EXTENSION): test/$(1).cpp $(TSAN_FLAGS_STAMP) \
		| $(TSAN_APP_DIR)/$(TSAN_TARGET)
	@printf "\n### Compiling (TSan) $$@ ###\n"
	@mkdir -p $$(@D)
	$$(CXX) $$(TSAN_CXXFLAGS) $$(INCLUDE) -I test/ -o $$@ $$< $$(TSAN_LDFLAGS) \
		$$(TESTFLAGS) $$(TSAN_CUTILLIBRARY)
endef
$(foreach t,$(TSAN_TEST_NAMES),$(eval $(call TSAN_TEST_RULE,$(t))))

test-tsan: ## Make and run the concurrency tests under ThreadSanitizer
test-tsan: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_TEST_BINARIES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m"
	@printf "####################################\n"
	@printf "### Running tests with TSan      ###\n"
	@printf "####################################\n"
	@printf "\033[0m"
	@for t in $(TSAN_TEST_BINARIES); do \
		printf "\n--- $$t ---\n"; \
		env LD_LIBRARY_PATH="$(TSAN_APP_DIR)" LD_PRELOAD= \
			TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
			$$t --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\nAll tests passed with TSan.\033[0m\n"
else
	@printf "\033[0;31mSanitizer builds are currently only supported on Linux.\033[0m\n"
	@exit 1
endif


# ---------------------------------------------------------------------------
# The fiber gate, and the defects it plants
#
# The fiber tests are only worth running if they are known to fail when the
# switch is wrong, and the sanitizers only see through a fiber library that
# tells them about each switch.  So this builds the library several more
# times, each with one deliberate defect compiled into src/fiber.c (the
# GCU_FIBER_PLANT_* macros, which no ordinary build defines), runs the fiber
# tests against it, and requires them to report:
#
#   plain tree     the switch skips the MXCSR control bits (x86-64), or the
#                  x87 control word (x86-64), or FPCR (arm64): the rounding
#                  test must fail.
#   ASan tree      the switch does not tell AddressSanitizer about the new
#                  stack: it must report.  Run with
#                  detect_stack_use_after_return=0, because in the default
#                  mode locals live on a heap-backed fake stack and the real
#                  stack never holds the stale poison the defect leaves
#                  behind; there the unannotated switch only prints a warning,
#                  which the second arm requires.
#   TSan tree      the switch does not tell ThreadSanitizer about the new
#                  fiber: the test that asks which TSan context is running
#                  must fail.
#
# Each arm also runs the real library first and requires it to be quiet, so a
# gate that fails for some other reason cannot pass as having caught the
# defect.  test/check-fiber-defects.sh is the arm; this is the building.
#
# The rules are one per tree and spell the tree's own stamp, because
# test/stamp-audit.awk reads the target's name to decide which stamp a rule
# owes.  A planted library lives in a directory of its own beside the real one
# and is found by the test binary through LD_LIBRARY_PATH, which beats the
# RUNPATH the binary carries.
# ---------------------------------------------------------------------------

# What the library is being built FOR, not what is running make: a cross build
# with CC=aarch64-linux-gnu-gcc on an x86-64 host needs the arm64 defects (and
# cannot run them, which is a different matter -- see below).  Each entry is
# name:MACRO:substring of the test that must fail.
FIBER_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq ($(findstring aarch64,$(FIBER_TARGET)),)
FIBER_GATE_SUPPORTED := yes
FIBER_FP_PLANTS := no-fpcr:NO_FPCR:EachFiberKeepsItsOwnRoundingMode \
	no-callee-saved:NO_CALLEE_SAVED:CalleeSavedRegistersSurvive
else ifneq ($(findstring x86_64,$(FIBER_TARGET)),)
FIBER_GATE_SUPPORTED := yes
FIBER_FP_PLANTS := no-mxcsr:NO_MXCSR:EachFiberKeepsItsOwnRoundingMode \
	no-x87cw:NO_X87CW:EachFiberKeepsItsOwnRoundingMode \
	no-callee-saved:NO_CALLEE_SAVED:CalleeSavedRegistersSurvive
else
# i686, s390x, powerpc, sparc: no switch routine, the tests skip, and a gate
# that runs skipped tests proves nothing.  The gate says so and stops.
FIBER_GATE_SUPPORTED := no
FIBER_FP_PLANTS :=
endif

define FIBER_PLANT_RULE
$(APP_DIR)/plant-fiber-$(1)/fiber.o: src/fiber.c $(FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling fiber.c with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -DGCU_FIBER_PLANT_$(2) $$(INCLUDE) -c $$< -o $$@ $$(OS_SPECIFIC_COMPILE_FLAGS)

$(APP_DIR)/plant-fiber-$(1)/$(TARGET): $(APP_DIR)/plant-fiber-$(1)/fiber.o \
		$(filter-out $(OBJ_DIR)/fiber.o,$(LIBOBJECTS)) $(FLAGS_STAMP)
	@printf "\n### Linking a library with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(OS_SPECIFIC_LINK_FLAGS) -o $$@ $(APP_DIR)/plant-fiber-$(1)/fiber.o $(filter-out $(OBJ_DIR)/fiber.o,$(LIBOBJECTS)) $$(LDFLAGS) $$(OS_SPECIFIC_LIBRARY_NAME_FLAG)
	@ln -f -s $(TARGET) $(APP_DIR)/plant-fiber-$(1)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/plant-fiber-$(1)/$(BASE_NAME)
endef
$(foreach p,$(FIBER_FP_PLANTS),$(eval $(call FIBER_PLANT_RULE,$(word 1,$(subst :, ,$(p))),$(word 2,$(subst :, ,$(p))))))

define ASAN_FIBER_PLANT_RULE
$(ASAN_APP_DIR)/plant-fiber-$(1)/fiber.o: src/fiber.c $(ASAN_FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling (ASan+UBSan) fiber.c with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(ASAN_CFLAGS) -DGCU_FIBER_PLANT_$(2) $$(INCLUDE) -c $$< -o $$@ $$(OS_SPECIFIC_COMPILE_FLAGS)

$(ASAN_APP_DIR)/plant-fiber-$(1)/$(ASAN_TARGET): $(ASAN_APP_DIR)/plant-fiber-$(1)/fiber.o \
		$(filter-out $(ASAN_OBJ_DIR)/fiber.o,$(ASAN_LIBOBJECTS)) $(ASAN_FLAGS_STAMP)
	@printf "\n### Linking (ASan+UBSan) a library with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(ASAN_CFLAGS) $$(OS_SPECIFIC_LINK_FLAGS) -o $$@ $(ASAN_APP_DIR)/plant-fiber-$(1)/fiber.o $(filter-out $(ASAN_OBJ_DIR)/fiber.o,$(ASAN_LIBOBJECTS)) $$(ASAN_LDFLAGS)
endef
$(eval $(call ASAN_FIBER_PLANT_RULE,no-asan,NO_ASAN))

define TSAN_FIBER_PLANT_RULE
$(TSAN_APP_DIR)/plant-fiber-$(1)/fiber.o: src/fiber.c $(TSAN_FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling (TSan) fiber.c with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(TSAN_CFLAGS) -DGCU_FIBER_PLANT_$(2) $$(INCLUDE) -c $$< -o $$@ $$(OS_SPECIFIC_COMPILE_FLAGS)

$(TSAN_APP_DIR)/plant-fiber-$(1)/$(TSAN_TARGET): $(TSAN_APP_DIR)/plant-fiber-$(1)/fiber.o \
		$(filter-out $(TSAN_OBJ_DIR)/fiber.o,$(TSAN_LIBOBJECTS)) $(TSAN_FLAGS_STAMP)
	@printf "\n### Linking (TSan) a library with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(TSAN_CFLAGS) $$(OS_SPECIFIC_LINK_FLAGS) -o $$@ $(TSAN_APP_DIR)/plant-fiber-$(1)/fiber.o $(filter-out $(TSAN_OBJ_DIR)/fiber.o,$(TSAN_LIBOBJECTS)) $$(TSAN_LDFLAGS)
endef
$(eval $(call TSAN_FIBER_PLANT_RULE,no-tsan,NO_TSAN))

FIBER_PLANT_LIBS := $(foreach p,$(FIBER_FP_PLANTS),$(APP_DIR)/plant-fiber-$(word 1,$(subst :, ,$(p)))/$(TARGET))

.PHONY: check-fiber-defects
check-fiber-defects: ## Plant each way the fibers can be wrong and require the tests to notice
ifeq ($(OS_NAME)$(FIBER_GATE_SUPPORTED), Linuxyes)
check-fiber-defects: $(APP_DIR)/$(TARGET) $(APP_DIR)/test-fiber$(EXE_EXTENSION) $(FIBER_PLANT_LIBS) \
		$(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_APP_DIR)/test-fiber$(EXE_EXTENSION) \
		$(ASAN_APP_DIR)/plant-fiber-no-asan/$(ASAN_TARGET) \
		$(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_APP_DIR)/test-fiber$(EXE_EXTENSION) \
		$(TSAN_APP_DIR)/plant-fiber-no-tsan/$(TSAN_TARGET)
endif
check-fiber-defects:
	@printf "\n### Planting each fiber defect; the tests must report it ###\n"
ifneq ($(OS_NAME), Linux)
	@printf "check-fiber-defects: SKIPPED (Linux only); nothing was checked\n"
else ifneq ($(FIBER_GATE_SUPPORTED), yes)
	@printf "check-fiber-defects: SKIPPED (no fiber switch for target '$(FIBER_TARGET)', so the tests skip); nothing was checked\n"
else
	@set -e; \
	for p in $(FIBER_FP_PLANTS); do \
		oldifs=$$IFS; IFS=:; set -- $$p; IFS=$$oldifs; \
		sh test/check-fiber-defects.sh "$$1 (switch)" \
			$(APP_DIR)/test-fiber "$(APP_DIR)" "$(APP_DIR)/plant-fiber-$$1" \
			"FAILED +\\] Fiber\\.$$3" \
			LD_PRELOAD=; \
	done; \
	if [ -n "$(ASAN_RUNTIME)" ]; then \
		sh test/check-fiber-defects.sh "no-asan (no annotation, real stack)" \
			$(ASAN_APP_DIR)/test-fiber "$(ASAN_APP_DIR)" \
			"$(ASAN_APP_DIR)/plant-fiber-no-asan" \
			'ERROR: AddressSanitizer' \
			LD_PRELOAD="$(ASAN_RUNTIME)" \
			ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=0; \
		GCU_GATE_MAY_EXIT_ZERO=1 sh test/check-fiber-defects.sh \
			"no-asan (no annotation, default options)" \
			$(ASAN_APP_DIR)/test-fiber "$(ASAN_APP_DIR)" \
			"$(ASAN_APP_DIR)/plant-fiber-no-asan" \
			'ASan is ignoring requested __asan_handle_no_return' \
			LD_PRELOAD="$(ASAN_RUNTIME)" \
			ASAN_OPTIONS=detect_leaks=1:halt_on_error=1; \
	else \
		printf "  ASan arms SKIPPED: no ASan runtime to preload (CC is not gcc); nothing was checked there\n"; \
	fi; \
	sh test/check-fiber-defects.sh "no-tsan (no annotation)" \
		$(TSAN_APP_DIR)/test-fiber "$(TSAN_APP_DIR)" \
		"$(TSAN_APP_DIR)/plant-fiber-no-tsan" \
		'FAILED +\] Fiber\.EachFiberRunsAsItsOwnContextToThreadSanitizer' \
		LD_PRELOAD= TSAN_OPTIONS=halt_on_error=1; \
	printf "\033[0;32mEvery planted fiber defect that could be run was caught.\033[0m\n"
endif

# ---------------------------------------------------------------------------
# The loop gate, and the defects it plants
#
# The loop tests are only worth running if they are known to fail when the
# loop is wrong.  This builds the library again with one deliberate defect
# compiled into src/loop.c (the GCU_LOOP_PLANT_* macros, which no ordinary
# build defines), runs the one test that carries that defect, and requires it
# to report:
#
#   no-wake       gcu_loop_post() queues the record and never wakes the loop.
#                 The test waits in the OS with no timeout, so the loop hangs
#                 and the hang guard names the test.
#   cancel-early  gcu_loop_cancel() reports CANCELLED at once and leaves the
#                 OS reading into the buffer the caller may now free.  The
#                 test that checks the callback has not run, and that the
#                 buffer stays untouched, must fail ...
#   cancel-early  ... and, in the ASan tree, the test that frees the buffer in
#                 the completion must get a heap-use-after-free report.
#   timer-order   the timer queue is ordered by start, not by deadline: the
#                 timer test must fail.
#
# Each arm also runs the real library first, with the same filter, and
# requires it to be quiet, so a gate that fails for some other reason cannot
# pass as having caught the defect.  test/check-loop-defects.sh is the arm;
# this is the building, and mirrors check-fiber-defects above.  Linux only:
# the Windows arm is exercised by tools/xwin/loop.sh and the arm64 build by
# tools/xarch/loop.sh, each with these same defects.
# ---------------------------------------------------------------------------

LOOP_PLANTS := no-wake:NO_WAKE cancel-early:CANCEL_EARLY_RELEASE timer-order:TIMER_ORDER

define LOOP_PLANT_RULE
$(APP_DIR)/plant-loop-$(1)/loop.o: src/loop.c $(FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling loop.c with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) -DGCU_LOOP_PLANT_$(2) $$(INCLUDE) -c $$< -o $$@ $$(OS_SPECIFIC_COMPILE_FLAGS)

$(APP_DIR)/plant-loop-$(1)/$(TARGET): $(APP_DIR)/plant-loop-$(1)/loop.o \
		$(filter-out $(OBJ_DIR)/loop.o,$(LIBOBJECTS)) $(FLAGS_STAMP)
	@printf "\n### Linking a library with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(OS_SPECIFIC_LINK_FLAGS) -o $$@ $(APP_DIR)/plant-loop-$(1)/loop.o $(filter-out $(OBJ_DIR)/loop.o,$(LIBOBJECTS)) $$(LDFLAGS) $$(OS_SPECIFIC_LIBRARY_NAME_FLAG)
	@ln -f -s $(TARGET) $(APP_DIR)/plant-loop-$(1)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/plant-loop-$(1)/$(BASE_NAME)
endef
$(foreach p,$(LOOP_PLANTS),$(eval $(call LOOP_PLANT_RULE,$(word 1,$(subst :, ,$(p))),$(word 2,$(subst :, ,$(p))))))

define ASAN_LOOP_PLANT_RULE
$(ASAN_APP_DIR)/plant-loop-$(1)/loop.o: src/loop.c $(ASAN_FLAGS_STAMP) \
		| $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/float.h \
		  $(BUILD_DIR)/include/$(SUITE)/$(PROJECT)/libver_gen.h
	@printf "\n### Compiling (ASan+UBSan) loop.c with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(ASAN_CFLAGS) -DGCU_LOOP_PLANT_$(2) $$(INCLUDE) -c $$< -o $$@ $$(OS_SPECIFIC_COMPILE_FLAGS)

$(ASAN_APP_DIR)/plant-loop-$(1)/$(ASAN_TARGET): $(ASAN_APP_DIR)/plant-loop-$(1)/loop.o \
		$(filter-out $(ASAN_OBJ_DIR)/loop.o,$(ASAN_LIBOBJECTS)) $(ASAN_FLAGS_STAMP)
	@printf "\n### Linking (ASan+UBSan) a library with $(2) planted ###\n"
	@mkdir -p $$(@D)
	$$(CC) $$(ASAN_CFLAGS) $$(OS_SPECIFIC_LINK_FLAGS) -o $$@ $(ASAN_APP_DIR)/plant-loop-$(1)/loop.o $(filter-out $(ASAN_OBJ_DIR)/loop.o,$(ASAN_LIBOBJECTS)) $$(ASAN_LDFLAGS)
endef
$(eval $(call ASAN_LOOP_PLANT_RULE,cancel-early,CANCEL_EARLY_RELEASE))

LOOP_PLANT_LIBS := $(foreach p,$(LOOP_PLANTS),$(APP_DIR)/plant-loop-$(word 1,$(subst :, ,$(p)))/$(TARGET))

.PHONY: check-loop-defects
check-loop-defects: ## Plant each way the loop can be wrong and require the tests to notice
ifeq ($(OS_NAME), Linux)
check-loop-defects: $(APP_DIR)/$(TARGET) $(APP_DIR)/test-loop$(EXE_EXTENSION) $(LOOP_PLANT_LIBS) \
		$(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_APP_DIR)/test-loop$(EXE_EXTENSION) \
		$(ASAN_APP_DIR)/plant-loop-cancel-early/$(ASAN_TARGET)
endif
check-loop-defects:
	@printf "\n### Planting each loop defect; the tests must report it ###\n"
ifneq ($(OS_NAME), Linux)
	@printf "check-loop-defects: SKIPPED (Linux only); nothing was checked\n"
else
	@set -e; \
	sh test/check-loop-defects.sh "no-wake (post does not wake the loop)" \
		$(APP_DIR)/test-loop "$(APP_DIR)" "$(APP_DIR)/plant-loop-no-wake" \
		'TIMED OUT, no answer from: LoopTest\.PostFromAnotherThreadWakesAWaitingLoop' \
		LD_PRELOAD= GTEST_FILTER='LoopTest.PostFromAnotherThreadWakesAWaitingLoop'; \
	sh test/check-loop-defects.sh "cancel-early (callback before release)" \
		$(APP_DIR)/test-loop "$(APP_DIR)" "$(APP_DIR)/plant-loop-cancel-early" \
		'FAILED +\] LoopNet\.CancelledReadCompletesOnceAndNeverTouchesItsBuffer' \
		LD_PRELOAD= GTEST_FILTER='LoopNet.CancelledReadCompletesOnceAndNeverTouchesItsBuffer'; \
	sh test/check-loop-defects.sh "timer-order (queue ordered by start)" \
		$(APP_DIR)/test-loop "$(APP_DIR)" "$(APP_DIR)/plant-loop-timer-order" \
		'FAILED +\] LoopTest\.TimersFireInDeadlineOrderAndNotBeforeTheirTime' \
		LD_PRELOAD= GTEST_FILTER='LoopTest.TimersFireInDeadlineOrderAndNotBeforeTheirTime'; \
	if [ -n "$(ASAN_RUNTIME)" ]; then \
		sh test/check-loop-defects.sh "cancel-early (use after free, ASan)" \
			$(ASAN_APP_DIR)/test-loop "$(ASAN_APP_DIR)" \
			"$(ASAN_APP_DIR)/plant-loop-cancel-early" \
			'ERROR: AddressSanitizer: heap-use-after-free' \
			LD_PRELOAD="$(ASAN_RUNTIME)" \
			ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
			GTEST_FILTER='LoopNet.CancelledReadBufferMayBeFreedByItsCompletion'; \
	else \
		printf "  ASan arm SKIPPED: no ASan runtime to preload (CC is not gcc); nothing was checked there\n"; \
	fi; \
	printf "\033[0;32mEvery planted loop defect that could be run was caught.\033[0m\n"
endif

clean: ## Remove all contents of the build directories.
# The sanitizer tree is removed too. It is a sibling of the ordinary build
# directory rather than a child, so a clean that names only the ordinary one
# leaves instrumented objects behind - and they are the ones a stale-binary
# mistake is hardest to notice with, because they still run.
	-@rm -rvf $(OBJ_DIR)/*
	-@rm -rvf $(APP_DIR)/*
	-@rm -rvf $(GEN_DIR)/*
	-@rm -rvf $(ASAN_BUILD_DIR)
	-@rm -rvf $(TSAN_BUILD_DIR)
	-@rm -f include/$(SUITE)/$(PROJECT)/float.h

# Files will be as follows:
# /usr/local/lib/(SUITE)/
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR).(MINOR)
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR) link to previous
#   lib(SUITE)-(PROJECT)(BRANCH).so link to previous
# Where the dynamic loader configuration fragment goes. Overridable so a
# staged or user-prefix install has somewhere to write it; the default is the
# system location, which is what an ordinary `sudo make install` uses.
LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d

# Where this project's own .pc file is installed. Defaults to the directory
# pkg-config is already being told to search, but separate from it so a
# staged install can write somewhere else without also redirecting lookups.
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

# $(LDCONF_INSTALL_PATH)/(SUITE)-(PROJECT)(BRANCH).conf will point to $(LIB_INSTALL_PATH)/(SUITE)
# /usr/local/include/(SUITE)/(PROJECT)(BRANCH)/
#   * copied from ./include/
# /usr/local/share/pkgconfig
#   (SUITE)-(PROJECT)(BRANCH).pc created

install: ## Install the library globally, requires sudo
install: all
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
# Install the .so file
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
# The .dll goes beside the other programs' DLLs in bin/, which is where the
# loader looks once that directory is on PATH - Windows has no rpath. The
# import library goes where the .pc's -L points, in lib/$(SUITE)/, as the .so
# does on Linux. It used to go in lib/, which no -L named, so a consumer that
# found cutil through pkg-config could not link against it.
	@mkdir -p $(BIN_INSTALL_PATH)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	# Installing the headers.
	# Removed first: this directory is owned entirely by this project and
	# branch, and copying over the top of it would leave headers behind that
	# have since been renamed or deleted.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@cd include &&	find . -name "*.h" -exec cp --parents '{}' $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ \;
	@cd $(BUILD_DIR)/include &&	find . -name "*.h" -exec cp --parents '{}' $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ \;
	# Installing the pkg-config files.
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	@echo "  pkg-config name: $(SUITE)-$(PROJECT)$(BRANCH)  (use: pkg-config --cflags --libs $(SUITE)-$(PROJECT)$(BRANCH))"
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
	# Deleting the shared library.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	# Deleting the ld configuration file.
	@rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	# Deleting the headers.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	# Deleting the pkg-config files.
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	# Cleaning up (potentially) no longer needed directories.
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the shared library in DEBUG mode
	make all BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include test Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
# Cleans first because the object files would otherwise be reused without the
# instrumentation, then cleans and rebuilds at the end: leaving the
# instrumented objects behind would have a later `make` silently link them,
# and leaving the tree cleaned would break any sibling project that links
# this one. The cost is one extra build; coverage is not run often.
	@$(MAKE) --no-print-directory clean > /dev/null
# The instrumented build, the report and the restoration of the tree are one
# shell command so that the cleanup runs whatever fails. Letting a failure
# stop the recipe leaves the --coverage objects in build/, and the next
# ordinary `make` links them into a library that needs the gcov runtime; every
# later build then fails with undefined references to __gcov_init until
# somebody works out why.
#
# TEST_GATES is cleared because --coverage links the gcov runtime, which
# exports mangle_path. check-symbols is right to reject that in a shipping
# build and wrong to reject it here, and it made this target fail before it
# ever produced a report.
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		EXTRA_CFLAGS="--coverage -O0" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(OBJ_DIR) || status=$$?; \
	else \
		printf "coverage: the instrumented test run failed; no report\n" >&2; \
	fi; \
	$(MAKE) --no-print-directory clean > /dev/null; \
	$(MAKE) --no-print-directory all > /dev/null; \
	exit $$status

.PHONY: oracle-build oracle-version check-oracle

oracle-build: ## Build the pinned oracle images from their Containerfiles
	@printf "\n### Building the OpenJDK oracle image ###\n"
	docker build -t localhost/ghoti-cutil-oracle-java:21.0.12 \
		-f tools/oracle/containers/java/Containerfile \
		tools/oracle/containers/java
	@printf "\n### Building the xoshiro256++, PCG64, and Mersenne Twister oracle image ###\n"
	docker build -t localhost/ghoti-cutil-oracle-prng:rand-0.10.3 \
		-f tools/oracle/containers/prng/Containerfile \
		tools/oracle

oracle-version: ## Print the references the random oracle would use
	@GHOTI_ORACLE_REQUIRED=1 python3 tools/oracle/oracle_run.py java,pcg,xoshiro,mt -- true

$(APP_DIR)/random-words: examples/random_words.c $(APP_DIR)/$(TARGET) $(FLAGS_STAMP)
	@printf "\n### Compiling the random oracle driver ###\n"
	$(CC) -std=c17 -Wall -Wextra -Werror $(INCLUDE) -o $@ $< $(LDFLAGS) $(CUTILLIBRARY)

check-oracle: ## Judge the seeded generators against OpenJDK, Rust, pcg-c, and libstdc++
check-oracle: $(APP_DIR)/random-words
	@LD_LIBRARY_PATH="$(APP_DIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
		GHOTI_ORACLE_REQUIRED=1 python3 tools/oracle/oracle_run.py java,pcg,xoshiro,mt -- \
		python3 tools/oracle/random_diff.py $(APP_DIR)/random-words

help: ## Display this help
# Scan only this makefile. $(MAKEFILE_LIST) grows to include every generated
# .d file once the project has been built, and grep prefixes each match with
# a filename when given more than one file - so every target name in the
# output became "Makefile".
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' $(firstword $(MAKEFILE_LIST)) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "%-15s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"


####################################################################
# Flag stamps
####################################################################
# Placed at the end of the file for two reasons. Each target expands when make
# reads its line, and ASAN_FLAGS_STAMP and TSAN_FLAGS_STAMP are defined with
# their build directories much further down -- a rule above those definitions
# has an empty target and silently does not exist, which reads as "No rule to
# make target .../.flags". And the first target in a makefile is the default
# goal, so a stamp rule near the top makes a bare `make` build only the stamp.
# What a stamp records has to be everything the guarded recipes expand, not
# just the flag variables that were on hand when it was written. A rule can
# name the right stamp for the right tree and still be blind to a variable
# only its own recipe mentions -- the rule looks guarded, the audit agrees,
# and the rebuild never happens.
#
# $(CC) and $(CXX) were in no stamp at all, which made `make CC=clang` a
# silent no-op. Measured 2026-09-23 before the fix: `make test-asan CC=clang
# CXX=clang++` compiled 0 objects and re-ran GCC's binaries, and
# `readelf -p .comment` on the ASan objects said GCC. The suite reported 538
# passing "under clang" and no clang had run. A second compiler is a gate, so
# that is a gate reporting green while switched off.
#
# $(TESTFLAGS) is the gtest flags from pkg-config and is expanded here through
# $(shell ...) rather than recorded as the literal backtick string the recipes
# use, because the string never changes and the flags it produces do. That is
# what makes a gtest upgrade rebuild the tests.
GTEST_FLAGS_NOW := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest 2>/dev/null)
# Taken through $(call) with the tree's own app directory, the way the recipes
# take them -- expanded bare they come out with an empty $(1) and the stamp
# stops reflecting the -D values that actually reach the compiler.
TEST_CPPFLAGS_ALL = $(call TEST_CPPFLAGS_test-library,$(1)) \
	$(call TEST_CPPFLAGS_test-filelock,$(1)) $(call TEST_CPPFLAGS_test-mmap,$(1)) \
	$(call TEST_CPPFLAGS_test-subprocess,$(1))
COMMON_STAMP_TEXT := $(CC) $(CXX) $(INCLUDE) $(TESTFLAGS) $(GTEST_FLAGS_NOW) \
	$(OS_SPECIFIC_COMPILE_FLAGS) $(OS_SPECIFIC_LINK_FLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

.PHONY: force-flags
$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(COMMON_STAMP_TEXT) $(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(CUTILLIBRARY) $(call TEST_CPPFLAGS_ALL,$(APP_DIR))' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(COMMON_STAMP_TEXT) $(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(ASAN_CUTILLIBRARY) $(call TEST_CPPFLAGS_ALL,$(ASAN_APP_DIR))' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(COMMON_STAMP_TEXT) $(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(TSAN_CUTILLIBRARY) $(call TEST_CPPFLAGS_ALL,$(TSAN_APP_DIR))' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
