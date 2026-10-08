.DEFAULT_GOAL := host
SOURCES := $(wildcard backend/*.c) .deps/cjson/cJSON.c
REMOTE_HEADERS := backend/remote_source.h backend/webdav_listing.h vendor/yxml/yxml.c vendor/yxml/yxml.h
CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ibackend -Ilauncher -I.deps/cjson -Ibuild/generated
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-misleading-indentation
HOST_CC ?= clang-19
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_LIB := $(PS5_PAYLOAD_SDK)/target/user/homebrew
PS5_STRIP := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
GENERATED := build/generated/assets.h build/generated/catalog.h build/generated/ca.h build/generated/network.h build/generated/launcher.h
PAYLOAD_SOURCES := $(SOURCES) launcher/install.c launcher/platform_ps5.c
build/atmosphere_runtime.elf build/atmosphere.elf build/atmosphere_core.elf: $(REMOTE_HEADERS)
PAYLOAD_CC = $(PS5_CC) $(CPPFLAGS) $(CFLAGS) -DATMOSPHERE_INSTALL_LAUNCHER -Wno-unreachable-code-generic-assoc -I$(PS5_LIB)/include
# The SDK opens DT_NEEDED libraries in order. AppInst uses Ipmi internally;
# retain and load Ipmi first even though Atmosphere does not call its API directly.
PS5_LAUNCHER_LIBS := -Wl,--push-state,--no-as-needed -lSceIpmi -Wl,--pop-state -lSceAppInstUtil
PAYLOAD_LIBS := -L$(PS5_LIB)/lib -lsmb2 -lcurl -lmicrohttpd -lssl -lcrypto -lz $(PS5_LAUNCHER_LIBS)
PS5_FS_WRAPS := -Wl,--wrap=openat -Wl,--wrap=mkdirat -Wl,--wrap=fstatat -Wl,--wrap=linkat -Wl,--wrap=renameat -Wl,--wrap=unlinkat
.PHONY: host payload payload-core test-backend test-launcher test-payload-imports test-updates
build/generated/network.h build/generated/launcher.h &: tools/embed-launcher.py config/network.json launcher/sce_sys/param.json launcher/sce_sys/icon0.png
	python3 tools/embed-launcher.py
host: build/atmosphere-host
build/atmosphere-host: $(SOURCES) $(REMOTE_HEADERS) backend/atmosphere.h backend/image_metadata.h backend/game_region.h backend/copy_pipeline.h $(GENERATED)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DATMOSPHERE_DESKTOP $(SOURCES) -o $@ $(LDFLAGS) -lsmb2 -lcurl -lmicrohttpd -lssl -lcrypto -lz -lpthread -lm $(LDLIBS)
build/atmosphere-test: $(SOURCES) $(REMOTE_HEADERS) backend/atmosphere.h backend/image_metadata.h backend/game_region.h backend/copy_pipeline.h $(GENERATED)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DATMOSPHERE_DESKTOP -DATMOSPHERE_TEST $(SOURCES) -o $@ $(LDFLAGS) -lsmb2 -lcurl -lmicrohttpd -lssl -lcrypto -lz -lpthread -lm $(LDLIBS)
payload: build/atmosphere.elf
# The runtime is the copy saved on the console for payload managers to start.
build/atmosphere_runtime.elf: $(PAYLOAD_SOURCES) backend/atmosphere.h backend/image_metadata.h launcher/install.h $(GENERATED) Makefile
	$(PAYLOAD_CC) $(PAYLOAD_SOURCES) -o $@ $(PAYLOAD_LIBS) $(PS5_FS_WRAPS)
	$(PS5_STRIP) --strip-debug $@
# The public payload: the same server and icon setup, carrying the runtime it saves.
build/atmosphere.elf: $(PAYLOAD_SOURCES) backend/atmosphere.h backend/image_metadata.h launcher/install.h launcher/runtime_image.c $(GENERATED) build/atmosphere_runtime.elf Makefile
	$(PAYLOAD_CC) -DATMOSPHERE_EMBED_RUNTIME $(PAYLOAD_SOURCES) launcher/runtime_image.c -o $@ $(PAYLOAD_LIBS) $(PS5_FS_WRAPS)
payload-core: build/atmosphere_core.elf
build/atmosphere_core.elf: $(SOURCES) backend/atmosphere.h backend/image_metadata.h $(GENERATED) Makefile
	$(PS5_CC) $(CPPFLAGS) $(CFLAGS) -Wno-unreachable-code-generic-assoc -I$(PS5_LIB)/include $(SOURCES) -o $@ -L$(PS5_LIB)/lib -lsmb2 -lcurl -lmicrohttpd -lssl -lcrypto -lz $(PS5_FS_WRAPS)
test-updates: build/atmosphere-test
	python3 tests/update_integration.py

test-backend: build/atmosphere-host build/atmosphere-test
	python3 tests/backend_integration.py
build/atmosphere-launcher-test: launcher/install.c launcher/install.h tests/launcher_driver.c build/generated/launcher.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) launcher/install.c tests/launcher_driver.c -o $@
build/atmosphere-launcher-platform-test: launcher/platform_ps5.c launcher/install.h tests/launcher_platform_driver.c tests/stubs/ps5/kernel.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -Itests/stubs launcher/platform_ps5.c tests/launcher_platform_driver.c -Wl,--wrap=dlsym -o $@
test-launcher: build/atmosphere-launcher-test build/atmosphere-launcher-platform-test
	python3 tests/launcher_integration.py
	python3 tests/launcher_platform.py
test-payload-imports: payload payload-core
	python3 tests/payload_imports.py

.PHONY: test-catalog-refresh
test-catalog-refresh: build/atmosphere-test
	python3 tests/catalog_refresh.py

.PHONY: test-library
test-library: build/atmosphere-host build/atmosphere-test
	python3 tests/library_integration.py
	python3 tests/library_actions.py
