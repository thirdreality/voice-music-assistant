################################################################################
#
# linux-voice-assistant-cpp
#
# C++ rewrite of the linux-voice-assistant package. Feature-complete
# voice assistant for the ThirdReality speaker. See
# doc/linux-voice-assistant-cpp-refactor-plan.md.
#
################################################################################

LINUX_VOICE_ASSISTANT_CPP_VERSION = 1.0.0
LINUX_VOICE_ASSISTANT_CPP_SITE = $(TOPDIR)/package/thirdreality/linux-voice-assistant-cpp
LINUX_VOICE_ASSISTANT_CPP_SITE_METHOD = local
LINUX_VOICE_ASSISTANT_CPP_LICENSE = Apache-2.0
LINUX_VOICE_ASSISTANT_CPP_LICENSE_FILES =

# host-protobuf provides protoc for build-time .proto -> .cc/.h codegen.
# protobuf provides libprotobuf at runtime (full, not lite — api.proto
# does not set optimize_for = LITE_RUNTIME).
# alsa-lib is used by the dev tool aec_loopback_test (and, in a follow-up
# change, will back the production AudioCapture's ALSA backend for
# hardware-loopback AEC). Keep it explicit even though pulseaudio /
# webrtc-audio-processing already pull it transitively.
LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = host-protobuf protobuf json-for-modern-cpp mpv pulseaudio webrtc-audio-processing libcurl avahi alsa-lib

LINUX_VOICE_ASSISTANT_CPP_CONF_OPTS =

# Install vendored wake-word models to /usr/share/thirdreality/wakewords/.
# Source layout mirrors the target: wakewords/{microwakeword,openwakeword}/.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_WAKEWORDS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/wakewords/microwakeword
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/wakewords/microwakeword/*.tflite \
	      $(TARGET_DIR)/usr/share/thirdreality/wakewords/microwakeword/
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/wakewords/microwakeword/*.json \
	      $(TARGET_DIR)/usr/share/thirdreality/wakewords/microwakeword/
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/wakewords/openwakeword
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/wakewords/openwakeword/*.tflite \
	      $(TARGET_DIR)/usr/share/thirdreality/wakewords/openwakeword/
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/wakewords/openwakeword/*.json \
	      $(TARGET_DIR)/usr/share/thirdreality/wakewords/openwakeword/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_WAKEWORDS

# Install our own copy of libtensorflowlite_c.so to /usr/lib/. Phase 8
# removes pyopen-wakeword + python-pymicro-wakeword from defconfig;
# this hook ensures the binary doesn't depend on those packages'
# python site-packages directories existing.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_TFLITE
	$(INSTALL) -D -m 0644 \
	    $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/third_party/tflite_c/libtensorflowlite_c.so \
	    $(TARGET_DIR)/usr/lib/libtensorflowlite_c.so
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_TFLITE

# Install the vendored UI sound effects (thinking/processing,
# wake-word feedback, timer ring, mute toggle). Originally lived in
# the Python LVA package's `sounds/` directory; ship our own copy so
# we don't depend on that package being selected.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/sounds
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.wav \
	      $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.flac \
	      $(TARGET_DIR)/usr/share/thirdreality/sounds/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS

$(eval $(cmake-package))
