################################################################################
#
# sendspin-cpp
#
# Sendspin synchronized audio streaming library (staging only). Linked into
# linux-voice-assistant-cpp, which owns playback, LED and the HA media player.
#
################################################################################

SENDSPIN_CPP_VERSION = v0.9.5
SENDSPIN_CPP_VERSION_NUMBER = $(patsubst v%,%,$(SENDSPIN_CPP_VERSION))
SENDSPIN_CPP_SITE = $(call github,Sendspin,sendspin-cpp,$(SENDSPIN_CPP_VERSION))
SENDSPIN_CPP_LICENSE = Apache-2.0, MIT, BSD-3-Clause
SENDSPIN_CPP_LICENSE_FILES = \
	LICENSE \
	third_party/arduinojson/LICENSE.txt \
	third_party/micro-flac/LICENSE \
	third_party/micro-flac/lib/micro-ogg-demuxer/LICENSE \
	third_party/micro-opus/LICENSE \
	third_party/micro-opus/lib/opus/COPYING \
	third_party/ixwebsocket/LICENSE.txt \
	third_party/noise-c/COPYING
SENDSPIN_CPP_DEPENDENCIES =
SENDSPIN_CPP_INSTALL_STAGING = YES
SENDSPIN_CPP_INSTALL_TARGET = NO

SENDSPIN_CPP_ARDUINOJSON_VERSION = 32520135092970120a5ac165cf45f48e658c421d
SENDSPIN_CPP_MICRO_FLAC_VERSION = f9c27d4d0166021db977d7fc37d205b21d81e9f2
SENDSPIN_CPP_MICRO_OPUS_VERSION = 3e9ce44c56ab8007261d68c511f0162599542aa2
SENDSPIN_CPP_IXWEBSOCKET_VERSION = 64fae7676bd8fe31f7cb4bcde7a6841892dad65e
SENDSPIN_CPP_NOISE_C_VERSION = a1e08809a1b8f65cd91765ba7d68a6d00648ad61
SENDSPIN_CPP_MICRO_OGG_VERSION = 865ad9d831e7dc76bb9c142607bae33fc75648e7
# micro-flac v0.3.0 pins a newer micro-ogg-demuxer than micro-opus v0.3.5.
SENDSPIN_CPP_MICRO_FLAC_OGG_VERSION = ee95ec6f6b633789ec31e0e44877f6cc7fe7cf67
SENDSPIN_CPP_OPUS_VERSION = 22244de5a79bd1d6d623c32e72bf1954b56235be

SENDSPIN_CPP_ARDUINOJSON_ARCHIVE = arduinojson-$(SENDSPIN_CPP_ARDUINOJSON_VERSION).tar.gz
SENDSPIN_CPP_MICRO_FLAC_ARCHIVE = micro-flac-$(SENDSPIN_CPP_MICRO_FLAC_VERSION).tar.gz
SENDSPIN_CPP_MICRO_OPUS_ARCHIVE = micro-opus-$(SENDSPIN_CPP_MICRO_OPUS_VERSION).tar.gz
SENDSPIN_CPP_IXWEBSOCKET_ARCHIVE = ixwebsocket-$(SENDSPIN_CPP_IXWEBSOCKET_VERSION).tar.gz
SENDSPIN_CPP_NOISE_C_ARCHIVE = noise-c-$(SENDSPIN_CPP_NOISE_C_VERSION).tar.gz
SENDSPIN_CPP_MICRO_OGG_ARCHIVE = micro-ogg-demuxer-$(SENDSPIN_CPP_MICRO_OGG_VERSION).tar.gz
SENDSPIN_CPP_MICRO_FLAC_OGG_ARCHIVE = micro-ogg-demuxer-$(SENDSPIN_CPP_MICRO_FLAC_OGG_VERSION).tar.gz
SENDSPIN_CPP_OPUS_ARCHIVE = opus-$(SENDSPIN_CPP_OPUS_VERSION).tar.gz

SENDSPIN_CPP_EXTRA_DOWNLOADS = \
	$(call github,bblanchon,ArduinoJson,$(SENDSPIN_CPP_ARDUINOJSON_VERSION))/$(SENDSPIN_CPP_ARDUINOJSON_ARCHIVE) \
	$(call github,esphome-libs,micro-flac,$(SENDSPIN_CPP_MICRO_FLAC_VERSION))/$(SENDSPIN_CPP_MICRO_FLAC_ARCHIVE) \
	$(call github,esphome-libs,micro-opus,$(SENDSPIN_CPP_MICRO_OPUS_VERSION))/$(SENDSPIN_CPP_MICRO_OPUS_ARCHIVE) \
	$(call github,machinezone,IXWebSocket,$(SENDSPIN_CPP_IXWEBSOCKET_VERSION))/$(SENDSPIN_CPP_IXWEBSOCKET_ARCHIVE) \
	$(call github,esphome-libs,noise-c,$(SENDSPIN_CPP_NOISE_C_VERSION))/$(SENDSPIN_CPP_NOISE_C_ARCHIVE) \
	$(call github,esphome-libs,micro-ogg-demuxer,$(SENDSPIN_CPP_MICRO_OGG_VERSION))/$(SENDSPIN_CPP_MICRO_OGG_ARCHIVE) \
	$(call github,esphome-libs,micro-ogg-demuxer,$(SENDSPIN_CPP_MICRO_FLAC_OGG_VERSION))/$(SENDSPIN_CPP_MICRO_FLAC_OGG_ARCHIVE) \
	$(call github,xiph,opus,$(SENDSPIN_CPP_OPUS_VERSION))/$(SENDSPIN_CPP_OPUS_ARCHIVE)

