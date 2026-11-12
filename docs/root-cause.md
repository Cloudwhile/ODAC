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

`odac-opennds` 已由 CI 构建，并在 x86/64 测试虚拟机上安装：安装后守护进程
没有启动，nft 表和管理链路不变（见部署文档的验证表）。后续准备独立网段、
测试证书及 dummy 端口后，r1 原版启用因立即检查尚未就绪的 ndsctl 而失败并回滚；
诊断副本等待 4 秒后成功。r2 增加有限次数的就绪检查，修复包的 VM 验收待执行。
诊断启用期间 nds 规则仅引用 br-odac，WAN6、DHCP 配置不变，管理面检查通过；
没有真实受控客户端，不能据此声称完成准入验收。

对固定版本 v10.3.1 的源码只读调查确认：

- [main.c](https://github.com/openNDS/openNDS/blob/v10.3.1/src/main.c#L522)
  在启动时两处调用 `reload_only`（另一处在 894 行，为后台调用），
  [fw_iptables.c](https://github.com/openNDS/openNDS/blob/v10.3.1/src/fw_iptables.c#L423)
  在内部防火墙初始化时再调用一次；正常启动共请求三次 reload。
  [终止处理](https://github.com/openNDS/openNDS/blob/v10.3.1/src/main.c#L186)
  无条件请求一次 `restart_only`。现有四个配置选项不能抑制这些调用。
- [dnsconfig.sh](https://github.com/openNDS/openNDS/blob/v10.3.1/forward_authentication_service/libs/dnsconfig.sh#L57)
  操作共享 dnsmasq 服务，没有独立实例选择；调用服务命令后直接退出 0，
  未保留其真实退出状态。C 调用点只输出通用 `system()` 失败日志，后台调用也
  不等待 reload 完成。因此 VM 日志不能证明 dnsmasq 服务失败，具体原因和实际
  DNS 影响尚未确认，不能把子进程回收竞态等推测写成已确认根因。
- helper 的 `revert` 对整个 DHCP 配置执行 `uci revert dhcp`，option 114 关闭时
  `cpidconf` 仍可能删除匹配配置。VM 中 DHCP 文件不变只是该次观察。
- [HTTP listener](https://github.com/openNDS/openNDS/blob/v10.3.1/src/http_microhttpd.c#L79)
  没有设置 `MHD_OPTION_SOCK_ADDR`，与 VM 的 `0.0.0.0:2050` 监听一致；
  日志打印受控地址不证明地址绑定。请求级接口判断不能代替防火墙隔离，
  实际可达范围仍未验证。

这些结论来自固定上游版本的调用链；本轮未核对 VM 可执行文件与上游源码的
逐字节对应关系，也未测定其安装的 dnsmasq init/procd reload 行为。

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

上述配方指向候选链路 `opennds → libmicrohttpd 源码包 → gnutls →
gettext-full/host → gnulib-l10n/host`，但尚缺该次构建的生成依赖图与
Make prerequisite trace，不能把每一条边都视为已证实。SDK 的 feeds 工具会安装
同一源码包各变体的依赖，package-metadata.pl 也会聚合源码包的编译依赖；
配方被安装并不等于它必然被编译，更不等于 no-ssl 变体运行时链接 GnuTLS。

后续成功构建 [run 43154130757](https://github.com/Cloudwhile/ODAC/actions/runs/43154130757)
（提交 `10726da`）的 `sdk-build.log` 提供了以下直接证据：

- 首次 gettext/gnulib 宿主编译出现在 `package/odac/odac-opennds/compile`
  调用范围内（日志 2873–90323 行）；GnuTLS 配置实际调用
  `staging_dir/hostpkg/bin/autopoint --force`（82773 行），该工具由 gettext
  安装（78009 行）。这是已确认的宿主工具消费关系。
- libmicrohttpd 的实际配置带 `--disable-https --without-gnutls`（89513 行）。
  因此不能把所观察到的 GnuTLS 编译解释为 no-ssl 库的运行时链接需求。
- `gnulib-l10n` 安装的是宿主本地化文件（48282–48320 行）；gettext 自身的
  autogen 还会导入 gnulib 模块。这是两类不同工作。
- gettext 首次耗时 301.92 s，后续两次目标访问均为 0.35 s；
  gnulib-l10n 首次 2.64 s，后续均为 0.32 s，不能把重复目标名当作完整重编译。
  GnuTLS 则确实又执行了一次配置/编译，耗时从首次 222.54 s 到第二次 221.41 s。

三次相同构建步骤的 x86/64 运行（43154130757、43180259567、43186925580，
产物逐字节一致）的日志还显示：

- **构建是串行的**。`build-sdk.sh` 调用 `make` 时没有给 `-j`，日志里 19 处
  包级子 make 全部是 `make -j1`。
- **四次 `make` 调用之间有重复工作**。`package/kernel/linux/compile` 在后三次
  调用中各执行一遍，每遍重新执行 966 次 APK 打包（其中 965 个是 `kmod-*`，
  165–211 s/遍）。
  GnuTLS 的两次完整编译分别落在第 2、3 次调用，第三次访问只用 0.26 s。
- **GnuTLS 第二次编译的原因仍是待证实的解释**。两次编译之间
  `pkginfo/libopenssl.version` 等 15 个库版本文件被写入；上游
  `include/package.mk` 让 prepare stamp 依赖配方内各子包传递依赖的
  `pkginfo/*.version`，而 gnutls 配方的 `libgnutls-dane` 依赖 `libunbound`，
  后者依赖 `libopenssl`。这与「第 3 次调用编译 OpenSSL 后 GnuTLS 的 stamp
  过期」相符，但没有那次 `make -q -d` 的输出，不能当作已确认。内核包重做的
  stamp 原因同样未确定。
- **依赖边已从生成的依赖图直接读到**（run 43209085571 诊断产物
  `packagedeps.mk`）：

  ```make
  $(curdir)/odac/odac-opennds/compile += … $(curdir)/feeds/packages/libmicrohttpd/compile …
  $(curdir)/feeds/packages/libmicrohttpd/compile += $(curdir)/feeds/packages/gnutls/compile $(curdir)/toolchain/compile
  $(curdir)/feeds/packages/gnutls/compile += $(curdir)/feeds/base/gettext-full/host/compile $(curdir)/feeds/base/nettle/compile … $(if $(CONFIG_LIBNETTLE_MINI),,$(curdir)/feeds/base/gmp/compile) …
  $(curdir)/feeds/base/gettext-full/host/compile += …gnulib-l10n/host/compile …gperf/host/compile …libiconv-full/host/compile …libunistring/host/compile …libxml2/host/compile
  ```

  依赖图中只有两个目标要求 gnutls：libmicrohttpd（无条件）和 curl（受
  `CONFIG_LIBCURL_GNUTLS` 约束，本配置未设置，curl 使用 mbedTLS）。
  libmicrohttpd → gnutls 这条边没有条件，与选中哪个变体无关：
  `scripts/package-metadata.pl` 按源码包聚合所有子包的 `DEPENDS`，而
  `libmicrohttpd-ssl` 写的是 `DEPENDS:=+libgnutls`。所以 TODO §11 的回答是：
  首先触发的是 `package/odac/odac-opennds/compile`；gettext 与 gnulib-l10n 是
  **宿主构建依赖**，不是目标系统的运行依赖；它们由 SDK 的构建图要求，不是
  `feeds install -a`、LuCI、Argon2 或 CMake 引入的。这些依赖不删除。
- **选中的包符号有 1473 个，其中 1176 个是 `kmod-*`，来自 SDK 自带的默认值**。
  已校验的官方 x86/64 SDK 归档中的 `Config-build.in` 为 1455 个 `PACKAGE_*`
  符号记录了默认值（含全部 1176 个 `kmod-*`），CI 选中的 1473 个正好是这
  1455 个加上 ODAC 四包及其依赖的 18 个。也就是说内核模块打包是 SDK 发布
  配置带来的，不是 ODAC 的依赖；输入配置里的 `# CONFIG_ALL_KMODS is not set`
  不会取消这些逐包默认值。本次不改动这一选择。
- **Runner 之间的波动很大**。相同步骤的总时长为 2403、2486、3105 s
  （gettext 302/313/384 s），单次对比不足以证明提速或变慢。

据此 `build-sdk.sh` 改为**一次** `make` 调用并传入 `-j$(nproc)`：目标是依赖
其余三包的 `luci-app-device-access`，随后检查四个包都已产出。依赖和包选择
不变。构建脚本同时把 defconfig 前后的配置和生成的 `tmp/.packagedeps` 存入
诊断产物（`sdk-config-input`、`sdk-config`、`packagedeps.mk`），用于逐边核对
上面的内容。

x86/64 实测，均为 4 个并行任务：
[43209085571](https://github.com/Cloudwhile/ODAC/actions/runs/43209085571)
（`435a76e`，SDK 与源码缓存未命中）、
[43213571827](https://github.com/Cloudwhile/ODAC/actions/runs/43213571827)
（`8900f71`，缓存命中）、全矩阵运行
[43214081475](https://github.com/Cloudwhile/ODAC/actions/runs/43214081475)
中的 x86/64 Job，以及
[43219699053](https://github.com/Cloudwhile/ODAC/actions/runs/43219699053)
（`37f2daf`），以及 [43224183934](https://github.com/Cloudwhile/ODAC/actions/runs/43224183934)（`58a6883`）。

| 项目 | 四次串行调用（三次运行） | 一次并行调用（五次运行） |
| --- | --- | --- |
| 编译与打包阶段 | 2324 / 2381 / 3020 s | 1122 / 886 / 1128 / 865 / 1071 s |
| `build-sdk.sh` 全程 | 2403 / 2486 / 3105 s | 1205 / 964 / 1340 / 951 / 1156 s |
| `package/kernel/linux` 执行次数 | 3 | 1 |
| GnuTLS 完整编译次数 | 2 | 1 |
| 四个 APK 的 SHA256 | — | 与串行构建逐字节一致 |

五次并行运行都比三次串行运行中最快的一次短，但 Runner 波动大（并行样本
自身相差约 40%），所以不给出稳定的提速比例。第一次并行运行的各目标耗时
之和为 2978 s（并行下单个目标的墙钟时间变长，例如 openssl 585 s），说明
收益来自并行和去掉重复工作，而不是少编译了东西：访问的目标仍是 56 个。
仍有 19 处子 make 是 `-j1`，对应没有声明可并行构建的配方（例如 gettext
明确写了 `PKG_BUILD_PARALLEL:=0`），未作改动。

全矩阵运行的 11 个 target 都只有一次 `make` 调用、GnuTLS 各编译一次，
构建脚本全程 921–1343 s。

改为一次调用后 GnuTLS 只编译一次；旧布局下第二次编译的确切原因没有被证实，
也不再出现。

SDK 归档和下载源码的缓存不缩短编译（缓存命中的 43180259567 为 2486 s）。
跨 Job 复用宿主编译产物尚未验证，预编译 host 工具清单仍待补证。

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

2026-11-12 在 x86/64 测试虚拟机上用测试签名包执行了第 2–5 步，断言全部成立
（结果见部署文档的验证表）。第 1 步的「干净系统」前提**不满足**：该虚拟机有
旧安装的残留。这验证的是修复后的包，不是对原故障的复现。
