# ODAC 部署与恢复

适用边界：OpenWrt 25.12.x，APK 包管理，firewall4/nftables，openNDS 10.3.1。
安装默认关闭，不创建任何网络、DHCP 段或转发规则。**在生产路由器上执行前必须先完成验收。**
不会重启 WAN 或 LAN。不会读取、替换或覆盖既有上游自动登录脚本。

## 已验证与未验证

| 范围 | 状态 |
|---|---|
| 核心账号、审批、会话、设备策略、备份恢复 | 本机测试通过（核心 31 项 + 边界 22 项，valgrind 0 错误） |
| nftables 守卫、授权撤销、IPv6 拒绝、租约过期、fw4 表重载 | 隔离网络命名空间测试通过（52 项） |
| 真实路由器安装/升级/停用/卸载 | **未验证** |
| OpenWrt 25.12.2 x86/64 测试虚拟机 | 已实测：三个 APK 离线安装、默认不启用、预检通过、`odac-setup --enable` 接管成功、守护进程与守卫运行、门户 HTTP→HTTPS 跳转与 HTTPS 服务可用、rpcd→ubus 管理链路返回真实数据、管理 SSH 未被阻断。不等同生产路由器验收 |
| APK 产出 | `x86/64` 与 `mediatek/filogic`（Cudy TR3000 v1）两个架构均已产出 |
| 真实 LuCI 主题下的管理页渲染 | 静态资源与菜单/ACL 已验证，浏览器内渲染**未验证** |
| 限速精度、CPU/内存、并发吞吐、Flow Offloading 影响 | **未测量** |

未验证项意味着不能据此声称「已在真实设备上可用」。

## 构建

使用与目标设备**完全一致**的发行版、target、subtarget 的 Linux x86_64 SDK，解压前先按发行版 `sha256sums` 校验。

本项目在 25.12.2 上验证过两个 SDK target：`mediatek/filogic`（Cudy TR3000 v1）与 `x86/64`（测试虚拟机）。两者的 `include/version.mk` 都是 25.12.2，且 `CONFIG_USE_APK=y`。

下载并解压 filogic SDK 时，显式使用解压后的目录，不要把压缩包文件名当成目录：

~~~sh
SDK_ARCHIVE=openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst
SDK_DIR="$PWD/${SDK_ARCHIVE%.tar.zst}"
curl -O "https://downloads.openwrt.org/releases/25.12.2/targets/mediatek/filogic/$SDK_ARCHIVE"
curl -O "https://downloads.openwrt.org/releases/25.12.2/targets/mediatek/filogic/sha256sums"
grep "$SDK_ARCHIVE\$" sha256sums | sha256sum -c -
tar --zstd -xf "$SDK_ARCHIVE"
test -f "$SDK_DIR/include/package-pack.mk"
~~~

SDK 必须放在**大小写敏感**的文件系统上；放在 NTFS/exFAT 挂载点会被 OpenWrt 拒绝。

把三个包放入 SDK 后构建。`SRC_DIR` 指向本仓库根目录：

~~~sh
SRC_DIR=/path/to/ODAC
cp -R "$SRC_DIR/device-access"          "$SDK_DIR/package/"
cp -R "$SRC_DIR/luci-app-device-access" "$SDK_DIR/package/"
mkdir -p "$SDK_DIR/package/argon2"
cp "$SRC_DIR/argon2/Makefile"           "$SDK_DIR/package/argon2/Makefile"
cd "$SDK_DIR"
./scripts/feeds update -a
./scripts/feeds install -a
printf '\nCONFIG_USE_APK=y\nCONFIG_PACKAGE_libargon2=m\nCONFIG_PACKAGE_device-access=m\nCONFIG_PACKAGE_luci-app-device-access=m\n' >> .config
make defconfig
make package/device-access/compile package/argon2/compile package/luci-app-device-access/compile V=s
~~~

三个打包注意点，缺一即构建失败：

1. 官方源 `libjson-c` 的 pkg-config 文件在 SDK 中路径不可用，因此 `device-access` 的 `CMakeLists.txt` 用显式 include/library 路径，不依赖 pkg-config 查找。
2. 官方源没有 Argon2，`argon2` 配方自带上游 tarball（SHA256 已核对），导出 `libargon2.so.1` 与 `argon2.h`。配方目录固定为 `package/argon2/`，不要用包名当目录名。
3. LuCI 包现在是普通 `package/` 包，直接复制到 `$SDK_DIR/package/luci-app-device-access`；不要再放进 `feeds/luci`。

两个在交叉编译时才暴露的打包问题：

4. `CMakeLists.txt` 不能自带 `-D_FORTIFY_SOURCE=2`：SDK 的交叉编译参数已经带 `-D_FORTIFY_SOURCE=1`，重复定义会被 `-Werror` 当作错误。加固与优化参数改为仅本机构建时追加。
5. 纯静态的 LuCI 包需要显式的空 `Build/Compile` 目标，否则构建会去找一个不存在的 Makefile。

后端不再链接 libcurl：上游状态由管理员脚本写入状态文件，ODAC 只读该文件。

## 安装

~~~sh
apk add device-access luci-app-device-access
~~~

安装后系统仍是关闭状态，不接管任何网络。

## 启用

启用前必须准备好：

- 一个**独立**的受控网络（独立网段、独立 DHCP、独立防火墙 zone）。
- 内网可解析的门户域名，以及客户端**信任**的 TLS 证书与私钥（私钥 root 属主、权限 400 或 600）。
- 管理员自有的上游状态脚本，或直接使用 `/usr/libexec/odac-wan-state` 上报状态。

