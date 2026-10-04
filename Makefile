# PC98PLAYER（SDL 版）のビルド。Windows 版は PC98PLAYER.sln / CMakeLists.txt / build_win.sh
#
#   make                        手元の環境で（SDL3 / FreeType はシステムのもの）
#   make dist                   配布用（SDL3 / FreeType をソースから静的に埋め込む）→ out/dist/
#   make dist ARCH=universal    macOS: arm64 / x86_64 / universal
#   make info                   できたものの依存ライブラリとアーキテクチャ
#   make clean / distclean
#
# 調整用: BUILD_TYPE, MACOS_MIN, SDL_TAG, FT_TAG, CMAKE_FLAGS（全部に）, SDL_FLAGS / FT_FLAGS / APP_FLAGS（個別に）

OS   := $(if $(filter Darwin,$(shell uname -s)),macos,linux)
ARCH ?= $(shell uname -m)
BUILD_TYPE ?= Release
MACOS_MIN  ?= 11.0
SDL_TAG ?= release-3.4.16
FT_TAG  ?= VER-2-14-3
JOBS ?= $(shell getconf _NPROCESSORS_ONLN)

TAG  := $(OS)-$(ARCH)
SRC  := out/src
B    := out/build/$(TAG)
DEPS := $(abspath $(B)/deps)
DIST := out/dist

# 手元の LDFLAGS など（Homebrew の llvm@14 など）を持ち込まない
unexport LDFLAGS CPPFLAGS CFLAGS CXXFLAGS LIBRARY_PATH CPATH

CMAKE_COMMON := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)
ifeq ($(OS),macos)
  ifeq ($(ARCH),universal)
    CMAKE_ARCHS := arm64;x86_64
  else
    CMAKE_ARCHS := $(ARCH)
  endif
  CMAKE_COMMON += -DCMAKE_OSX_ARCHITECTURES="$(CMAKE_ARCHS)"
  # 配布用では Homebrew / MacPorts のライブラリを拾わず、古い macOS でも動くようにする
  VENDOR_ENV   := PKG_CONFIG_LIBDIR=$(DEPS)/lib/pkgconfig
  VENDOR_FLAGS := -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_MIN) -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local"
  APP := PC98PLAYER.app
else
  APP := PC98PLAYER_SDL
endif
VENDOR_FLAGS += -DCMAKE_INSTALL_PREFIX=$(DEPS) -DCMAKE_PREFIX_PATH=$(DEPS)

.PHONY: all sdl dist deps info clean distclean
all: sdl

# ---- 手元用 ---------------------------------------------------------------
sdl: ymfm
	cmake -S sdl -B $(B)/sdl $(CMAKE_COMMON) $(APP_FLAGS)
	cmake --build $(B)/sdl -j$(JOBS)

# ---- 配布用 ---------------------------------------------------------------
deps: $(DEPS)/lib/libSDL3.a $(DEPS)/lib/libfreetype.a

$(SRC)/SDL/CMakeLists.txt:
	git clone --depth 1 --branch $(SDL_TAG) https://github.com/libsdl-org/SDL.git $(SRC)/SDL
$(SRC)/freetype/CMakeLists.txt:
	git clone --depth 1 --branch $(FT_TAG) https://gitlab.freedesktop.org/freetype/freetype.git $(SRC)/freetype

$(DEPS)/lib/libSDL3.a: $(SRC)/SDL/CMakeLists.txt
	$(VENDOR_ENV) cmake -S $(SRC)/SDL -B $(B)/SDL $(CMAKE_COMMON) $(VENDOR_FLAGS) \
	  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF $(SDL_FLAGS)
	cmake --build $(B)/SDL -j$(JOBS)
	cmake --install $(B)/SDL

# 16 ドットの白黒描画しかしないので、外部ライブラリはすべて切る
$(DEPS)/lib/libfreetype.a: $(SRC)/freetype/CMakeLists.txt
	$(VENDOR_ENV) cmake -S $(SRC)/freetype -B $(B)/freetype $(CMAKE_COMMON) $(VENDOR_FLAGS) -DBUILD_SHARED_LIBS=OFF \
	  -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON $(FT_FLAGS)
	cmake --build $(B)/freetype -j$(JOBS)
	cmake --install $(B)/freetype

dist: ymfm deps
	$(VENDOR_ENV) cmake -S sdl -B $(B)/app $(CMAKE_COMMON) $(VENDOR_FLAGS) -DFREETYPE_DIR=$(DEPS) $(APP_FLAGS)
	cmake --build $(B)/app -j$(JOBS)
	rm -rf $(DIST)/$(TAG) && mkdir -p $(DIST)/$(TAG)
	cp -R $(B)/app/$(APP) README.md LICENSE THIRD-PARTY-ymfm-LICENSE.txt PC98PLAYER_MANUAL.html manual 設定INI作成.html $(DIST)/$(TAG)/
	cp $(SRC)/SDL/LICENSE.txt $(DIST)/$(TAG)/THIRD-PARTY-SDL3-LICENSE.txt
	cp $(SRC)/freetype/docs/FTL.TXT $(DIST)/$(TAG)/THIRD-PARTY-FreeType-LICENSE.txt
	cp third_party/TinySoundFont/LICENSE $(DIST)/$(TAG)/THIRD-PARTY-TinySoundFont-LICENSE.txt
ifeq ($(OS),macos)
	codesign --force --sign - $(DIST)/$(TAG)/$(APP)
	cd $(DIST) && rm -f PC98PLAYER-$(TAG).zip && ditto -c -k --keepParent $(TAG) PC98PLAYER-$(TAG).zip
else
	cd $(DIST) && tar czf PC98PLAYER-$(TAG).tar.gz $(TAG)
endif
	@$(MAKE) --no-print-directory info

info:
ifeq ($(OS),macos)
	@B=$(DIST)/$(TAG)/$(APP)/Contents/MacOS/PC98PLAYER; otool -L $$B; lipo -info $$B; otool -l $$B | grep -A3 LC_BUILD_VERSION | grep minos
else
	@B=$(DIST)/$(TAG)/$(APP); ldd $$B; file $$B
endif

ymfm:
	@[ -f third_party/ymfm/src/ymfm_opn.cpp ] || git submodule update --init third_party/ymfm
.PHONY: ymfm

clean:
	rm -rf $(B)/sdl $(B)/app $(DIST)/$(TAG) $(DIST)/PC98PLAYER-$(TAG).*
distclean:
	rm -rf out/build out/src $(DIST)
