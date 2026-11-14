include $(TOPDIR)/rules.mk

# ODAC admission backend: the pinned upstream openNDS release, packaged with a
# gated init script and without the stock install-time network changes.
# patches/100-isolate-shared-dns.patch removes daemon DNS/DHCP lifecycle
# actions; odac-opennds-check rejects configurations needing those features.
#
# The stock routing/opennds 10.3.1-r2 recipe starts its init script from the
# package postinst. That start_service has no "enabled" check and always runs
# "libopennds.sh ipv6_routing block" (uci set network.wan6.proto=none plus a
# network reload); its uci-defaults rewrites and commits firewall.opennds.
# Neither can be suppressed by configuration, so ODAC cannot depend on it.
PKG_NAME:=odac-opennds
PKG_VERSION:=10.3.1
PKG_RELEASE:=2

# Same source archive and hash as the official feed recipe.
PKG_SOURCE:=opennds-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/opennds/opennds/tar.gz/v$(PKG_VERSION)?
PKG_HASH:=f82fe0fa2e4e8ab66abf33cae7cb20e79661c8a183af82eefa11307e9c66968d
PKG_BUILD_DIR:=$(BUILD_DIR)/openNDS-$(PKG_VERSION)
PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING

PKG_FIXUP:=autoreconf
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/odac-opennds
  SECTION:=net
  CATEGORY:=Network
  TITLE:=openNDS admission backend for ODAC (gated, no WAN6 changes)
  URL:=https://github.com/opennds/opennds
  DEPENDS:=+libmicrohttpd-no-ssl +nftables-json +uci
  # Same binaries, configuration path and nftables tables as the stock
  # package. With CONFIG_USE_APK, CONFLICTS only constrains Kconfig selection
  # (package-metadata.pl mconf_conflicts); it is not written into the APK.
  # On a router, both packages own /usr/bin/opennds and /etc/init.d/opennds,
  # so APK should refuse the file overwrite; that has not been verified on a
  # real APK database yet. odac-preflight also refuses a stock opennds.
  CONFLICTS:=opennds nodogsplash
endef

define Package/odac-opennds/description
  openNDS $(PKG_VERSION) for the OpenWrt Device Access Controller. Installs
  disabled. The service starts only after ODAC took ownership and installed
  its fail-closed guard for the controlled interface. It never changes WAN6,
  never inserts rules into the fw4 table and ships no uci-defaults.
endef

define Package/odac-opennds/conffiles
/etc/config/opennds
endef

define Package/odac-opennds/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/usr/lib/opennds $(1)/etc/opennds/htdocs/images
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d $(1)/usr/libexec
	$(INSTALL_BIN) ./files/usr/libexec/odac-opennds-check $(1)/usr/libexec/odac-opennds-check
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/opennds $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ndsctl $(1)/usr/bin/
	$(CP) $(PKG_BUILD_DIR)/resources/splash.css $(1)/etc/opennds/htdocs/
	$(CP) $(PKG_BUILD_DIR)/resources/splash.jpg $(1)/etc/opennds/htdocs/images/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/binauth/binauth_log.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/binauth/custombinauth.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/libopennds.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/PreAuth/theme_click-to-continue-basic.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/PreAuth/theme_click-to-continue-custom-placeholders.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/PreAuth/theme_user-email-login-basic.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/PreAuth/theme_user-email-login-custom-placeholders.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/get_client_interface.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/client_params.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/authmon.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/dnsconfig.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/download_resources.sh $(1)/usr/lib/opennds/
	$(CP) $(PKG_BUILD_DIR)/forward_authentication_service/libs/post-request.php $(1)/usr/lib/opennds/
	# Gated replacements for the stock init script and configuration. The
	# stock uci-defaults (40_opennds) and fw4 restart hook are not shipped.
	$(INSTALL_CONF) ./files/etc/config/opennds $(1)/etc/config/opennds
	$(INSTALL_BIN) ./files/etc/init.d/opennds $(1)/etc/init.d/opennds
endef

$(eval $(call BuildPackage,odac-opennds))
