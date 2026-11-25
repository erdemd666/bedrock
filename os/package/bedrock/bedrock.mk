################################################################################
#
# bedrock
#
################################################################################

BEDROCK_VERSION = local
BEDROCK_SITE = $(BR2_EXTERNAL_BEDROCK_PATH)/../tools
BEDROCK_SITE_METHOD = local
BEDROCK_LICENSE = MIT

# Name of the binary tools/Makefile produces; installed as /usr/bin/bedrock.
BEDROCK_BIN = bedrock

# -B: the rsynced tree may carry a binary from another build.
define BEDROCK_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CC="$(TARGET_CC)" $(BEDROCK_BIN)
endef

define BEDROCK_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/$(BEDROCK_BIN) $(TARGET_DIR)/usr/bin/bedrock
endef

$(eval $(generic-package))
