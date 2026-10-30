################################################################################
#
# thirdreality ha speaker project
#
################################################################################

TR_PROJ_HA_SPEAKER_VERSION = 0.1
TR_PROJ_HA_SPEAKER_SITE = $(TOPDIR)/package/thirdreality/tr-proj-ha-speaker
TR_PROJ_HA_SPEAKER_SITE_METHOD = local

TR_PROJ_HA_SPEAKER_INSTALL_TARGET = YES
TR_PROJ_HA_SPEAKER_DEPENDENCIES = tr-volume jq

REALITY_DIR = $(TARGET_DIR)/usr/share/thirdreality

ifdef SPEAKER_FIRMWARE_VERSION
	IMAGE_VERSION=$(SPEAKER_FIRMWARE_VERSION)
else
	IMAGE_VERSION=$(shell date "+0.%m.%d")
endif

define TR_PROJ_HA_SPEAKER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/device.json ${REALITY_DIR}/conf/device.json
	
	$(INSTALL) -D -m 0644 $(@D)/audio/setup_mode.wav ${REALITY_DIR}/audio/setup_mode.wav
	$(INSTALL) -D -m 0644 $(@D)/audio/change_wifi.wav ${REALITY_DIR}/audio/change_wifi.wav
	$(INSTALL) -D -m 0644 $(@D)/audio/not_ready.wav ${REALITY_DIR}/audio/not_ready.wav
	$(INSTALL) -D -m 0644 $(@D)/audio/factory_reset.wav ${REALITY_DIR}/audio/factory_reset.wav
	$(INSTALL) -D -m 0644 $(@D)/audio/ready_to_connect_ha.wav ${REALITY_DIR}/audio/ready_to_connect_ha.wav

	$(INSTALL) -D -m 0755 $(@D)/script/setup_env.sh ${REALITY_DIR}/script/setup_env.sh
	$(INSTALL) -D -m 0755 $(@D)/script/wifi_connect ${REALITY_DIR}/script/wifi_connect
	$(INSTALL) -D -m 0755 $(@D)/script/netmonitor ${REALITY_DIR}/script/netmonitor
	$(INSTALL) -D -m 0755 $(@D)/script/network-policy.sh ${REALITY_DIR}/script/network-policy.sh
	$(INSTALL) -D -m 0755 $(@D)/script/ntp-apply ${REALITY_DIR}/script/ntp-apply
	$(INSTALL) -D -m 0755 $(@D)/script/S99ha-speaker $(TARGET_DIR)/etc/init.d/S99ha-speaker

	@echo "firmwareVersion is $(IMAGE_VERSION)"
	@jq '.device.firmwareVersion = "$(IMAGE_VERSION)"' $(TARGET_DIR)/usr/share/thirdreality/conf/device.json > \
		tmp.$$.json && mv tmp.$$.json $(TARGET_DIR)/usr/share/thirdreality/conf/device.json

endef

$(eval $(generic-package))
