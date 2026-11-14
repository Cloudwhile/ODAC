include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-device-access
PKG_VERSION:=0.1.0_rc2
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=ODAC Developers
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

LUCI_TITLE:=LuCI support for Device Access Controller (ODAC)
LUCI_DEPENDS:=+luci-base +rpcd +device-access

define Package/luci-app-device-access
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=$(LUCI_TITLE)
  DEPENDS:=$(LUCI_DEPENDS)
  PKGARCH:=all
endef

define Package/luci-app-device-access/description
  Native LuCI management interface for the OpenWrt Device Access Controller.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./htdocs $(PKG_BUILD_DIR)/
	$(CP) ./root $(PKG_BUILD_DIR)/
endef

define Build/Compile
endef

define Package/luci-app-device-access/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d
	cp -pR $(PKG_BUILD_DIR)/htdocs/luci-static/. $(1)/www/luci-static/
	cp -pR $(PKG_BUILD_DIR)/root/usr/. $(1)/usr/
endef

define Package/luci-app-device-access/postinst
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] || {
	rm -f /tmp/luci-indexcache
	/etc/init.d/rpcd reload 2>/dev/null
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-device-access))
