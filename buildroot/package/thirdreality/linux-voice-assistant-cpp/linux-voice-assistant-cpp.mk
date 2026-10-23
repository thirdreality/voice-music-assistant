################################################################################
#
# linux-voice-assistant-cpp
#
################################################################################

LINUX_VOICE_ASSISTANT_CPP_VERSION = 1.0.0
LINUX_VOICE_ASSISTANT_CPP_SITE = $(TOPDIR)/package/thirdreality/linux-voice-assistant-cpp
LINUX_VOICE_ASSISTANT_CPP_SITE_METHOD = local
# LVA sources are Apache-2.0; espeak-ng and the sanoTTS runtime linked into
# liblva_speech.so are GPL-3.0+ (see THIRD_PARTY_NOTICES.md).
LINUX_VOICE_ASSISTANT_CPP_LICENSE = Apache-2.0, GPL-3.0+ (espeak-ng, sanoTTS), MIT (whisper.cpp), BSD-3-Clause (kissfft)
LINUX_VOICE_ASSISTANT_CPP_LICENSE_FILES = \
	THIRD_PARTY_NOTICES.md \
	third_party/espeak-ng/COPYING \
	third_party/sanotts/LICENSE \
	third_party/whisper.cpp/LICENSE \
	third_party/microfrontend/LICENSE \
	third_party/microfrontend/LICENSE.kissfft

LINUX_VOICE_ASSISTANT_CPP_DEPENDENCIES = host-protobuf protobuf json-for-modern-cpp mpv webrtc-audio-processing libcurl avahi alsa-lib tr-volume sendspin-cpp pulseaudio

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

# TTS data for the local voice backend. The STT model is not installed: LVA
# downloads it to /data/models/stt when the backend is first selected.
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_VOICE_MODELS
	$(INSTALL) -d $(TARGET_DIR)/usr/share/thirdreality/tts
	cp -rf $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/models/tts/espeak-ng-data \
	      $(TARGET_DIR)/usr/share/thirdreality/tts/
	cp -rf $(LINUX_VOICE_ASSISTANT_CPP_PKGDIR)/models/tts/amy \
	      $(TARGET_DIR)/usr/share/thirdreality/tts/
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_VOICE_MODELS

# Licence notices on the device (THIRD_PARTY_NOTICES.md).
LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR = \
	$(TARGET_DIR)/usr/share/thirdreality/licenses/linux-voice-assistant-cpp
define LINUX_VOICE_ASSISTANT_CPP_INSTALL_LICENSES
	$(INSTALL) -D -m 0644 $(@D)/THIRD_PARTY_NOTICES.md \
	    $(LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR)/THIRD_PARTY_NOTICES.md
	$(INSTALL) -D -m 0644 $(@D)/third_party/espeak-ng/COPYING \
	    $(LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR)/GPL-3.0.txt
	$(INSTALL) -D -m 0644 $(@D)/third_party/whisper.cpp/LICENSE \
	    $(LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR)/MIT-whisper.cpp.txt
	$(INSTALL) -D -m 0644 $(@D)/third_party/microfrontend/LICENSE \
	    $(LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR)/Apache-2.0.txt
	$(INSTALL) -D -m 0644 $(@D)/third_party/microfrontend/LICENSE.kissfft \
	    $(LINUX_VOICE_ASSISTANT_CPP_LICENSE_DIR)/BSD-3-Clause-kissfft.txt
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_INSTALL_LICENSES

# _sendspin._tcp is published by LVA itself while the Sendspin client runs
# (MdnsPublisher). Drop the static file older images installed.
define LINUX_VOICE_ASSISTANT_CPP_REMOVE_STATIC_SENDSPIN_SERVICE
	rm -f $(TARGET_DIR)/etc/avahi/services/sendspin.service
endef
LINUX_VOICE_ASSISTANT_CPP_POST_INSTALL_TARGET_HOOKS += \
	LINUX_VOICE_ASSISTANT_CPP_REMOVE_STATIC_SENDSPIN_SERVICE

$(eval $(cmake-package))
