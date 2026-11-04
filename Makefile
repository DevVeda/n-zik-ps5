# n-zik-ps5
#   make host     Mac/Linux build for testing against tools/mock_nzik.py (needs ffmpeg dev libs)
#   make test     host build + end-to-end test with the mock phone and a mock Kabuki
#   make ps5      build/n-zik-ps5.elf (needs build/ffmpeg-install: tools/build-ffmpeg.sh)
#   make push     upload to Payload Manager by FTP (PS5_HOST=<console IP>, or set it in local.mk)

SDK      ?= $(abspath ../ps5-payload-sdk)
-include local.mk # PS5_HOST := <console IP> (not in git)
FTP      = ftp://$(PS5_HOST):2121
need-host = @[ -n "$(PS5_HOST)" ] || { echo "Set PS5_HOST=<console IP> (or put it in local.mk)"; exit 1; }
export LLVM_CONFIG ?= /opt/homebrew/opt/llvm@18/bin/llvm-config

B        := build
TP       := third_party
COMMON   := src/net.cpp src/bridge.cpp src/pairing.cpp src/player.cpp src/library.cpp src/local.cpp src/offline.cpp src/audio/decoder.cpp src/ui/kabuki_ui.cpp src/main.cpp
WARN     := -Wall -Wextra -Wno-missing-field-initializers
CXXSTD   := -std=c++20

# ---- host ----
FF_HOST_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libswresample libavutil 2>/dev/null)
FF_HOST_LIBS   := $(shell pkg-config --libs libavformat libavcodec libswresample libavutil 2>/dev/null)
HOST_SRC := $(COMMON) src/audio/file_sink.cpp
# The Xcode SDK: the newer Command Line Tools SDK may not match the toolchain's linker.
HOST_ENV := SDKROOT=$(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)

host: $(B)/n-zik-host
$(B)/n-zik-host: $(HOST_SRC) $(wildcard src/*.hpp src/*/*.hpp) $(TP)/qrcodegen.c
	@mkdir -p $(B)
	$(HOST_ENV) /usr/bin/cc -O2 -c $(TP)/qrcodegen.c -o $(B)/qrcodegen-host.o
	$(HOST_ENV) /usr/bin/c++ $(CXXSTD) -O2 -g $(WARN) -I$(TP) $(FF_HOST_CFLAGS) $(HOST_SRC) $(B)/qrcodegen-host.o $(FF_HOST_LIBS) -o $@

test: host $(B)/test-decoder $(B)/test-json $(B)/test-net
	$(B)/test-json
	$(B)/test-net
	python3 tools/test_host.py

$(B)/test-json: tools/test_json.cpp src/json.hpp
	$(HOST_ENV) /usr/bin/c++ $(CXXSTD) -g -fsanitize=address,undefined -I$(TP) -Isrc $< -o $@
$(B)/test-net: tools/test_net.cpp src/net.cpp src/net.hpp
	$(HOST_ENV) /usr/bin/c++ $(CXXSTD) -g -fsanitize=address,undefined -I$(TP) -Isrc tools/test_net.cpp src/net.cpp -o $@
$(B)/test-decoder: tools/test_decoder.cpp src/audio/decoder.cpp src/net.cpp
	$(HOST_ENV) /usr/bin/c++ $(CXXSTD) -O1 -g -fsanitize=address,undefined -I$(TP) -Isrc $(FF_HOST_CFLAGS) $^ $(FF_HOST_LIBS) -o $@

# ---- console ----
PCC   := $(SDK)/bin/prospero-clang
PCXX  := $(SDK)/bin/prospero-clang++
PXX   := -O2 -fPIC $(WARN) $(CXXSTD) -fno-exceptions -fno-rtti -fno-threadsafe-statics -I$(TP) -I$(TP)/shellui-loader -Isrc
LOADER := $(B)/ps5/pt.o $(B)/ps5/loader.o $(B)/ps5/injector.o $(B)/ps5/remote_call.o $(B)/ps5/worker-blob.o $(B)/ps5/host.o

$(B)/ps5/worker.elf: src/shell/worker.cpp src/shell/protocol.hpp
	@mkdir -p $(B)/ps5
	$(PCXX) $(PXX) -Wframe-larger-than=16384 -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv $< -lkernel_sys -lSceLibcInternal -o $@
$(B)/ps5/worker-blob.c: $(B)/ps5/worker.elf
	python3 tools/embed.py $< worker_blob $@
$(B)/ps5/worker-blob.o: $(B)/ps5/worker-blob.c
	$(PCC) -O2 -fPIC -c $< -o $@
$(B)/ps5/pt.o: $(TP)/shellui-loader/pt.c
	@mkdir -p $(B)/ps5
	$(PCC) -O2 -fPIC -Wno-sign-compare -I$(TP)/shellui-loader -c $< -o $@
$(B)/ps5/loader.o: $(TP)/shellui-loader/stable_elfldr_bridge.c
	$(PCC) -O2 -fPIC -I$(TP)/shellui-loader -c $< -o $@
$(B)/ps5/injector.o: $(TP)/shellui-loader/stable_shellui_injector.cpp
	$(PCXX) $(PXX) -c $< -o $@
$(B)/ps5/remote_call.o: $(TP)/shellui-loader/remote_call.cpp
	$(PCXX) $(PXX) -c $< -o $@
$(B)/ps5/host.o: src/shell/host.cpp src/shell/host.hpp src/shell/protocol.hpp
	$(PCXX) $(PXX) -c $< -o $@

FF  := $(B)/ffmpeg-install
ps5: $(B)/n-zik-ps5.elf
$(B)/n-zik-ps5.elf: $(COMMON) src/audio/shell_sink.cpp $(wildcard src/*.hpp src/*/*.hpp) $(LOADER) $(FF)/lib/libavformat.a
	$(PCC) -O2 -c $(TP)/qrcodegen.c -o $(B)/ps5/qrcodegen.o
	$(PCXX) $(PXX) -I$(FF)/include $(COMMON) src/audio/shell_sink.cpp $(B)/ps5/qrcodegen.o $(LOADER) \
	  $(FF)/lib/libavformat.a $(FF)/lib/libavcodec.a $(FF)/lib/libswresample.a $(FF)/lib/libavutil.a \
	  -lkernel_sys -lSceLibcInternal -lSceNet -lSceRandom -lSceSystemService -lc++ -lc++abi -Wl,--gc-sections -o $@
	$(SDK)/bin/prospero-strip --strip-all $@ # payloads much over 2 MiB fail to start
	@ls -la $@
$(FF)/lib/libavformat.a:
	@echo "FFmpeg for PS5 missing: run tools/build-ffmpeg.sh" && false

# Payload Manager (itsPLK): one local payload per folder, like veda-hub's `make install`.
PLDMGR := $(FTP)/data/pldmgr/payloads/local
UPLOAD := curl -sS --fail --ftp-create-dirs --disable-epsv --connect-timeout 5 -T
push: $(B)/n-zik-ps5.elf
	$(need-host)
	$(UPLOAD) $(B)/n-zik-ps5.elf $(PLDMGR)/n-zik-ps5/n-zik-ps5.elf
log:
	$(need-host)
	-curl -sS $(FTP)/data/n-zik-ps5/n-zik-ps5.log
	-curl -sS $(FTP)/data/n-zik-ps5/shell/worker.log
stop:
	$(need-host)
	printf 1 | curl -sS -T - $(FTP)/data/n-zik-ps5/stop

clean:
	rm -rf $(B)/ps5 $(B)/n-zik-host $(B)/*.elf $(B)/*.o

.PHONY: host test ps5 push log stop clean
