include $(TOPDIR)/rules.mk

PKG_NAME:=device-access
PKG_VERSION:=0.1.0_rc1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/device-access
	SECTION:=net
	CATEGORY:=Network
	TITLE:=OpenWrt Device Access Controller
	# TLS: this package does NOT depend on any libustream variant.
	#
	# The three upstream variants (libustream-mbedtls, -openssl, -wolfssl)
	# are mutually exclusive (CONFLICTS in package/libs/ustream-ssl) and every
	# one of them installs the SAME file /lib/libustream-ssl.so. A hard
	# dependency on one variant therefore makes APK replace the operator's
	# existing provider, and the replacement removes /lib/libustream-ssl.so
	# in between. uhttpd and uclient-fetch resolve the provider at runtime
	# through dlopen("libustream-ssl.so"), so a missing file disables HTTPS
	# for LuCI and for the package manager.
	#
	# ODAC's own code neither links nor dlopens libustream (see
	# device-access/CMakeLists.txt): it links json-c, sqlite3 and argon2 and
	# uses curl, openssl and ndsctl as separate processes. The TLS provider
	# of the system is therefore left entirely alone.
	#
	# The portal needs uhttpd with TLS, so the presence of a TLS provider is
	# verified at activation time by odac-preflight instead of being forced
	# at install time. A missing provider is a hard, explicit error, never a
	# silent downgrade to plain HTTP.
	#
	# Admission: odac-opennds, not the stock opennds. The stock package is
	# started by its own postinst before any ODAC script can run, and its
	# init script changes network.wan6 and may capture br-lan. odac-opennds
	# is the same pinned openNDS release with a gated init script, so this
	# dependency installs inert. It ships the same files as stock opennds,
	# so it cannot silently replace an existing instance; preflight refuses
	# a stock opennds as well.
	DEPENDS:=+libsqlite3 +libjson-c +libargon2 +ca-bundle +uhttpd +odac-opennds +firewall4 +nftables-json +conntrack +rpcd +uci +ubus +jshn +jsonfilter +curl +openssl-util
endef

define Package/device-access/description
	Native account and per-device admission controller. Disabled until explicit
	isolated-network setup. Requires the companion libargon2 package recipe.
endef

define Package/device-access/conffiles
/etc/config/odac
/etc/odac/
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(CP) ./CMakeLists.txt $(PKG_BUILD_DIR)/
endef

define Build/Configure
	$(call Build/Configure/Default)
endef

define Package/device-access/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/www/odac/cgi-bin $(1)/www/odac-landing/cgi-bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/odacd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/odacctl $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/odac-portal $(1)/www/odac/cgi-bin/odac
	$(LN) /www/odac/cgi-bin/odac $(1)/www/odac-landing/cgi-bin/landing
	$(CP) ./files/. $(1)/
	chmod 0755 $(1)/etc/init.d/odac $(1)/etc/odac/firewall-include $(1)/usr/libexec/odac-* $(1)/usr/libexec/rpcd/odac
endef

define Package/device-access/prerm
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] || /usr/libexec/odac-remove --retain-guard
endef

$(eval $(call BuildPackage,device-access))
