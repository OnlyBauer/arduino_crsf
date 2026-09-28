# Host-side tests for the Arduino wrapper.
#
# The code under test is the real, shipping src/CrsfPort.cpp. What is replaced
# is Arduino.h, by arduino_stubs/ -- so Stream, HardwareSerial and micros() are
# things a test controls.
#
#   make              build and run everything
#   make sanitize     the same, under AddressSanitizer and UBSan
#   make clean
#
# The C sources are compiled with the C compiler and the wrapper with the C++
# one, which is what the Arduino build does. Compiling the core as C++ is not a
# supported configuration and would not be representative.
#
# Read arduino_stubs/Arduino.h before trusting a pass: there is no board here.

CC       ?= gcc
CXX      ?= g++
CPPFLAGS := -Isrc -I. -Iarduino_stubs
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2
CXXFLAGS := -std=c++14 -Wall -Wextra -Werror -O2
LDLIBS   := -lm
BUILD    := ../build_tests

SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

# The vendored core, flat in src/ because the Arduino 1.5 format only puts src/
# itself on the include path. Its own suites are not here: they ran in c_crsf,
# against these same bytes.
CORE_SRCS := crsf_core.c crsf_crc.c crsf_parser.c crsf_codec.c crsf_tunnel.c \
             crsf_params.c crsf_router.c
CORE_OBJS := $(addprefix $(BUILD)/,$(CORE_SRCS:.c=.o))

CXX_SRCS  := ../src/CrsfPort.cpp arduino_stubs/arduino_stubs.cpp
SUITES    := test_arduino_port

.PHONY: all run sanitize clean
all: run

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: ../src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I../src $(CPPFLAGS:-Isrc=-I../src) -c $< -o $@

$(BUILD)/test_arduino_port: test_arduino_port.cpp $(CXX_SRCS) $(CORE_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I../src -I. -Iarduino_stubs -o $@ \
	    test_arduino_port.cpp $(CXX_SRCS) $(CORE_OBJS) $(LDLIBS)

BINS := $(addprefix $(BUILD)/,$(SUITES))

# Each suite has to say how it went rather than only setting an exit status: a
# suite killed by a signal does not reliably report one, and under Cygwin make a
# segfaulting binary arrives as exit 0.
run: $(BINS)
	@fail=0; reported=0; \
	for t in $(BINS); do \
	  out=$$(./$$t 2>&1) || fail=1; \
	  printf '%s\n' "$$out"; \
	  case "$$out" in *PASS*|*FAIL*) reported=$$((reported+1)) ;; esac; \
	done; \
	expected=$(words $(SUITES)); \
	if [ $$reported -ne $$expected ]; then \
	  echo "ONLY $$reported OF $$expected SUITES REPORTED — one died without saying so"; \
	  exit 1; \
	fi; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; fi; \
	echo "All $$expected suites passed."

sanitize: | $(BUILD)
	@for s in $(SUITES); do \
	  $(CXX) $(CXXFLAGS) $(SANFLAGS) -I../src -I. -Iarduino_stubs \
	      -o $(BUILD)/$$s.san $$s.cpp $(CXX_SRCS) \
	      $(addprefix ../src/,$(CORE_SRCS)) $(LDLIBS) || exit 1; \
	  ./$(BUILD)/$$s.san || exit 1; \
	done
	@echo "Sanitized suites passed."

clean:
	rm -rf $(BUILD)
