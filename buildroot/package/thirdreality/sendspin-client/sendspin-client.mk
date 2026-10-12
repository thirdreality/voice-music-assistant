################################################################################
#
# sendspin-client
#
# Sendspin synchronized audio streaming client for ThirdReality speaker.
# Builds sendspin-cpp library and a PulseAudio-based client application.
# mDNS advertisement via avahi-daemon service file.
#
################################################################################

SENDSPIN_CLIENT_VERSION = v0.9.3
SENDSPIN_CLIENT_VERSION_NUMBER = $(patsubst v%,%,$(SENDSPIN_CLIENT_VERSION))
SENDSPIN_CLIENT_SITE = $(call github,Sendspin,sendspin-cpp,$(SENDSPIN_CLIENT_VERSION))
SENDSPIN_CLIENT_LICENSE = Apache-2.0, MIT, BSD-3-Clause
SENDSPIN_CLIENT_LICENSE_FILES = \
	LICENSE \
	third_party/arduinojson/LICENSE.txt \
	third_party/micro-flac/LICENSE \
	third_party/micro-flac/lib/micro-ogg-demuxer/LICENSE \
	third_party/micro-opus/LICENSE \
	third_party/micro-opus/lib/opus/COPYING \
	third_party/ixwebsocket/LICENSE.txt \
	third_party/noise-c/COPYING
SENDSPIN_CLIENT_DEPENDENCIES = pulseaudio avahi tr-volume

SENDSPIN_CLIENT_ARDUINOJSON_VERSION = 32520135092970120a5ac165cf45f48e658c421d
SENDSPIN_CLIENT_MICRO_FLAC_VERSION = 9f8bfe5c9ee46cea175084b49ae8ac95545705b5
SENDSPIN_CLIENT_MICRO_OPUS_VERSION = 3e9ce44c56ab8007261d68c511f0162599542aa2
SENDSPIN_CLIENT_IXWEBSOCKET_VERSION = 64fae7676bd8fe31f7cb4bcde7a6841892dad65e
SENDSPIN_CLIENT_NOISE_C_VERSION = a1e08809a1b8f65cd91765ba7d68a6d00648ad61
SENDSPIN_CLIENT_MICRO_OGG_VERSION = 865ad9d831e7dc76bb9c142607bae33fc75648e7
SENDSPIN_CLIENT_OPUS_VERSION = 22244de5a79bd1d6d623c32e72bf1954b56235be

SENDSPIN_CLIENT_ARDUINOJSON_ARCHIVE = arduinojson-$(SENDSPIN_CLIENT_ARDUINOJSON_VERSION).tar.gz
SENDSPIN_CLIENT_MICRO_FLAC_ARCHIVE = micro-flac-$(SENDSPIN_CLIENT_MICRO_FLAC_VERSION).tar.gz
SENDSPIN_CLIENT_MICRO_OPUS_ARCHIVE = micro-opus-$(SENDSPIN_CLIENT_MICRO_OPUS_VERSION).tar.gz
SENDSPIN_CLIENT_IXWEBSOCKET_ARCHIVE = ixwebsocket-$(SENDSPIN_CLIENT_IXWEBSOCKET_VERSION).tar.gz
SENDSPIN_CLIENT_NOISE_C_ARCHIVE = noise-c-$(SENDSPIN_CLIENT_NOISE_C_VERSION).tar.gz
SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE = micro-ogg-demuxer-$(SENDSPIN_CLIENT_MICRO_OGG_VERSION).tar.gz
SENDSPIN_CLIENT_OPUS_ARCHIVE = opus-$(SENDSPIN_CLIENT_OPUS_VERSION).tar.gz

SENDSPIN_CLIENT_EXTRA_DOWNLOADS = \
	$(call github,bblanchon,ArduinoJson,$(SENDSPIN_CLIENT_ARDUINOJSON_VERSION))/$(SENDSPIN_CLIENT_ARDUINOJSON_ARCHIVE) \
	$(call github,esphome-libs,micro-flac,$(SENDSPIN_CLIENT_MICRO_FLAC_VERSION))/$(SENDSPIN_CLIENT_MICRO_FLAC_ARCHIVE) \
	$(call github,esphome-libs,micro-opus,$(SENDSPIN_CLIENT_MICRO_OPUS_VERSION))/$(SENDSPIN_CLIENT_MICRO_OPUS_ARCHIVE) \
	$(call github,machinezone,IXWebSocket,$(SENDSPIN_CLIENT_IXWEBSOCKET_VERSION))/$(SENDSPIN_CLIENT_IXWEBSOCKET_ARCHIVE) \
	$(call github,esphome-libs,noise-c,$(SENDSPIN_CLIENT_NOISE_C_VERSION))/$(SENDSPIN_CLIENT_NOISE_C_ARCHIVE) \
	$(call github,esphome-libs,micro-ogg-demuxer,$(SENDSPIN_CLIENT_MICRO_OGG_VERSION))/$(SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE) \
	$(call github,xiph,opus,$(SENDSPIN_CLIENT_OPUS_VERSION))/$(SENDSPIN_CLIENT_OPUS_ARCHIVE)

