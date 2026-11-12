# OpenWrt Device Access Controller (ODAC)

面向 OpenWrt 25.12.x 的网络准入与认证管理插件。

- 后端：原生 C，单进程守护进程 `odacd`，SQLite 存储，Argon2id 口令哈希，不链接 HTTP 客户端。
- 管理界面：LuCI JavaScript 插件，7 个页面。
- 客户端门户：独立原生 HTML/CSS/JS，账号注册、审批、登录与状态查询，只经 HTTPS。
- 准入引擎：openNDS 10.3.x 的 `ndsctl` 接口，nftables 故障关闭守卫。
- 上游认证：管理员脚本写状态文件，ODAC 只读，不探测、不登录、不接触上游门户。

门户规则：每个设备只能注册一个账号（以邻居表核验的 MAC 判定，IP 与设备名仅作记录）；
注册后账号状态为待批准，登录时账号无权限则提示「该账号没有权限访问」。

## 目录

~~~
openwrt-device-access/
├── device-access/            后端包（odacd / odacctl / odac-portal，依赖 libargon2）
├── luci-app-device-access/   LuCI 管理界面包（依赖 device-access）
├── argon2/                   Argon2 库配方，生成 libargon2 包（官方源无此包）
├── docs/                     部署、恢复与 CI 构建说明
├── tests/                    核心、边界、生命周期与平台测试
└── scripts/                  快速校验、SDK 构建与环境探测辅助
~~~

## 架构

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| `odacd` | `device-access/src/main.c` | 守护进程。Unix socket 只接受 root 调用，每 5 秒一次心跳，维护会话、策略、租约与审计。 |
| `odacctl` | `device-access/src/odacctl.c` | 管理命令行。`--wait-ready` 用 inotify 等待守护进程就绪。 |
| `odac-portal` | `device-access/src/portal-cgi.c` | 门户 CGI。身份只取自服务器观察到的对端地址。 |
| 核心 | `device-access/src/core*.c` 等 | 账号、审批、会话、设备策略、备份恢复。 |
| JSON 兼容层 | `device-access/src/json_text.c` | 严格 JSON 解析；为 json-c 0.18 的 UTF-8 缺陷提供转义。 |
| 平台 | `device-access/src/platform*.c` | openNDS 交互、邻居校验、WAN 状态文件读取。 |
| 守卫 | `files/usr/libexec/odac-firewall` | 独立 nft 表，`inet odac_guard`，forward/input 优先级 -10。 |

管理面与门户面完全分离：门户只经受限 HTTPS 接口，HTTP 端口只做跳转，无法访问管理员 RPC；
管理接口全部经 rpcd，权限由 ACL 控制。包依赖方向为 `luci-app-device-access` → `device-access` → `libargon2`（配方目录为 `argon2/`），后两者不依赖 LuCI。

## 安全模型

- **默认拒绝**：未认证设备无法访问 WAN；未知状态按拒绝处理。
- **故障关闭**：守卫只清理租约、不删除表；守护进程停止或崩溃后，转发在租约到期时关闭。
- **上游隔离**：`/usr/libexec/odac-wan-state` 只写状态文件，ODAC 只读；不做探测、不登录、不访问上游门户。
- **身份来自服务端**：请求体中的 MAC、IP 与令牌一律丢弃，只信任服务器观察到的对端地址；会话令牌只认 HttpOnly Cookie。
- **口令保护**：Argon2id（8 MiB / 3 轮 / 1 并行度），明文不落库、不入日志；限制单实例验证以控制内存峰值。
- **隔离**：认证网络不能访问路由器管理接口与 SSH；认证网络 IPv6 转发被拒绝。
- **审计**：审批、封禁、限速等操作记录操作者、时间、对象与详情。

## 访问策略优先级

1. 设备黑名单：拒绝。
2. 上游 WAN 未认证：停止普通客户端转发。
3. 有效白名单：免登录放行，仍受限速与统计约束。
4. 有效账号会话：按设备策略放行。
5. 其他：进入认证门户。

限速覆盖顺序：设备自定义 > 账号默认 > 系统默认。`null` 表示继承，`0` 表示明确不限速。

## 认证门户开关

管理员可在 LuCI「系统设置 → 认证门户」停用认证。该开关只改准入策略，不动网络与防火墙配置：

- **启用**（默认）：设备需登录账号或加入白名单才能上网。
- **停用**：受控网络内除黑名单设备外的所有设备直接放行，无需登录；同时拒绝新的注册与登录。

停用期间黑名单与策略过期仍然生效，且单个设备授权失败不会连累其他设备（不会关闭整个网关）。

## 门户错误提示

