# 生产故障根因分析：安装 ODAC 后 LuCI 404、SSH 失联、HTTPS 失效

本文记录对已发生故障的分析。每条结论都标注了证据来源；未经证实的内容明确标为
「待验证」，不写成已确认的根因。

## 故障环境

| 项目 | 值 | 证据 |
| --- | --- | --- |
| 硬件 | Cudy TR3000 | 故障报告 |
| 系统 | OpenWrt 25.12.2 | 故障报告 |
| 架构 | aarch64_cortex-a53 | 故障报告 |
| Target | mediatek/filogic | 官方 25.12.2 发布目录 `targets/mediatek/filogic/` 存在 |
| 安装包 | `device-access-0.1.0_rc1-r1-aarch64_cortex-a53.apk` | 故障报告 |
| 现象 | 安装后 LuCI 返回 404、SSH 无法连接、HTTPS 软件包下载异常 | 故障报告 |

## 分析方法

分析基于以下**可复现的源码证据**，而不是对故障时序的推测：

- OpenWrt 25.12.2 的 `package/base-files/files/lib/functions.sh`（`default_postinst`）
- OpenWrt 25.12.2 的 `feeds/base/libs/ustream-ssl/Makefile`
- OpenWrt 25.12.2 的 `feeds/base/network/services/uhttpd/Makefile`
- OpenWrt 25.12.2 官方发布索引（`packages/<arch>/base.adb` 等）的实际元数据
- openNDS 10.3.1 的发布源码（`opennds/opennds` v10.3.1，PKG_HASH 与官方配方一致）
- OpenWrt 25.12.2 官方 x86/64 rootfs 的实际包清单

---

## 根因 1：openNDS 作为依赖被安装后自动启动并接管管理 LAN

### 证据链

1. **上游 `default_postinst` 会 enable 并 start 每个新装包的 init 脚本。**
   `package/base-files/files/lib/functions.sh`：

   ```sh
   for i in $(grep -s "^/etc/init.d/" "$filelist"); do
       if [ "$PKG_UPGRADE" != "1" ]; then
           "$i" enable
       fi
       "$i" start
   done
   ```

   openNDS 的官方配方安装了 `/etc/init.d/opennds`，因此它作为 `device-access`
   的依赖被安装时**必然被 enable 并 start**。

2. **openNDS 用自己的默认值启动，默认就是管理 LAN。**
   `opennds/src/src/conf.h`：

   ```c
   #define DEFAULT_ENABLED "1"
   #define DEFAULT_GATEWAYINTERFACE "br-lan"
   ```

   `conf.c` 读取 `enabled`；不为 1 时进程直接 `exit(0)`。
   `gatewayinterface` 缺省为 `br-lan`。

3. **openNDS 的 nftables 基础链排在 fw4 之前。**
   `opennds/forward_authentication_service/libs/libopennds.sh`：

   ```sh
   nft add chain ip nds_filter ndsINP "{ type filter hook input priority -100 ; }"
   nft add chain ip nds_filter ndsFWD "{ type filter hook forward priority -100 ; }"
   ```

   优先级 -100 早于 fw4 的 input 链，因此 openNDS 的规则**先于**系统防火墙
   生效，系统防火墙里放行 LuCI/SSH 的规则来不及起作用。

4. **路由器方向的流量最终以 reject 收尾。**
   `opennds/src/src/fw_iptables.c` 在 `ndsRTR` 链中放行 gw_port 与 fas_port，
   `libopennds.sh` 的 `users_to_router` 逻辑再补上默认放行
   `udp/53 udp/67 tcp/22 tcp/443`，随后追加：

   ```sh
   # Block everything else
   nft add rule ip nds_filter $chain counter reject
   ```

   也就是说：走 `br-lan` 进来的、目标不是 22/53/67/443 的路由器流量会被 reject。
   LuCI 的 HTTP 端口 80 不在放行列表内。

5. **官方 init 脚本在启动时改写 WAN6 与 fw4。**
   官方 `opennds` 10.3.1 的 `start_service` 在注册 procd 实例后执行
   `libopennds.sh ipv6_routing block`（`uci set network.wan6.proto='none'`，
   随后 `service network reload`）和 `users_to_router allow`（向 fw4 input
   链插入放行规则）。它不检查 `enabled`；`enabled` 只在守护进程的 `conf.c`
   里起作用。它的 uci-defaults `40_opennds` 还会重建并提交 `firewall.opennds`。
   `dnsconfig.sh` 会按 `gatewayfqdn` 和 DHCP option 114 设置改写 dhcp 配置。

### 结论

上面的机制都有源码证据：依赖安装时 openNDS 会被自动启动，以 `br-lan`
为网关接口，在 fw4 之前以 reject 收尾，并改写 WAN6 与网络配置。它们是
LuCI 不可用与管理链路异常的**候选机制**，但不能单独解释全部现象：
`tcp/22` 在默认放行列表内，单凭这条规则不能推出 SSH 失联；被 reject 的
连接如何呈现为 404 也没有证据。故障现场没有保留，见「尚未确认的部分」。

