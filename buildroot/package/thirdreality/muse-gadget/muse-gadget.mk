################################################################################
#
# muse-gadget
#
################################################################################

MUSE_GADGET_VERSION = 0.1.0
MUSE_GADGET_SITE = $(TOPDIR)/package/thirdreality/muse-gadget
MUSE_GADGET_SITE_METHOD = local
MUSE_GADGET_LICENSE = Apache-2.0
MUSE_GADGET_DEPENDENCIES = alsa-lib openssl libcurl json-for-modern-cpp pulseaudio

MUSE_GADGET_CONF_OPTS = -DMUSE_GADGET_VERSION=$(MUSE_GADGET_VERSION)

define MUSE_GADGET_INSTALL_INIT
	$(INSTALL) -D -m 0755 $(MUSE_GADGET_PKGDIR)/script/S98muse-gadget \
		$(TARGET_DIR)/etc/init.d/S98muse-gadget
endef
MUSE_GADGET_POST_INSTALL_TARGET_HOOKS += MUSE_GADGET_INSTALL_INIT

$(eval $(cmake-package))