门户对所有涉及账号授权状态的错误（未批准、已拒绝、已封禁、已过期、设备被禁、会话配额已满、注册受限）统一显示「无权限，请联系网络管理员」，不透露账号是否存在或处于何种状态。密码错误、输入格式、频率限制等仍显示各自的具体提示；未知错误码不会回显后端原始信息。

## 安装与依赖关系

四个 APK 必须按 **libargon2 → odac-opennds → device-access → luci-app-device-access** 的顺序安装：

| 包 | 架构 | 依赖 |
| --- | --- | --- |
| `libargon2` | 目标架构 | `libc` |
| `odac-opennds` | 目标架构 | `libmicrohttpd-no-ssl`、`nftables-json`、`uci` |
| `device-access` | 目标架构 | `libargon2`、`libsqlite3`、`libjson-c`、`uhttpd`、`odac-opennds`、`firewall4`、`nftables-json`、`rpcd`、`uci`、`ubus`、`curl`、`openssl-util`、`ca-bundle` 等 |
| `luci-app-device-access` | `all`（架构无关） | `device-access`、`luci-base`、`rpcd` |

~~~sh
apk add libargon2
apk add odac-opennds
apk add device-access
apk add luci-app-device-access
~~~

依赖方向是 `luci-app-device-access` → `device-access` → `libargon2`，后两者不依赖 LuCI。
`apk` 会自动按依赖关系排序，但显式按顺序安装可以在依赖求解失败时立即看到原因。

### TLS 提供者：不固定实现，也不替换现有实现

OpenWrt 25.12 的 `libustream-mbedtls`、`libustream-openssl`、`libustream-wolfssl`
三者**互斥**（`package/libs/ustream-ssl/Makefile` 中的 `CONFLICTS`），并且
**都安装同一个文件** `/lib/libustream-ssl.so`。`uhttpd` 与 `uclient-fetch` 在运行时
用 `dlopen("libustream-ssl.so")` 加载它；该文件缺失时它们打印
"SSL support not available"，`apk update` 与 HTTPS 下载随即失败。

因此 `device-access` **不依赖任何 libustream 变体**：

- ODAC 自身不链接 libustream（`device-access/CMakeLists.txt` 只链接 json-c、
  sqlite3、argon2），因此它不需要某个特定实现。
- 强制依赖某个变体会让 APK **替换**管理员已有的提供者。生产 TR3000 上安装的是
  `libustream-openssl20201210`，依赖 `libustream-mbedtls` 会触发变体替换；
  三个变体安装同一路径，替换过程中 `/lib/libustream-ssl.so` 会短暂消失。
- TLS 提供者的存在性在**启用时**由 `odac-preflight` 显式校验
  （`/lib/libustream-ssl.so` 必须存在且可加载）。缺失时是硬错误，
  **不会**自动安装某个变体，也**不会**降级为明文 HTTP。

检查当前路由器上的 TLS 提供者：

~~~sh
apk info -W /lib/libustream-ssl.so   # 谁提供了这个文件
apk list --installed 'libustream-*'   # 已安装的变体
apk info -a libustream-openssl        # 仓库中的可用版本
~~~

### 安装与网络接管是两件事

ODAC 由四个 APK 组成：`libargon2`、`odac-opennds`、`device-access`、
`luci-app-device-access`。安装它们**不会**启用准入控制，也不会改动网络配置。
接管是独立的显式操作：

~~~sh
/usr/libexec/odac-setup --enable
~~~

准入后端是 `odac-opennds`，而不是官方 `opennds` 包。它用同一份上游源码
（openNDS 10.3.1，校验和与官方配方一致）编译，只替换了安装生命周期：

- OpenWrt 的 `default_postinst`（`/lib/functions.sh`）会对新装包的每个
  `/etc/init.d/*` 执行 `enable` 和 `start`，依赖包的 postinst 先于 ODAC 运行，
  ODAC 的 uci-defaults 无法拦住它。
- 官方 opennds 的 `start_service` 会执行 `libopennds.sh ipv6_routing block`
  （`uci set network.wan6.proto=none` 并 reload 网络），并向 fw4 input 链插入
  `users_to_router` 规则；它的 uci-defaults 还会重建 `firewall.opennds`。

`odac-opennds` 的 init 脚本只有在以下条件全部满足时才注册守护进程：配置属于
`odac-opennds`、openNDS 与 ODAC 都已启用、`gatewayinterface` 等于 ODAC 的受控
接口、且该接口已在 `inet odac_guard` 的 controlled 集合中。否则 `start` 只记一条
日志并返回 0。它不调用 `ipv6_routing`、不调用 `users_to_router`、没有
`stop_service`，包内也不带 uci-defaults 和 `restart.sh`。默认配置为
`enabled 0`、`fwhook_enabled 0`、`users_to_router_passthrough 1`、
`gatewayfqdn disable`、`dhcp_default_url_enable 0`。