~~~sh
uci set odac.main.interface='br-odac'
uci set odac.main.gateway='192.168.30.1'
uci set odac.main.portal_hostname='portal.example.lan'
uci set odac.main.tls_cert='/etc/odac/portal.crt'
uci set odac.main.tls_key='/etc/odac/portal.key'
uci commit odac
/usr/libexec/odac-setup --enable
~~~

`odac-setup` 先安装故障关闭守卫，再启动 openNDS，然后写配置、`fw4 check`、`fw4 reload`，
最后做健康检查。任何一步失败都会回滚配置、停用服务并保留守卫；受控网络保持拒绝。

**启用顺序不可颠倒**：守卫必须在 openNDS 之前就位，否则会出现短暂放行窗口。

## 停用与卸载

~~~sh
/etc/init.d/odac stop
/usr/libexec/odac-remove --retain-guard
~~~

卸载刻意保留受控网络的拒绝规则，并保留 `/etc/odac` 下的数据库与备份。
重复启用时 `odac-setup` 会先清理上一次卸载遗留的退役守卫文件。

## 上游认证

上游认证完全由管理员自己的脚本负责，ODAC 不做探测、不实现登录、不接触上游门户。

管理员脚本在每次尝试后写一次状态：

~~~sh
/usr/libexec/odac-wan-state ONLINE
/usr/libexec/odac-wan-state AUTH_REQUIRED
~~~

该脚本写入一个固定格式的文件（默认 `/var/run/odac/upstream.status`，可用 `odac.main.status_file` 改）：

~~~
state=ONLINE
updated=1757500000
~~~

ODAC 只读这个文件：

- `state` 取 `ONLINE` / `REAUTHENTICATING` / `AUTH_REQUIRED` / `OFFLINE` / `UNKNOWN`。
- `updated` 是脚本最后一次刷新的 Unix 时间。文件过期或格式不对时，ODAC 视为「未证实」，下游转发保持关闭。
- 脚本进程是否存在只作为诊断展示，**不算**认证成功。
- 文件必须是 root 属主、不可被 group/world 写入，且不能是符号链接。

状态非 `ONLINE`（含 `UNKNOWN`）时下游转发关闭；恢复 `ONLINE` 后自动放行，已登录设备不需要重新输入密码。
路由器自身访问上游的能力不受影响（守卫没有 output 钩子）。

若管理员脚本写 PID 文件，可设 `odac.main.pid_file`，管理界面会显示脚本是否在运行（仅作诊断）；未配置时不显示。

上游认证门户地址属于部署配置，ODAC 不内置任何地址。按实际网络配置 IPv4 列表（最多 16 个），守卫会在受控接口上双向阻断这些地址，以免下游客户端绕过 ODAC 直接访问上游门户：

~~~sh
uci add_list odac.main.upstream='203.0.113.10'
uci add_list odac.main.upstream='203.0.113.20'
uci commit odac
~~~

未配置时守卫不阻断任何上游地址。修改后重新执行 `odac-setup --enable`，或重启 `odac` 服务，让守卫表重建。

阻断认证地址不能消除上游发出的全部重定向。管理员脚本写入状态到 ODAC 读取之间存在短暂窗口，
这段窗口内个别请求仍可能收到上游重定向。缩短窗口是管理员脚本的责任：每次尝试后立即写入状态。

## 故障恢复

| 现象 | 处理 |
|---|---|
| 守护进程崩溃或被杀 | 守卫租约最长 90 秒后自然过期，转发关闭。procd 会自动重启。 |
| 防火墙重载 | 守卫位于独立 nft 表，`fw4` 重载后仍在。 |
| `fw4` 规则被误删 | 重新执行 `odac-setup --enable` 或 `/usr/libexec/odac-firewall init ...`。 |
| 撤销授权失败 | 设备进入待重试队列，转发保持关闭；每次心跳重试，直到成功。 |
| 需要恢复接入 | `/etc/init.d/odac stop` 后按启用步骤重跑 `odac-setup --enable`。 |
| 数据库损坏 | 用 LuCI 系统设置中的备份文件恢复，或直接放置 `/tmp/odac-restore.json` 后调用 `restore`。 |

## 设备上的已知限制

- OpenWrt 25.12 的 busybox **不含 `stat` applet**。项目脚本已改用 `ls -ld`，但自写的运维脚本若依赖 `stat -c`，需改用 `ls -ld` 或安装 `coreutils-stat`。
- uhttpd 的 CGI 模式不实现 `Expect: 100-continue` 握手。用 curl 向门户 CGI 发 POST 时需加 `-H "Expect:"`，否则请求体不会送达 CGI。
- openNDS 10.3.1 在 OpenWrt 25.12 上启动时输出 `uci: Invalid argument`（其 ipset/nftset 配置路径）。原始官方 openNDS 配置同样复现，属上游不兼容，不影响 ODAC 的守卫与授权接口。

## 安全约束

- 未认证设备默认拒绝访问 WAN；认证网络 IPv6 转发被显式拒绝。
- 管理网络与认证网络必须隔离；受控网络无法访问路由器管理接口与 SSH。
- 账号密码只经 HTTPS 传输。未受信任的自签名证书会让客户端无法验证服务器身份；把该证书加入客户端信任，或使用客户端信任的私有 CA 签发的证书，都可以正常工作。
- MAC 白名单只是便捷准入，不能证明设备身份，会被随机 MAC 或 MAC 伪造绕过。
- 限速是 openNDS 的平均速率控制，不保证瞬时带宽上限。
- 会话只保存在内存，路由器重启后需要重新登录；白名单与限速策略保留在数据库中。