### 修复

- 不再依赖官方 `opennds` 包。新增 `odac-opennds`：同一份上游源码，
  只替换安装生命周期。它不安装 `40_opennds`、`restart.sh` 和官方 init 脚本；
  自带的 init 脚本只有在 ODAC 已启用、`gatewayinterface` 等于受控接口、
  且该接口已在 `inet odac_guard` 的 controlled 集合中时才注册守护进程，
  不调用 `ipv6_routing` 或 `users_to_router`，也没有 `stop_service`。
  默认配置 `enabled 0`、`gatewayinterface odac-unconfigured`。
- `odac-preflight`：只接受 `odac-opennds`；发现官方 `opennds` 包、
  不属于 `odac-opennds` 的 `/etc/config/opennds` 或未门控的 init 脚本即拒绝。
  APK 不携带 `CONFLICTS`，两个包能否同时安装尚未验证，由这一条兜底。
- `odac-setup --enable`：在任何修改前检查 `nft list table ip nds_filter`
  中没有管理接口；先记录所有权、再装 fail-closed guard；结束时核对
  `network.wan6.proto` 未变，变了即回滚。
- 旧的 `60_odac-opennds-safe-default` uci-defaults 已删除：它在 openNDS 的
  postinst 之后运行，拦不住 postinst 的启动，也拦不住 WAN6 被改写。

### 未验证

`odac-opennds` 尚未完成真实 SDK 构建；新选项下守护进程的行为、
虚拟机内的 nft 与管理链路可达性都未验证。

---

## 根因 2：硬依赖 `libustream-mbedtls` 触发 TLS 提供者替换

### 证据链

1. **三个变体互斥，且安装同一个文件。**
   `feeds/base/libs/ustream-ssl/Makefile`：

   ```make
   define Package/libustream-wolfssl
     CONFLICTS := libustream-openssl
   define Package/libustream-mbedtls
     CONFLICTS := libustream-openssl libustream-wolfssl
   ```

   三个变体的 install 规则相同：

   ```make
   define Package/libustream/default/install
       $(INSTALL_DIR) $(1)/lib/
       $(INSTALL_DATA) $(PKG_INSTALL_DIR)/usr/lib/libustream-ssl.so $(1)/lib/
   endef
   ```

   即**都**安装 `/lib/libustream-ssl.so`。

2. **提供者是通过运行时 dlopen 加载的，不是链接期依赖。**
   `feeds/base/network/services/uhttpd/Makefile` 只有构建期依赖：

   ```make
   PKG_BUILD_DEPENDS = ustream-ssl
   ```

   而运行期依赖里**没有**任何 libustream 变体：

   ```
   Depends: libc, libblobmsg-json, libc, libjson-script, libjson-c
   ```

   uhttpd 源码 `tls.c`：

   ```c
   dlh = dlopen("libustream-ssl." LIB_EXT, RTLD_LAZY | RTLD_LOCAL);
   ```

   `libuclient`（`uclient-fetch` 的库）同样只有构建期依赖
   `Build-Depends: ustream-ssl`。

3. **官方发布索引里的实际元数据。**
   OpenWrt 25.12.2 `x86_64` 的 `base.adb`：

   ```yaml
   - name: libustream-mbedtls20201210
     provider-priority: 100
     provides:
       - libustream-mbedtls
       - libustream-mbedtls-any
   - name: libustream-openssl20201210
     provider-priority: 10
     provides:
       - libustream-openssl
       - libustream-openssl-any
   - name: libustream-wolfssl20201210
     provider-priority: 10
   ```

   **三个变体只 provide 自己的名字**（以及 `-any` 后缀），没有任何
   「实现无关的 TLS 能力包」。

4. **标准镜像装的是 mbedtls 变体。**
   官方 25.12.2 x86/64 rootfs 的包清单里是
   `libustream-mbedtls20201210`（并由 `apk-mbedtls` 提供 `apk`）。
   而生产 TR3000 上装的是 `libustream-openssl20201210`。

### 结论

`device-access` 硬依赖 `+libustream-mbedtls`，在已安装
`libustream-openssl20201210` 的路由器上会让 APK 求解出一个**变体替换**：
先卸载 openssl 变体（`/lib/libustream-ssl.so` 随之消失），再装 mbedtls 变体。

在这个窗口里：

- `uhttpd` 的 `dlopen("libustream-ssl.so")` 失败 → HTTPS 不可用；
- `uclient-fetch` 同样失败 → `wget: SSL support not available`；
- `apk` 自身依赖 `wget-any`（`apk-mbedtls` / `apk-openssl` 都依赖 `wget-any`），
  下载 HTTPS 索引失败 → `apk update` 失败。

这与故障报告中「HTTPS 软件包下载功能曾异常」和「`wget: SSL support not available`」
的现象吻合。

### 修复

`device-access` **不再依赖任何 libustream 变体**。理由是它不需要：

- ODAC 自身不链接 libustream。`device-access/CMakeLists.txt` 只链接
  json-c、sqlite3、argon2。