ODAC 不接管也不改写官方 opennds：preflight 发现已安装官方 `opennds` 包、
`/etc/config/opennds` 不属于 `odac-opennds`，或 init 脚本不是带门控的版本时，
直接拒绝启用。APK 不会携带 `CONFLICTS` 字段，两个包同时安装时是否会因文件冲突
被 apk 拒绝**尚未验证**，因此这条拒绝由 preflight 保证。

`--enable` 在第一次改动前记录所有权（含 `enable_pending`），然后先装 fail-closed
guard，再切换配置。任何一步失败都会关闭受控网络、逐字节恢复
`/etc/config/opennds`、恢复退役 guard 文件、删除 ODAC 保留的防火墙段；回滚本身
有任何一步失败，则保留所有权并提示运行 `odac-setup --disable`。启用结束时会核对
`network.wan6.proto` 与启用前一致，不一致视为失败并回滚。

## 停用、卸载与彻底清理

三个生命周期操作彼此独立，语义不同：

| 操作 | 命令 | 影响 |
| --- | --- | --- |
| 普通卸载 | `apk remove device-access`（触发 `odac-remove --retain-guard`） | 只停 ODAC 自己的服务；不动别的业务的 openNDS；保留数据库与备份；**受控网络保持关闭** |
| 安全停用 | `/usr/libexec/odac-setup --disable` | 关闭准入、关守卫、保留受控网络的拒绝边界；管理网络不受影响 |
| 恢复原网络策略 | `/usr/libexec/odac-setup --restore-network` | 显式移除退役守卫，受控网络回到普通防火墙策略（要求受控接口已 down） |
| 数据清理 | `/usr/libexec/odac-purge --dry-run` / `--backup` / `--confirm [--confirm-data]` | 单独的命令，**不会**在普通卸载时自动执行 |

`odac-purge` 的规则：

- **只删除明确列出的文件**：`/etc/odac/odac.db`、`odac.db-wal`、`odac.db-shm`
  与 `/etc/config/odac`。不递归清空 `/etc/odac`，管理员的密钥、备份与
  其它文件一律保留。
- 不删除防火墙段、不删除守护守卫。**网络退役由 `odac-remove` 负责**，
  数据清理只处理数据，两者的职责不混在一起。
- `--dry-run` 先列出将被删除的数据文件与被保留的内容，不做任何改动。
- `--backup` 生成一份包含数据库、WAL、归属文件与退役守卫的私有快照（0700）。
- 删除数据库需要额外的 `--confirm-data`；只给 `--confirm` 会被拒绝。
- ODAC 仍在运行、仍有未退役的防火墙段、或仍被启用时，清理被拒绝。
- **上游自动认证脚本、`/etc/config/opennds`、`/etc/config/uhttpd` 与非 ODAC
  创建的防火墙段一律不动。**

资源归属的唯一事实来源是 `/etc/odac/ownership`（root 属主，0600）。

## 构建与测试

### 快速校验（不下载 SDK）

`scripts/fast-check.sh` 用宿主工具链编译可移植部分并跑测试，不下载 SDK：

~~~sh
sudo sh scripts/fast-check.sh
~~~

它依次执行：所有 shell 脚本的语法检查、原生 CMake 配置与编译、CTest（核心账号/会话测试与
JSON 边界测试）、构建脚本单元测试、生命周期回归测试、启用事务测试（`scripts/test-deployment.py`），
以及平台与 nftables 测试。

平台与 nftables 测试需要非特权用户命名空间。环境不提供时会**明确报告 SKIPPED**，
不会被当成通过。

### SDK 打包

SDK 打包入口是 `scripts/build-sdk.sh`。先按官方 `sha256sums` 校验并解压与路由器一致的
Linux x86_64 SDK（25.12.2），然后：

~~~sh
scripts/build-sdk.sh /path/to/sdk/mediatek-filogic-gcc-14.3.0_musl.Linux-x86_64 mediatek filogic
~~~

脚本做四件事：

1. 校验 SDK 目录名与请求的 target/subtarget 一致（防止把 x86 的产物冒充其它 target）。
2. 用 `feeds install -p <feed> <package>` **按名字精确安装**所需的 feed 包。
   注意：`feeds install base/libjson-c` 这种写法无效——工具会把
   `base/libjson-c` 当作整个包名，找不到时只打印一句警告并**仍然返回 0**。
