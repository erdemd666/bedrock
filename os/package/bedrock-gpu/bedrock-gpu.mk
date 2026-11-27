################################################################################
#
# bedrock-gpu
#
################################################################################

BEDROCK_GPU_VERSION = local
BEDROCK_GPU_SITE = $(BR2_EXTERNAL_BEDROCK_PATH)/../tools/gpu
BEDROCK_GPU_SITE_METHOD = local
BEDROCK_GPU_LICENSE = MIT
BEDROCK_GPU_DEPENDENCIES = libegl libgles libgbm

define BEDROCK_GPU_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra \
		-o $(@D)/bedrock-gpu $(@D)/bedrock-gpu.c -lEGL -lGLESv2 -lgbm -lm
endef

define BEDROCK_GPU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bedrock-gpu $(TARGET_DIR)/usr/bin/bedrock-gpu
endef

$(eval $(generic-package))