SENDSPIN_CLIENT_CONF_OPTS = \
	-DSENDSPIN_CLIENT_PROJECT_VERSION=$(SENDSPIN_CLIENT_VERSION_NUMBER) \
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
	-DBUILD_SHARED_LIBS=OFF

# Restructure: move upstream sendspin-cpp source into a subdirectory and
# overlay our wrapper CMakeLists.txt + client source at the build dir root.
#
# This must happen in POST_EXTRACT (not PRE_BUILD or PRE_CONFIGURE),
# because cmake configure runs on $(@D) and needs to see CMakeLists.txt
# already in place. Hooks earlier than configure don't fire on a plain
# `<pkg>-rebuild`; if you edit sendspin-client.cpp and want to re-pick it
# up without a full extract, use `<pkg>-reconfigure` (which also doesn't
# re-cp here, so dirclean is the safe fallback) or just dirclean the
# package build dir.
define SENDSPIN_CLIENT_RESTRUCTURE
	$(Q)mkdir -p $(@D)/_sendspin_src
	$(Q)mv $(@D)/CMakeLists.txt $(@D)/cmake $(@D)/include $(@D)/src \
	       $(@D)/examples $(@D)/Kconfig $(@D)/idf_component.yml \
	       $(@D)/_sendspin_src/ 2>/dev/null || true
	$(Q)mv $(@D)/_sendspin_src $(@D)/sendspin-cpp
	$(Q)$(SED) '/add_subdirectory(examples/d' $(@D)/sendspin-cpp/CMakeLists.txt
	$(Q)cp $(SENDSPIN_CLIENT_PKGDIR)/CMakeLists.txt $(@D)/
	$(Q)cp $(SENDSPIN_CLIENT_PKGDIR)/sendspin-client.cpp $(@D)/
endef
SENDSPIN_CLIENT_POST_EXTRACT_HOOKS += SENDSPIN_CLIENT_RESTRUCTURE

define SENDSPIN_CLIENT_EXTRACT_PINNED_DEPENDENCIES
	$(INSTALL) -d $(@D)/third_party/arduinojson
	$(call suitable-extractor,$(SENDSPIN_CLIENT_ARDUINOJSON_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_ARDUINOJSON_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/arduinojson $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/micro-flac/lib/micro-ogg-demuxer
	$(call suitable-extractor,$(SENDSPIN_CLIENT_MICRO_FLAC_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_MICRO_FLAC_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-flac $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-flac/lib/micro-ogg-demuxer $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/micro-opus/lib/micro-ogg-demuxer \
		$(@D)/third_party/micro-opus/lib/opus
	$(call suitable-extractor,$(SENDSPIN_CLIENT_MICRO_OPUS_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_MICRO_OPUS_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_MICRO_OGG_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus/lib/micro-ogg-demuxer $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CLIENT_OPUS_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_OPUS_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/micro-opus/lib/opus $(TAR_OPTIONS) -
	$(INSTALL) -d $(@D)/third_party/ixwebsocket $(@D)/third_party/noise-c
	$(call suitable-extractor,$(SENDSPIN_CLIENT_IXWEBSOCKET_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_IXWEBSOCKET_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/ixwebsocket $(TAR_OPTIONS) -
	$(call suitable-extractor,$(SENDSPIN_CLIENT_NOISE_C_ARCHIVE)) \
		$(SENDSPIN_CLIENT_DL_DIR)/$(SENDSPIN_CLIENT_NOISE_C_ARCHIVE) | \
		$(TAR) --strip-components=1 -C $(@D)/third_party/noise-c $(TAR_OPTIONS) -
endef
SENDSPIN_CLIENT_POST_EXTRACT_HOOKS += \
	SENDSPIN_CLIENT_EXTRACT_PINNED_DEPENDENCIES

# Install avahi service file for mDNS advertisement
define SENDSPIN_CLIENT_INSTALL_AVAHI_SERVICE
	$(INSTALL) -D -m 0644 $(SENDSPIN_CLIENT_PKGDIR)/sendspin.service \
		$(TARGET_DIR)/etc/avahi/services/sendspin.service
endef
SENDSPIN_CLIENT_POST_INSTALL_TARGET_HOOKS += SENDSPIN_CLIENT_INSTALL_AVAHI_SERVICE

$(eval $(cmake-package))
