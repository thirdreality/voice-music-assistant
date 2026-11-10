# Query Buildroot's active packages, including implicitly enabled dependencies.
# Run from buildroot/ with an absolute O= and umask 0022, as in ./go rootfs.
include Makefile

.PHONY: rootfs-packages
rootfs-packages:
	@$(foreach p,$(sort $(PACKAGES) $(foreach p,$(PACKAGES) $(TARGETS_ROOTFS),$($(call UPPERCASE,$(p))_FINAL_RECURSIVE_DEPENDENCIES))), \
		$(if $(filter package/thirdreality/%,$($(call UPPERCASE,$(p))_PKGDIR)), \
			$(if $($(call UPPERCASE,$(p))_IS_VIRTUAL),, \
				printf 'ROOTFS_PACKAGE\t%s\t%s\n' '$(p)' '$($(call UPPERCASE,$(p))_DIR)';))) :