3. 写入最小 `.config` 并执行 `make defconfig`，关闭 `CONFIG_ALL`
   （Kconfig 把关闭的布尔写成 `# CONFIG_ALL is not set`，校验按这两种形式都接受）。
4. 在 `defconfig` 之后**校验**四个包和它们的运行依赖确实被选中，
   并确认 `CONFIG_ALL` 仍然是关闭的、官方 `opennds` 没有被选中。

依赖会由 feeds 工具自行递归解析：`odac-opennds` 依赖 `libmicrohttpd-no-ssl`；
feeds 按**源码包**安装，`libmicrohttpd` 源码包同时含 ssl 变体，
`libmicrohttpd-ssl` 拉入 `gnutls`，而 `gnutls` 需要 `gettext-full/host`，
后者需要 `gnulib-l10n/host`、`gperf/host`、`libiconv-full/host`、
`libunistring/host` 与 `libxml2/host`。**这些宿主工具是真实需要的**，
不能通过删除依赖来缩短构建时间，只能靠缓存复用。

本机可运行（Linux 主机，无需路由器；需要 json-c、SQLite 与 Argon2 的开发头文件和库）：

~~~sh
cd device-access
cmake -S . -B build -DODAC_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
~~~

平台与 nftables 测试：

~~~sh
sh tests/run_platform_tests.sh
~~~

该脚本需要 cc、json-c 头文件、iproute2、nftables 与可用的非特权用户命名空间，在隔离环境中运行，不会触碰宿主机网络。

生命周期回归测试（需要 root，脚本会自行检查）：

~~~sh
sudo sh tests/lifecycle_test.sh
~~~

### CI 构建范围

| 触发类型 | 构建范围 |
| --- | --- |
| Pull Request | 仅 x86_64 |
| main 分支 Push | 仅 x86_64 |
| 版本 Tag（`v*`） | 全部架构 |
| 手动 `workflow_dispatch` | 默认 x86_64，`build_scope=full` 时全部架构 |

构建范围由 `scope` Job 解析成**真正的矩阵**：quick 运行的矩阵里只有 x86_64 一项，
而不是「跳过步骤但仍执行后续步骤」。支持的架构列表在 `scripts/targets.txt`，
构建矩阵、Release 完整性检查与 workflow 校验都读这一个文件。

快速校验 Job 在所有交叉编译 Job 之前运行（`needs: [scope, fast-check]`），
它失败时**不会**下载任何 SDK。它执行：shell 语法检查、原生 CMake 配置与编译、
CTest、构建脚本单元测试、workflow 配置检查与变异回归、构建范围解析、
生命周期回归测试，以及平台与 nftables 测试。

每个架构的构建结果、SDK 校验、耗时与 APK 的 SHA256 会写入 Job Summary。
CI 的完整说明（SDK 校验、APK 元数据校验、Release 保护、缓存与计时）见
`docs/deployment.md` 的「CI」一节。

## 已知限制

- 启用前必须提供客户端信任的 TLS 证书。未受信任的自签名证书会让客户端无法验证服务器身份；把该证书加入客户端信任，或使用客户端信任的私有 CA 签发的证书，都可以正常工作。
- 上游认证失效到管理员脚本写入状态之间存在短暂窗口，ODAC 不自行探测。
- 首版以「禁止认证网络 IPv6 转发」防绕过，不是完整双栈准入。
- 限速是平均速率控制，不保证瞬时带宽上限。
- 会话只在内存中，路由器重启后需重新登录。
- OpenWrt 25.12 提供 json-c 0.18，其严格模式用带符号 `char` 比较控制字符，会把 UTF-8 高位字节误判为非法，导致中文门户文案无法保存或读回（上游在 0.19 修复）。`src/json_text.c` 在严格解析前把字符串内的原始 UTF-8 转成 `\uXXXX` 转义来规避，保留严格模式对控制字符、注释与尾随垃圾的拒绝。
- `odac-opennds` 的 init 脚本不调用 `ipv6_routing`，因此启用 ODAC 不再把
  `network.wan6.proto` 改成 `none`；但 openNDS 守护进程在新选项
  （`users_to_router_passthrough 1`、`gatewayfqdn disable`、`fwhook_enabled 0`）
  下的实际行为**尚未在虚拟机或路由器上验证**。
- 受控客户端只能访问门户端口，不能直连路由器的 SSH/LuCI，这是设计意图。
- 真实路由器上的安装、验收与性能**尚未验证**。本次整改后的代码（含 `odac-opennds`）尚未完成一次
  真实 SDK 构建，也未在测试虚拟机上复测；Cudy TR3000 实体机验证**未完成**。
  详见 `docs/deployment.md` 的「已验证与未验证」表，根因分析见
  `docs/root-cause.md`。

