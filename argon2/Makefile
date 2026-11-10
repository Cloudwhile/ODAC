include $(TOPDIR)/rules.mk

PKG_NAME:=argon2
PKG_VERSION:=20190702
PKG_RELEASE:=1
PKG_SOURCE:=phc-winner-argon2-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/P-H-C/phc-winner-argon2/tar.gz/refs/tags/$(PKG_VERSION)?
PKG_HASH:=daf972a89577f8772602bf2eb38b6a3dd3d922bf5724d45e7f9589b5e830442c
PKG_BUILD_DIR:=$(BUILD_DIR)/phc-winner-argon2-$(PKG_VERSION)
PKG_LICENSE:=CC0-1.0 OR Apache-2.0
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/libargon2
	SECTION:=libs
	CATEGORY:=Libraries
	TITLE:=Argon2 password hashing reference library
	URL:=https://github.com/P-H-C/phc-winner-argon2
endef

# Compile the portable reference implementation explicitly: never -march=native.
define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c99 -fPIC -shared \
	 -fvisibility=hidden -DA2_VISCTL=1 -DARGON2_NO_THREADS -I$(PKG_BUILD_DIR)/include -I$(PKG_BUILD_DIR)/src \
	 $(PKG_BUILD_DIR)/src/argon2.c $(PKG_BUILD_DIR)/src/core.c \
	 $(PKG_BUILD_DIR)/src/blake2/blake2b.c $(PKG_BUILD_DIR)/src/thread.c \
	 $(PKG_BUILD_DIR)/src/encoding.c $(PKG_BUILD_DIR)/src/ref.c \
	 $(TARGET_LDFLAGS) -Wl,-soname,libargon2.so.1 -o $(PKG_BUILD_DIR)/libargon2.so.1
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/include/argon2.h $(1)/usr/include/
	$(CP) $(PKG_BUILD_DIR)/libargon2.so.1 $(1)/usr/lib/
	$(LN) libargon2.so.1 $(1)/usr/lib/libargon2.so
endef

define Package/libargon2/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/libargon2.so.1 $(1)/usr/lib/
endef

$(eval $(call BuildPackage,libargon2))
