ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/mk/toolchain.mk

VERSION := $(shell cat VERSION)
PKGREL ?= 1
LIB_DIR := $(BIN_DIR)/lib
OBJ := $(BIN_DIR)/obj/purec-os-pkg
PROGRAM := $(BIN_DIR)/programs/pkg
OUT := $(BIN_DIR)/repo
IMAGE := $(BIN_DIR)/pkg-image
CRYPT_DIR := $(ROOT_DIR)/libxcrypt
SOURCES := main util db repo json tx sig archive version info
OBJECTS := $(SOURCES:%=$(OBJ)/%.o)
LIBS := $(LIB_DIR)/libz.a $(LIB_DIR)/libwolf.a $(LIB_DIR)/libpurecrypt.a $(LIB_DIR)/libpurefs.a $(LIB_DIR)/libpurec.a
CFLAGS := $(CPPFLAGS) -I$(ROOT_DIR)/src/libc/include/hosted -I$(CURDIR)/src -I$(CRYPT_DIR)/include \
	-I$(ROOT_DIR)/netsurf-libs/zlib -I$(ROOT_DIR)/wolfssl -I$(ROOT_DIR)/wolfssl/purec \
	-DWOLFSSL_USER_SETTINGS -DWOLF_PUREC_TLS -DPKG_VERSION_TEXT='"pkg $(VERSION)"' $(DEPFLAGS) $(USER_CFLAGS)
PKG_FILE := $(OUT)/pkg-$(VERSION)-$(PKGREL)-x86_64.pkg.tar.gz
SIGN_KEY ?= $(HOME)/.config/purec-pkg/signing.pem
HOST := $(CURDIR)/build/host/pkg

.PHONY: all package image host test clean

all: $(PROGRAM)

$(OBJ)/%.o: src/%.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBS):
	@echo "missing $@: build the PureC-OS tree first (make libraries kernel, make -C mk/netsurf zlib, make wolf)" >&2
	@false

$(PROGRAM): $(OBJECTS) $(LIB_DIR)/crt0.o $(LIBS) $(ROOT_DIR)/linker-userspace.ld
	@mkdir -p $(@D)
	$(LD) -S -T $(ROOT_DIR)/linker-userspace.ld -o $@ $(LIB_DIR)/crt0.o $(OBJECTS) $(LIBS)

package: $(PROGRAM)
	@mkdir -p $(OUT)
	rm -f $(OUT)/pkg-*.pkg.tar.gz*
	python3 $(ROOT_DIR)/mk/pkg/mkpkg.py --name pkg --version $(VERSION)-$(PKGREL) \
		--desc "PureC OS package manager" -o $(OUT) \
		--file usr/bin/pkg=$(PROGRAM) \
		--file usr/share/man/pkg.8=$(CURDIR)/pkg.8
	if [ -f "$(SIGN_KEY)" ]; then python3 $(ROOT_DIR)/mk/pkg/pkgsign.py sign $(SIGN_KEY) $(PKG_FILE) $(PKG_FILE).sig; fi
	@ls -l $(OUT)/pkg-*

# Local database entry for the OS image: /usr/bin/pkg then belongs to the package
# "pkg" from the first boot, so "pkg -Syu" can upgrade pkg itself.
image: package
	@mkdir -p $(IMAGE)
	python3 -c 'import sys, tarfile, time; t = tarfile.open(sys.argv[1], "r:gz"); \
info = t.extractfile(".PKGINFO").read().decode(); \
files = [m.name + ("/" if m.isdir() else "") for m in t.getmembers() if not (m.name.startswith(".") and "/" not in m.name)]; \
open(sys.argv[2] + "/desc", "w").write(info.rstrip("\n") + "\ninstalldate = %d\nreason = 0\n" % int(time.time())); \
open(sys.argv[2] + "/files", "w").write("".join(f + "\n" for f in files))' $(PKG_FILE) $(IMAGE)
	@echo "image database entry: $(IMAGE)/desc, $(IMAGE)/files"

host: $(HOST)

$(HOST): $(wildcard src/*.c src/*.h) VERSION
	@mkdir -p $(@D)
	gcc -O1 -g -Wall -DPKG_SIG_OPENSSL -DPKG_VERSION_TEXT='"pkg $(VERSION)"' -I$(CRYPT_DIR)/include -Isrc \
		src/*.c $(CRYPT_DIR)/src/sha512.c -lz -lcrypto -o $@

test: $(HOST)
	PKG=$(HOST) MK=$(ROOT_DIR)/mk/pkg tests/registry.sh

clean:
	rm -rf $(OBJ) build $(PROGRAM) $(IMAGE)

-include $(OBJECTS:.o=.d)