SENDSPIN_CPP_CONF_OPTS = \
	-DSENDSPIN_ENABLE_PLAYER=ON \
	-DSENDSPIN_ENABLE_CONTROLLER=ON \
	-DSENDSPIN_ENABLE_METADATA=ON \
	-DSENDSPIN_ENABLE_ARTWORK=OFF \
	-DSENDSPIN_ENABLE_VISUALIZER=ON \
	-DSENDSPIN_ENABLE_COLOR=ON \
	-DSENDSPIN_ENABLE_SOURCE=OFF \
	-DSENDSPIN_ENABLE_OPUS=ON \
	-DFETCHCONTENT_FULLY_DISCONNECTED=ON \
	-DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=$(@D)/third_party/arduinojson \
	-DFETCHCONTENT_SOURCE_DIR_MICRO_FLAC=$(@D)/third_party/micro-flac \
	-DFETCHCONTENT_SOURCE_DIR_MICRO_OPUS=$(@D)/third_party/micro-opus \
	-DFETCHCONTENT_SOURCE_DIR_IXWEBSOCKET=$(@D)/third_party/ixwebsocket \
	-DFETCHCONTENT_SOURCE_DIR_NOISE_C=$(@D)/third_party/noise-c \
	-DUSE_TLS=OFF \
	-DBUILD_EXAMPLES=OFF \
	-DSENDSPIN_USE_CCACHE=OFF \
	-DIXWEBSOCKET_INSTALL=OFF \
	-DBUILD_SHARED_LIBS=OFF

# Upstream builds examples only as a top-level project; drop them anyway so
# the host-only example dependencies are never configured.
define SENDSPIN_CPP_DROP_EXAMPLES
	$(Q)$(SED) '/add_subdirectory(examples/d' $(@D)/CMakeLists.txt
endef
SENDSPIN_CPP_POST_EXTRACT_HOOKS += SENDSPIN_CPP_DROP_EXAMPLES

define SENDSPIN_CPP_EXTRACT_PINNED_DEPENDENCIES
	$(INSTALL) -d $(@D)/third_party/arduinojson
	$(call suitable-extractor,$(SENDSPIN_CPP_ARDUINOJSON_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_ARDUINOJSON_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/arduinojson $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/micro-flac/lib/micro-ogg-demuxer
	$(call suitable-extractor,$(SENDSPIN_CPP_MICRO_FLAC_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_MICRO_FLAC_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-flac $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CPP_MICRO_FLAC_OGG_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_MICRO_FLAC_OGG_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-flac/lib/micro-ogg-demuxer $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/micro-opus/lib/micro-ogg-demuxer \
		$(@D)/third_party/micro-opus/lib/opus
	$(call suitable-extractor,$(SENDSPIN_CPP_MICRO_OPUS_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_MICRO_OPUS_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CPP_MICRO_OGG_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_MICRO_OGG_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus/lib/micro-ogg-demuxer $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CPP_OPUS_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_OPUS_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus/lib/opus $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/ixwebsocket $(@D)/third_party/noise-c
	$(call suitable-extractor,$(SENDSPIN_CPP_IXWEBSOCKET_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_IXWEBSOCKET_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/ixwebsocket $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CPP_NOISE_C_ARCHIVE)) \
		$(SENDSPIN_CPP_DL_DIR)/$(SENDSPIN_CPP_NOISE_C_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/noise-c $(TAR_OPTIONS) -
endef
SENDSPIN_CPP_POST_EXTRACT_HOOKS += \
	SENDSPIN_CPP_EXTRACT_PINNED_DEPENDENCIES

# sendspin-cpp has no install rules. Install the static archives, the public
# headers and a pkg-config file whose Cflags carry the role defines: the
# public headers change class layout with them, so consumers must match.
SENDSPIN_CPP_ARCHIVES = \
	libsendspin.a \
	libnoise_c.a \
	_deps/micro_flac-build/libmicro_flac.a \
	_deps/micro_flac-build/micro-ogg-demuxer/libmicro_ogg_demuxer.a \
	_deps/micro_opus-build/libmicro_opus.a \
	_deps/ixwebsocket-build/libixwebsocket.a

define SENDSPIN_CPP_INSTALL_STAGING_CMDS
	$(INSTALL) -d $(STAGING_DIR)/usr/lib/sendspin $(STAGING_DIR)/usr/include
	for a in $(SENDSPIN_CPP_ARCHIVES); do \
		$(INSTALL) -m 0644 $(@D)/$$a $(STAGING_DIR)/usr/lib/sendspin/ || exit 1; \
	done
	rm -rf $(STAGING_DIR)/usr/include/sendspin
	cp -a $(@D)/include/sendspin $(STAGING_DIR)/usr/include/sendspin
	$(INSTALL) -D -m 0644 $(SENDSPIN_CPP_PKGDIR)/sendspin-cpp.pc.in \
		$(STAGING_DIR)/usr/lib/pkgconfig/sendspin-cpp.pc
	$(SED) 's/@VERSION@/$(SENDSPIN_CPP_VERSION_NUMBER)/' \
		$(STAGING_DIR)/usr/lib/pkgconfig/sendspin-cpp.pc
endef

$(eval $(cmake-package))
