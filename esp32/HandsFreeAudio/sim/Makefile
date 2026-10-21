## Host simulator for the HandsFreeAudio firmware
# Builds ../main/main.c unmodified against the mocks in stubs/ and runs it on the PC,
# linked with the same fixed-point SpeexDSP the firmware builds (../components/speexdsp).
#   make                build ./build/sim
#   make test           build, then run every scenario and the SpeexDSP check
#   make run S=4        build, then run one scenario (SPEED=20 by default)
#   make speexcheck     build and run ./build/speexcheck: SpeexDSP memory, ERLE, leaks
#   make clean          delete ./build

# CFLAGS can be overridden (make CFLAGS="-O0 -g"); SIM_FLAGS and SPEEX_FLAGS are always applied.
CFLAGS    ?= -O2 -g
SIM_FLAGS := -std=gnu11 -pthread -Wall -Wno-unused-function -Wno-unused-but-set-variable \
             -Istubs -I../main
LDLIBS    := -lm
# ESP-IDF's export.sh puts its ULP toolchain's bare `ld` ahead of the system one on
# PATH, and gcc then links with it ("unrecognised emulation mode: elf_x86_64"). -B
# makes gcc look in /usr/bin first for its linker, so the sim also builds in a shell
# where ESP-IDF is loaded.
LDFLAGS   := $(if $(wildcard /usr/bin/ld),-B/usr/bin/)

# SpeexDSP: the same sources and defines as ../components/speexdsp/CMakeLists.txt.
SPEEX_DIR     := ../components/speexdsp
SPEEX_INCLUDE := -I$(SPEEX_DIR)/include
SPEEX_FLAGS   := -std=gnu11 -w -DFIXED_POINT -DUSE_KISS_FFT -DOS_SUPPORT_CUSTOM -DEXPORT= \
                 $(SPEEX_INCLUDE) -I$(SPEEX_DIR)/libspeexdsp
SPEEX_SOURCES := $(addprefix $(SPEEX_DIR)/libspeexdsp/,mdf.c preprocess.c filterbank.c \
                   fftwrap.c kiss_fft.c kiss_fftr.c) $(SPEEX_DIR)/speexdsp_alloc.c
SPEEX_HEADERS := $(wildcard $(SPEEX_DIR)/include/*.h $(SPEEX_DIR)/include/speex/*.h \
                   $(SPEEX_DIR)/libspeexdsp/*.h)

SPEED     ?= 20
SCENARIOS := 1 2 3 4 5 9 10
BUILD_DIR := build
SIM_BIN   := $(BUILD_DIR)/sim
CHECK_BIN := $(BUILD_DIR)/speexcheck
SPEEX_OBJECTS := $(addprefix $(BUILD_DIR)/speexdsp/,$(notdir $(SPEEX_SOURCES:.c=.o)))
SOURCES   := sim.c stubs/espmock.c
HEADERS   := ../main/main.c $(wildcard stubs/*.h stubs/*/*.h)

vpath %.c $(SPEEX_DIR)/libspeexdsp $(SPEEX_DIR)

.PHONY: all test run speexcheck clean

all: $(SIM_BIN) $(CHECK_BIN)

$(BUILD_DIR)/speexdsp/%.o: %.c $(SPEEX_HEADERS)
	@mkdir -p $(BUILD_DIR)/speexdsp
	$(CC) $(SPEEX_FLAGS) $(CFLAGS) -c -o $@ $<

$(SIM_BIN): $(SOURCES) $(HEADERS) $(SPEEX_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(SIM_FLAGS) $(SPEEX_INCLUDE) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(SPEEX_OBJECTS) $(LDLIBS)

$(CHECK_BIN): speexcheck.c $(SPEEX_OBJECTS) $(SPEEX_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=gnu11 -Wall $(SPEEX_INCLUDE) $(CFLAGS) $(LDFLAGS) -o $@ speexcheck.c $(SPEEX_OBJECTS) $(LDLIBS)

test: $(SIM_BIN) $(CHECK_BIN)
	@for scenario in $(SCENARIOS); do \
		echo "=== scenario $$scenario ==="; \
		./$(SIM_BIN) $$scenario $(SPEED) || exit 1; \
	done
	@echo "=== SpeexDSP check ==="
	@./$(CHECK_BIN)

run: $(SIM_BIN)
	@test -n "$(S)" || { echo "usage: make run S=<scenario>"; exit 2; }
	./$(SIM_BIN) $(S) $(SPEED)

speexcheck: $(CHECK_BIN)
	./$(CHECK_BIN)

clean:
	rm -rf $(BUILD_DIR)
