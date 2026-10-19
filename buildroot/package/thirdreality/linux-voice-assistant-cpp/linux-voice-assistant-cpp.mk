################################################################################
#
# linux-voice-assistant-cpp
#
################################################################################

LINUX_VOICE_ASSISTANT_CPP_VERSION = 1.0.0
LINUX_VOICE_ASSISTANT_CPP_SITE = $(TOPDIR)/package/thirdreality/linux-voice-assistant-cpp
LINUX_VOICE_ASSISTANT_CPP_SITE_METHOD = local
LINUX_VOICE_ASSISTANT_CPP_LICENSE = Apache-2.0
LINUX_VOICE_ASSISTANT_CPP_LICENSE_FILES =

LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = host-protobuf protobuf json-for-modern-cpp mpv webrtc-audio-processing libcurl avahi alsa-lib tr-volume

LINUX_VOICE_ASSISTANT_CPP_CONF_OPTS =

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

define LINUX_VOICE_ASSISTANT_CPP_INSTALL_TFLITE
	$(INSTALL) -D -m 0644 \
	    $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/third_party/tflite_c/libtensorflowlite_c.so \
	    $(TARGET_DIR)/usr/lib/libtensorflowlite_c.so
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_TFLITE

define LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/sounds
	cp -f $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.wav \
	      $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/sounds/*.flac \
	      $(TARGET_DIR)/usr/share/thirdreality/sounds/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_SOUNDS

# Models for --voice-backend local.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_VOICE_MODELS
	$(INSTALL) -D -m 0644 \
	    $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/models/stt/ggml-tiny.en-q5_1.bin \
	    $(TARGET_DIR)/usr/share/thirdreality/stt/ggml-tiny.en-q5_1.bin
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/tts
	cp -rf $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/models/tts/espeak-ng-data \
	      $(TARGET_DIR)/usr/share/thirdreality/tts/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_VOICE_MODELS

$(eval $(cmake-package))