- TLS 提供者的存在性在**启用时**由 `odac-preflight` 显式校验：
  `/lib/libustream-ssl.so` 必须存在且可加载，否则是硬错误。
  不会自动安装某个变体（那会替换已有实现），也**不会**降级为明文 HTTP。

---

## 根因 3：构建图过宽（`feeds install -a`）

### 证据链

baseline 构建日志（`x86_64.log`，run 42861338436，172132 行）中：

- `Installing package '...'` 出现 **2041 次**。
  这个数字是 **feed 配方安装条数**，不是编译完成的包数。
  之前的分析把它当作「编译了 2041 个包」，是错误的。
- 实际进入编译的包目录（`Entering directory`）里包括
  `gettext-full`、`gnulib-l10n`、`gperf`、`libiconv-full`、`libunistring`、
  `libxml2`、`gnutls`、`libmicrohttpd-ssl`、`libmicrohttpd-nossl`、
  `openssl`、`wolfssl` 等。

`feeds/base/libs/gettext-full/Makefile`：

```make
PKG_BUILD_DEPENDS:=gettext-full/host libunistring libxml2
HOST_BUILD_DEPENDS:=gnulib-l10n/host gperf/host libiconv-full/host libunistring/host libxml2/host
```

`feeds/packages/libs/gnutls/Makefile`：

```make
PKG_BUILD_DEPENDS:=gettext-full/host
```

`feeds/packages/libs/libmicrohttpd/Makefile`：

```make
define Package/libmicrohttpd-ssl
  DEPENDS:=+libgnutls
```

`feeds/routing/opennds/Makefile`：

```make
DEPENDS:=+libmicrohttpd-no-ssl
```

### 结论

`gettext-full/host` → `gnulib-l10n/host` 的链路是
`opennds → libmicrohttpd → gnutls → gettext-full/host → gnulib-l10n/host`
的真实传递依赖。**它们不是无用依赖**，不能通过删除来提速。

基线日志中的耗时（来自 `time: package/.../compile#用户#系统#总计`）：

| 目标 | 耗时 |
| --- | --- |
| `gettext-full/host-compile` | 378.24 s |
| `libunistring/host-compile` | 146.88 s |
| `openssl/compile` | 348.55 s |
| `gnutls/compile` | 295.50 s |
| `wolfssl/regular/compile` | 79.77 s |
| `wolfssl/cpu-crypto/compile` | 78.70 s |
| `curl/compile` | 96.68 s |
| `gmp/compile` | 122.41 s |
| `unbound/compile` | 91.74 s |
| `sqlite3/compile` | 37.66 s |
| `opennds/compile` | 3.17 s |

基线整轮耗时：**3394.83 s**（2026-11-10 17:05:45.652276 → 18:02:20.479336）。

### 修复

`scripts/build-sdk.sh` 改为按名字精确安装，并把 `CONFIG_ALL` 关闭。

**注意两个已确认的事实**：

1. `scripts/feeds install base/libjson-c` 这种写法**无效**。
   `scripts/feeds` 的 `install_target_or_package` 把整个参数当作包名查找，
   找不到时只执行 `warn "WARNING: No feed for package ... found"` 并**返回 0**。
   正确的语法是 `feeds install -p <feed> <package>`。
   之前的 A/B 实验因此全部无效：日志里 24 条 "No feed for package" 警告，
   所谓「精准安装」从未真正发生。
2. `install_src()` 会自行递归处理 `builddepends` 与 `builddepends/host`，
   所以宿主工具会随包一起被选中，不需要手工列举。

---

## 尚未确认的部分

以下内容**没有**证据支撑，不作为结论：

- 故障时 openNDS 是否真的被安装并启动过。故障机器已经人工恢复，
  没有保留当时的 `apk` 数据库、`nft list ruleset` 或系统日志。
  根因 1 与根因 2 的机制已由源码与官方元数据证实，但「这台机器上确实
  发生了」这一点未复现验证。
- LuCI「404」的具体呈现形式。拒绝连接、超时、反向代理的错误页都可能
  被记录成 404。
- 是否还有第三个因素（例如 DNS 或 DHCP 残留）参与了故障。

## 复现计划

在没有实体 TR3000 的情况下，隔离环境中的可复现验证：

1. 在 OpenWrt 25.12.2 x86/64 虚拟机上从**干净系统**安装四个 APK，
   记录安装前后的 `apk list`、`/etc/rc.d/` 符号链接、
   `nft list ruleset` 与 `ip addr`。
2. 断言安装后 `opennds.@opennds[0].gatewayinterface` 不是 `br-lan`，
   `network.wan6.proto` 与安装前一致，`opennds` 进程不存在。
3. 断言安装后 `nft list table ip nds_filter` 不存在，或不包含管理接口。
4. 断言安装前后的 `/lib/libustream-ssl.so` **所有者不变**。
5. 断言安装后 `uhttpd` 的 HTTPS 仍可用，`apk update` 仍能下载 HTTPS 索引。

这些步骤目前**尚未执行**，属于未完成项。
