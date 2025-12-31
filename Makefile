include $(TOPDIR)/rules.mk

# Name, version and release number
# The name and version of your package are used to define the variable to point to the build directory of your package: $(PKG_BUILD_DIR)
PKG_NAME:=fry-os-services
PKG_VERSION:=$(shell cat VERSION)
PKG_RELEASE:=1

# This is a custom variable, used below
AGENT_DIR:=apps/agent

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# Override the prepare step to copy local files
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./CMakeLists.txt $(PKG_BUILD_DIR)/
	$(CP) ./apps $(PKG_BUILD_DIR)/
	$(CP) ./lib $(PKG_BUILD_DIR)/
	$(CP) ./VERSION $(PKG_BUILD_DIR)/
endef

# Package definition; instructs on how and where our package will appear in the overall configuration menu ('make menuconfig')
define Package/fry-os-services
  SECTION:=admin
  CATEGORY:=Administration
  TITLE:=Fry OS config daemon and scripts
  DEPENDS:=+libcurl +libjson-c +libopenssl +libmosquitto-ssl +libubus +libubox +libblobmsg-json +lua
endef

# Package description; a more verbose description on what our package does
define Package/fry-os-services/description
  Services for access and accounting for fry-os
endef

# Package install instructions
# - Create the required directories
# - Install main scripts in the /usr/bin directory
# - Install init scripts in the /etc/init.d directory
# - Install app files in the /etc/fry-os-services directory
define Package/fry-os-services/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_DIR) $(1)/etc/fry-os-services
	$(INSTALL_DIR) $(1)/etc/fry-agent
	$(INSTALL_DIR) $(1)/etc/fry-agent/scripts
	$(INSTALL_DIR) $(1)/etc/fry-agent/data
	$(INSTALL_DIR) $(1)/etc/fry-agent/data/did-key
	$(INSTALL_DIR) $(1)/etc/fry-config
	$(INSTALL_DIR) $(1)/etc/fry-config/scripts
	$(INSTALL_DIR) $(1)/etc/fry-config/hashes
	$(INSTALL_DIR) $(1)/etc/fry-config/rollback
	$(INSTALL_DIR) $(1)/etc/fry-collector

	# Install the three new binaries
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fry-agent $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fry-config $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fry-collector $(1)/usr/bin/

	# Install init script and config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/agent/scripts/openwrt/fry-agent.init $(1)/etc/init.d/fry-agent
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/apps/agent/scripts/openwrt/fry-agent.config $(1)/etc/config/fry-agent

	# Install fry-config init script and config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/config/scripts/openwrt/fry-config.init $(1)/etc/init.d/fry-config
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/apps/config/scripts/openwrt/fry-config.config $(1)/etc/config/fry-config

	# Install collector init script and config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/collector/scripts/openwrt/fry-collector.init $(1)/etc/init.d/fry-collector
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/apps/collector/scripts/openwrt/fry-collector.config $(1)/etc/config/fry-collector

	# Install all scripts from the openwrt directory
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/agent/scripts/openwrt/*.sh $(1)/etc/fry-agent/scripts/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/agent/scripts/openwrt/*.lua $(1)/etc/fry-agent/scripts/

	# Install fry-config scripts
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apps/config/scripts/openwrt/*.uc $(1)/etc/fry-config/scripts/

	# Install VERSION file
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/VERSION $(1)/etc/fry-os-services/VERSION
endef

define Package/fry-os-services/conffiles
/etc/config/fry-agent
/etc/config/fry-config
/etc/config/fry-collector
endef

# This command is always the last, it uses the definitions and variables we give above in order to get the job done
$(eval $(call BuildPackage,fry-os-services))
