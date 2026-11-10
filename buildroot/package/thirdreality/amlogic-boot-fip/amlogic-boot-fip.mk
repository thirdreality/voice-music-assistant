################################################################################
#
# amlogic-boot-fip-e
#
################################################################################
AMLOGIC_BOOT_FIP_VERSION = f5cf6a7a78
AMLOGIC_BOOT_FIP_SITE = $(TOPDIR)/package/thirdreality/amlogic-boot-fip/src
AMLOGIC_BOOT_FIP_SITE_METHOD = local

AMLOGIC_BOOT_FIP_INSTALL_IMAGES = YES
AMLOGIC_BOOT_FIP_DEPENDENCIES = uboot

AMLOGIC_BOOT_FIP_LICENSE = PROPRIETARY
AMLOGIC_BOOT_FIP_REDISTRIBUTE = NO

AMLOGIC_BOOT_BINS += u-boot.bin u-boot.bin.sd.bin u-boot.bin.usb.bl2 u-boot.bin.usb.tpl

define AMLOGIC_BOOT_FIP_BUILD_CMDS
	mkdir -p $(@D)/fip
	# images/u-boot.bin is overwritten by this package's complete FIP. Reading
	# it on a rebuild would wrap that FIP as BL33 instead of using raw U-Boot.
	cp "$(UBOOT_DIR)/build/u-boot.bin" "$(@D)/fip/bl33.bin"
	cd "$(@D)"; ./build-fip.sh $(call qstrip,$(BR2_PACKAGE_AMLOGIC_BOOT_FIP_BOARD)) $(@D)/fip/bl33.bin $(@D)/fip
endef

ifeq ($(BR2_PACKAGE_AMLOGIC_BOOT_FIP),y)
ifeq ($(call qstrip,$(BR2_PACKAGE_AMLOGIC_BOOT_FIP_BOARD)),)
$(error No board u-boot firmware config name specified, check your BR2_PACKAGE_AMLOGIC_BOOT_FIP_BOARD setting)
endif
endif

define AMLOGIC_BOOT_FIP_INSTALL_IMAGES_CMDS
	$(foreach f,$(AMLOGIC_BOOT_BINS), \
		cp -dpf "$(@D)/fip/$(f)" "$(BINARIES_DIR)/";)
endef

$(eval $(generic-package))
