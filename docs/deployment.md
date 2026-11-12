# ODAC 部署与恢复

适用边界：OpenWrt 25.12.x，APK 包管理，firewall4/nftables，openNDS 10.3.1。
安装默认关闭，不创建任何网络、DHCP 段或转发规则。**在生产路由器上执行前必须先完成验收。**
不会重启 WAN 或 LAN。不会读取、替换或覆盖既有上游自动登录脚本。

故障根因与证据见 `docs/root-cause.md`。

## 已验证与未验证

| 范围 | 状态 |
| --- | --- |
| 核心账号、审批、会话、设备策略、备份恢复 | 本机 CTest 通过（`core` 与 `boundary` 两个测试，boundary 20.69 s） |
| nftables 守卫、授权撤销、IPv6 拒绝、租约过期、fw4 表重载 | 隔离网络命名空间测试通过（56 项） |
| 生命周期脚本（归属记录、停用、卸载、恢复网络、清理、`odac-opennds` 门控 init） | 宿主隔离测试通过（267 项，`tests/lifecycle_test.sh`，需 root） |
| 启用事务（所有权先记录、回滚、WAN6 核对、拒绝中断/未登记状态） | 宿主沙箱测试通过（11 项，`scripts/test-deployment.py`，含 openNDS 延迟就绪、始终未就绪回滚及 DNS checker 缺失/拒绝/通过），5 个故意注入的缺陷均被检出 |
| 构建脚本单元测试（feeds 选择、Kconfig、版本与 target 校验、拒绝官方 opennds） | 桩测试通过（25 项） |
| workflow 结构校验 + shell 语法检查 | 通过（`tests/workflow_check.py`，11 个目标） |
| workflow 变异回归 + 校验和夹具 | 通过（6 项，含各包独立版本与发布缺包拒绝，`tests/workflow_negative_test.sh`） |
| 构建范围解析（矩阵内容） | 通过（12 项，`tests/scope_test.sh`） |
| 11 个架构的 `arch` 与官方 `profiles.json` 一致 | 逐项核对通过 |
| 11 个目标均有官方 SDK 归档且归档名正则匹配 | 逐项核对通过 |
| 真实 SDK 构建与 APK 产出（含 `odac-opennds` 的 autoreconf 与链接） | 提交 `10726da` 的 [x86/64 CI 43154130757](https://github.com/Cloudwhile/ODAC/actions/runs/43154130757) 成功产出四包；`d7b95ec` 的构建成功但内容收集误判失败；修复提交 `90ac2ea`（[43180259567](https://github.com/Cloudwhile/ODAC/actions/runs/43180259567)）与 `c98f0f7`（[43186925580](https://github.com/Cloudwhile/ODAC/actions/runs/43186925580)）均成功，三次构建的四个 APK 逐字节一致。本地 SDK 编译按要求跳过 |
| 官方 `opennds` 与 `odac-opennds` 同时安装时 apk 是否因文件冲突拒绝 | **未验证**。在已装 `odac-opennds` 的 VM 上 `apk add --simulate opennds` 求解通过，说明元数据层面不冲突；模拟不检查文件冲突，真实事务没有执行（官方包的 postinst 可能接管管理 LAN）。由 preflight 拒绝兜底 |
| openNDS 守护进程在 `users_to_router_passthrough 1`、`gatewayfqdn disable`、`fwhook_enabled 0` 下的行为 | VM 上通过临时诊断脚本启用后，nds 规则仅引用 `br-odac`，没有 `firewall.opennds`、WAN6 或 DHCP 配置改动；管理面检查通过。上游 v10.3.1 源码确认正常启动请求三次共享 dnsmasq reload，终止请求一次 restart，上述选项不能抑制。新源码补丁移除 daemon 中 DHCP revert、dnsmasq reload/restart 和全局 nftset 清理/初始化；封装 read-only checker，在 init 与 daemon 内分别拒绝要求这些共享 DNS 功能的配置。补丁严格套用与完整宿主快速检查通过（DNS 策略 5 项、APK 内容 10 项／ELF 8 项）；检查已前移到 config_init 前并覆盖所有 section，后端依赖要求 odac-opennds>=10.3.1-r2。VM 上对新版 checker 源码执行当前配置只读检查；另用 VM 的 `/lib/functions.sh` 执行五个合成配置案例（替换 `config_load`，不是实际 UCI 解析），合法配置接受，隐藏策略、DNS 列表、多个 opennds section 与缺失配置拒绝。没有写入 VM 配置；r1 VM 未安装新版 checker，不等于新二进制验收。新目标为准入 r2／后端 r3；[CI 43265042292](https://github.com/Cloudwhile/ODAC/actions/runs/43265042292) SDK 编译成功，但 ELF 检查发现 `dnsconfig.sh` 字符串而拒绝收集，字符串来源尚未确认，没有上传新版 APK。VM 原版启用和真实受控客户端准入仍未验收 |
| VM 启用、回滚与停用 | r1 原版启用两次在单次 `ndsctl json` 检查处退出 3 并回滚，守卫保持关闭、管理配置不变。无端口的桥虽通过 preflight，openNDS 因无链路无法启动；从官方源安装 kmod-dummy 并仅在运行时将 dummy0 接入 br-odac 后，临时诊断脚本等待 4 秒即启用成功。诊断期间门户 HTTPS 200、管理面检查通过；原版 `--disable` 退出 0，数据库保留、退役守卫加载，openNDS 数秒后退出，随后管理面检查通过。**诊断脚本成功不等于原版启用通过**。r2 改为最多 30 次就绪检查，失败仍回滚；新包 VM 验收待执行 |
| 旧 device-access r2（不含 DNS 补丁）的构建、产物复核与测试签名 | [43242116170](https://github.com/Cloudwhile/ODAC/actions/runs/43242116170) 编译完成，但收集步骤把 LuCI r1 误按后端 r2 验证而失败，未上传 APK。`1705e87` 修正收集与 Release 清单按各自配方读取版本，6 项 workflow 回归通过；[43247282658](https://github.com/Cloudwhile/ODAC/actions/runs/43247282658) 全部构建检查成功，Release 按设计跳过。下载四包后校验和、内容、载荷哈希和 ELF 检查通过（6 个 ELF，LuCI 无 ELF）；用官方 SDK apk 测试签名后严格验签通过，空密钥目录与无关公钥均被拒绝。签名前后解析后的元数据一致，r2 包内 `odac-setup` SHA256 与修复源码一致。未安装到 VM，不代表升级或原版启用验收通过 |
| 真实路由器安装/升级/停用/卸载 | **未验证** |
| OpenWrt 25.12.2 x86/64 测试虚拟机（**不是干净镜像**） | 2026-11-12 用测试签名包实测。首次安装后和重新安装后各做一次管理面检查（新建 SSH、LuCI HTTP 与 HTTPS、LuCI 登录、HTTPS `apk update`、外网），全部通过；卸载后在 VM 上检查了 HTTPS `apk update` 和外网，没有检查 LuCI。**安装**：四个文件经 `apk --keys-dir` 验签后安装，未用 `--allow-untrusted`；安装后 `odac.main.enabled=0`，openNDS 为 `enabled 0`、`gatewayinterface odac-unconfigured`，`opennds` 与 `odacd` 未运行，nft 仍只有 `inet fw4`，network、firewall、dhcp、uhttpd、dropbear 五个配置和 `/lib/libustream-ssl.so` 及其所有者不变，没有包被移除；系统日志有门控记录 `odac-opennds: not started`；`ldd` 对 `odacd`、`odacctl`、`opennds`、`ndsctl` 全部解析成功。**默认配置下启用**：`odac-setup --enable` 被 preflight 拒绝（退出 1），配置、规则集、文件、地址和进程都没有变化，没有生成 ownership，锁已释放；`--disable` 退出 0 且无变化；`--restore-network` 拒绝。**卸载**：`apk del` 四包退出 0，管理配置、TLS 提供者、nft 表和地址不变；未修改的 `/etc/config/odac`、`/etc/config/opennds` 与空的 `/etc/odac` 随包删除。**重新安装**：结果与首次相同。**后续启用实验**见上方独立条目。**未完成**：修复后的原版启用、真实客户端准入、带数据库的卸载保留、版本升级、干净镜像首次安装。VM 上有旧安装的残留（首次安装时 Argon2 库已存在并被同版本替换；`network.odac_device`、`dhcp.odac` 仍在） |
| 真实 LuCI 主题下的管理页渲染 | 2026-11-13 在现有 r1 VM 的 disabled 状态下，七个 ODAC 页面均返回 200；未观察到 pageerror 或 HTTP 资源错误，正确显示后端不可用。1440×900 与 390×844 视口均无横向溢出、七个子菜单可见。未提交表单或修改配置；有数据/启用状态及实际管理操作仍未验证 |
| 限速精度、CPU/内存、并发吞吐、Flow Offloading 影响 | **未测量** |
| Cudy TR3000 实体机部署与回滚 | **未完成**（无实体设备，不以生产机替代测试） |
| CI 优化前后耗时对比 | **未完成同条件对比**；成功构建 `10726da` 的 SDK 构建阶段 2403 s（feeds 77、配置 2、Argon2 6、准入 1095、后端 988、LuCI 235），含依赖编译与打包。相同步骤的另外两次为 2486 s（SDK 与源码缓存命中）和 3105 s，缓存不缩短编译，Runner 波动大。日志显示构建串行（子 make 全为 `-j1`），内核模块打包在三次调用中各重做一遍，GnuTLS 完整编译两次（见[根因分析](root-cause.md)）。改为一次并行 `make`（`435a76e`，[43209085571](https://github.com/Cloudwhile/ODAC/actions/runs/43209085571)，4 任务，缓存未命中）后 x86/64 五次运行的脚本全程为 1205 / 964 / 1340 / 951 / 1156 s，内核模块打包与 GnuTLS 各执行一次，四个 APK 与串行构建逐字节一致。五次都短于最快的串行运行，但 Runner 波动大，不给出稳定的提速比例 |
| APK 文件树与门控源文件元数据 | `90ac2ea` 的检查器对上述历史四包清单通过；9 项回归测试通过。核对 init/默认配置的 SHA256、权限及归属，不等于签名、ELF ABI 或实际安装验证 |
| APK 载荷与 ELF（`scripts/check-apk-elf.py`） | CI 43186925580 在收集步骤对四个 x86/64 APK 执行并通过（本地对前两次产物复核结果相同）：每个文件的内容与清单 SHA256 一致；6 个 ELF 均为 ELF64 LSB x86-64，可执行文件使用 `/lib/ld-musl-x86_64.so.1`；Argon2 库的 SONAME 与文件名一致，`odacd` 的 NEEDED 含该 SONAME；LuCI 包不含 ELF。7 项回归测试通过。动态库在 x86/64 VM 上解析成功（见上），其它架构**未验证** |
| 11 个 target 的构建与 APK 检查 | 手动全矩阵运行 [43214081475](https://github.com/Cloudwhile/ODAC/actions/runs/43214081475)（`8900f71`，非 Tag，发布 Job 按设计跳过）全部成功。下载各 target 产物本地复核：校验和、内容检查、载荷哈希与 ELF 检查全部通过，每个 target 4 个 APK、6 个 ELF。ELF 头与加载器按架构为：x86_64 ELF64 LSB `ld-musl-x86_64`；5 个 aarch64 target ELF64 LSB `ld-musl-aarch64`；3 个 arm target ELF32 LSB `ld-musl-armhf`；mipsel ELF32 LSB `ld-musl-mipsel-sf`；mips ELF32 MSB `ld-musl-mips-sf`。LuCI 包在 11 个 target 上逐字节相同。这是该矩阵产物的构建检查；后续 x86/64 VM 的独立验收见上，其它 target 未做安装或运行验证 |
| 包签名 | **只有测试签名，发布签名未实现**。CI 产物不带签名块（SDK 调用 `apk mkpkg` 时没有签名参数），直接安装只能靠 `--allow-untrusted`。验收用已校验官方 SDK 自带的 apk-tools 3.0.5 和一次性测试密钥签名：`adbsign` 读取未签名输入时必须带 `--allow-untrusted`，否则报 `UNTRUSTED signature` 却仍退出 0；签名后每个包只凭测试公钥严格验签通过，对空密钥目录和无关密钥均被拒绝，内容与 ELF 检查结果不变。测试私钥只留在签名主机，不在仓库、日志或产物中。正式发布需要项目密钥或带签名索引的仓库，尚未设计 |

未验证项意味着不能据此声称「已在真实设备上可用」。

> 本表描述的是**当前工作区**的验证状态。历史条目（如「早期版本曾实测」）
> 不构成对当前代码的验收。

## 本次整改改动摘要

| 文件 | 变更要点 |
| --- | --- |
| `device-access/Makefile` | 移除 `+libustream-mbedtls` 硬依赖，改为不依赖任何 libustream 变体；`+opennds` 改为 `+odac-opennds` |
| `odac-opennds/`（新增） | 用 openNDS 10.3.1 官方源码与哈希构建的准入后端；不安装 uci-defaults、`restart.sh` 和官方 init；门控 init 只在 ODAC 已启用且受控接口已被 guard 时启动守护进程，不改 WAN6、不插 fw4 规则 |
| `device-access/files/usr/libexec/odac-preflight` | TLS 提供者缺失或不可加载为硬错误（无人拥有只警告）；只接受 `odac-opennds`，拒绝官方 opennds |
| `device-access/files/usr/libexec/odac-setup` | 全程持生命周期锁；变更前断言管理接口未被捕获；先记录所有权（`enable_pending`）再改动；回滚逐字节恢复 `/etc/config/opennds` 与退役 guard，回滚不完整则保留所有权；核对 WAN6；`--disable` 复用 `odac-remove`；`--restore-network` 要求 IFF_UP 已清除并核对 guard 归属 |
| `device-access/files/usr/libexec/odac-remove` | 失败即停的顺序化拆除；先校验全部段的内容哈希再删除，被改过的段拒绝删除；成功后才清除所有权记录 |
| `device-access/files/usr/libexec/odac-ownership` | 变量在子 shell 中运行（不再污染调用方变量）；键名与单行值校验；新增防火墙段未修改校验 |
| `device-access/files/usr/libexec/odac-purge` | 改为只删数据的明确文件清单；默认备份；多重确认；活动/受管状态下一律拒绝 |
| `device-access/files/etc/uci-defaults/60_odac-opennds-safe-default` | 删除：它在 openNDS postinst 之后运行，拦不住自动启动和 WAN6 改写 |
| `.github/workflows/build-apk.yml` | `scope` Job 解析真实矩阵；`fast-check` 前置；缓存键绑定 target/arch；SDK 校验改为唯一条目 + 摘要比对；APK 元数据校验；Release 草稿 + 回读校验 |
| `scripts/targets.txt`、`scripts/resolve-scope.sh`、`scripts/verify-sdk.py`、`scripts/fast-check.sh` | 新增：架构唯一清单、范围解析、SDK 校验、快速校验入口 |
| `tests/*` | 新增/扩展：workflow 契约与变异回归、范围解析、生命周期 256 项、构建脚本 25 项、启用事务 10 项 |

## 构建

使用与目标设备**完全一致**的发行版、target、subtarget 的 Linux x86_64 SDK，
解压前先按发行版 `sha256sums` 校验。

本项目使用的 SDK target：`mediatek/filogic`（Cudy TR3000 v1）与 `x86/64`（测试虚拟机）。
两者的 `include/version.mk` 都是 25.12.2。

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

### 一条命令构建

~~~sh
scripts/build-sdk.sh "$SDK_DIR" mediatek filogic
~~~

脚本会：校验 SDK 目录名与 target 一致 → `feeds update` base、packages、luci 三个 feed →
用 `feeds install -p <feed> <package>` 精确安装所需配方 → 写入最小 `.config` →
`make defconfig` → 校验选择结果（并拒绝选中官方 `opennds`）→ 编译四个包。

> **`feeds install` 的语法**：写成 `feeds install base/libjson-c` 是**无效**的。
> `scripts/feeds` 会把整个参数当作包名查找，找不到时只打印一句
> `WARNING: No feed for package ... found` 并**仍然返回 0**，构建会带着一个
> 不完整的包集合继续。正确写法是 `feeds install -p base libjson-c`。

### 手工步骤

`SRC_DIR` 指向本仓库根目录：

~~~sh
SRC_DIR=/path/to/ODAC
mkdir "$SDK_DIR/package/odac"
for pkg in argon2 odac-opennds device-access luci-app-device-access; do
	cp -R "$SRC_DIR/$pkg" "$SDK_DIR/package/odac/"
done
cd "$SDK_DIR"
./scripts/feeds update base
./scripts/feeds update packages
./scripts/feeds update luci
./scripts/feeds install -p base libjson-c
./scripts/feeds install -p base ca-bundle
./scripts/feeds install -p base uhttpd
./scripts/feeds install -p base firewall4
./scripts/feeds install -p base nftables-json
./scripts/feeds install -p base rpcd
./scripts/feeds install -p base uci
./scripts/feeds install -p base ubus
./scripts/feeds install -p base jshn
./scripts/feeds install -p base jsonfilter
./scripts/feeds install -p base openssl-util
./scripts/feeds install -p packages libsqlite3
./scripts/feeds install -p packages curl
./scripts/feeds install -p packages conntrack
./scripts/feeds install -p packages libmicrohttpd-no-ssl
./scripts/feeds install -p luci luci-base
./scripts/feeds install -p luci rpcd-mod-luci
printf '\nCONFIG_USE_APK=y\nCONFIG_PACKAGE_libargon2=m\nCONFIG_PACKAGE_odac-opennds=m\nCONFIG_PACKAGE_device-access=m\nCONFIG_PACKAGE_luci-app-device-access=m\n' >> .config
make defconfig
make package/odac/argon2/compile package/odac/odac-opennds/compile package/odac/device-access/compile package/odac/luci-app-device-access/compile V=s
~~~

**不要**执行 `feeds install -a`：它会安装每个 feed 的每个配方，把依赖图扩大
到与 ODAC 无关的范围。

**不要**安装任何 libustream 变体。三个变体互斥且都安装
`/lib/libustream-ssl.so`，选一个就等于替系统决定了 TLS 后端。
`device-access` 本身不链接 libustream，系统原有的提供者应保持不变。

### 打包注意点

1. 官方源 `libjson-c` 的 pkg-config 文件在 SDK 中路径不可用，因此 `device-access` 的 `CMakeLists.txt` 用显式 include/library 路径，不依赖 pkg-config 查找。
2. 官方源没有 Argon2，`argon2` 配方自带上游 tarball（SHA256 已核对），导出 `libargon2.so.1` 与 `argon2.h`。配方目录固定为 `package/argon2/`，不要用包名当目录名。
3. LuCI 包是普通 `package/` 包，直接复制到 `$SDK_DIR/package/luci-app-device-access`；不要再放进 `feeds/luci`。
4. `CMakeLists.txt` 不能自带 `-D_FORTIFY_SOURCE=2`：SDK 的交叉编译参数已经带 `-D_FORTIFY_SOURCE=1`，重复定义会被 `-Werror` 当作错误。加固与优化参数改为仅本机构建时追加。
5. 纯静态的 LuCI 包需要显式的空 `Build/Compile` 目标，否则构建会去找一个不存在的 Makefile。

后端不再链接 libcurl：上游状态由管理员脚本写入状态文件，ODAC 只读该文件。

### 关于 gettext / gnulib

成功构建 `10726da` 的日志确认 gettext/gnulib 首次宿主编译发生在准入包阶段，
GnuTLS 使用 gettext 的 autopoint；gettext 后续目标访问只需 0.35 s，
并未重复完整编译。GnuTLS 第二次编译的触发原因、完整 Make 依赖边和跨 Job
宿主缓存收益仍待验证。现有缓存仅覆盖 SDK 归档与下载源码；不能据此声称
编译已显著提速，也不能删除依赖来让测试通过。详见 `docs/root-cause.md`。

## CI

### 触发与范围

| 触发类型 | 范围 | 依据 |
| --- | --- | --- |
| Pull Request | 仅 `x86/64` | `scope` Job 的 `pull_request` 分支 |
| `main` 分支 Push | 仅 `x86/64` | `scope` Job 的 `push` + `ref_type != tag` |
| 版本 Tag（`v*`） | 全部 11 个架构 | `scope` Job 的 `push` + `ref_type == tag` |
| 手动 `workflow_dispatch` | 默认仅 `x86/64`，`build_scope=full` 时全部 | `inputs.build_scope` |

范围由 `scope` Job 解析后**输出到矩阵**，构建 Job 用
`matrix: ${{ fromJSON(needs.scope.outputs.matrix) }}` 消费它。这与「用一个
`if:` 步骤打印 skipping 然后继续」有本质区别：后者仍会为每个架构分配
runner、下载 SDK 并执行后续步骤。

范围解析由 `scripts/resolve-scope.sh` 实现，workflow 与测试共用同一份实现。
架构清单在 `scripts/targets.txt`（11 个架构的唯一来源），构建矩阵、Release
完整性检查与 workflow 校验都读这一个文件。

`bcm27xx/bcm2710` 与 `mediatek/filogic` 的 `arch` 同为 `aarch64_cortex-a53`。
**二进制架构相同不等于目标平台兼容**：内核模块与目标平台相关的包不同。
产出的 APK 文件名带 `target-subtarget`，因此两个目标的产物不会互相覆盖。

### Job 依赖

```
scope ──┬─→ fast-check ─→ build ─→ release
        └─────────────────↗
```

`fast-check` 失败时不会下载任何 SDK。

### SDK 下载与校验

`scripts/verify-sdk.py` 从官方 `sha256sums` 中**唯一**选出目标 SDK 的归档条目
（0 条或多条都失败），接受 sha256sum 的二进制标记（`*`）与文本标记（空格），
并逐字节计算归档的 SHA256 与官方清单比对。清单每次运行都重新下载，
**缓存命中不跳过校验**。

不用 `sha256sum -c -` 管道：空输入会让该命令退出 0，检查会在没有校验任何
内容的情况下「通过」。

### APK 校验

构建 Job 不信任文件名，`apk adbdump` 读出的真实元数据要与预期比对：

| 检查项 | 规则 |
| --- | --- |
| 包名 | `libargon2` / `odac-opennds` / `device-access` / `luci-app-device-access` |
| 架构 | 前三个等于目标 `arch`；LuCI 包等于 `noarch` |
| 版本 | 与各自 Makefile 的 `PKG_VERSION` / `PKG_RELEASE` 一致 |

架构无关的包声明的是 `noarch` 而**不是** `all`：`package-pack.mk` 在
`PKGARCH` 含 `all` 时写入 `arch:noarch`。

`libargon2`（`20190702-r1`）和 `odac-opennds`（`10.3.1-r1`）的版本与 ODAC 两个包不同，
因此不能用一个统一版本号去校验四个包。

包目录不写死：`FeedPackageDir` 可能是按 feed 划分的目录，也可能是目标目录，
因此在 `bin/` 下查找，再按真实元数据筛选，避免把 SDK 里其他包混入 Release。

### Release 保护

- `release` Job 要求 `github.event_name == 'push'`、`refs/tags/v*` 前缀，
  且 scope 为 `full`。
- `release` Job 是唯一拥有 `contents: write` 的 Job；workflow 默认权限是
  `contents: read`，因此 PR 永远没有 Release 写权限。
- 发布前校验**每一个**架构的**每一个**包都在附件集合中；缺一个即失败，
  不发布部分集合。附件数量也与期望数量逐一比对。
- **先建草稿再校验**：Release 以 draft 创建，上传后把整个资产集下载回来，
  比对文件清单与校验和清单，全部一致才转为可见的 prerelease。失败时留下的
  仍是草稿，不会产生半发布状态。
- 已存在的 Release 直接拒绝，不覆盖、不追加。

### 缓存与计时

| 缓存 | Key | 内容 |
| --- | --- | --- |
| SDK 归档 | `sdk-<版本>-<target>-<subtarget>-<arch>-<脚本哈希>` | 已下载的官方 SDK 压缩包 |
| 源码下载 | `dl-<版本>-<target>-<subtarget>-<arch>-<脚本哈希>` | SDK 的 `dl/` 目录 |

缓存键绑定 target 与 arch，跨架构的构建目录不会被复用。缓存的 key 使用
`${{ matrix.* }}` 上下文，不能写成 shell 变量：Action 的 `with:` 输入不做
shell 展开。

Job Summary 写入每个架构的：SDK 大小、宿主环境准备耗时、SDK 下载与校验耗时、
解压耗时、编译总耗时、产物收集耗时、两个缓存的命中状态、编译与校验的结果
状态，以及每个 APK 的大小与 SHA256。`scripts/build-sdk.sh` 按阶段计时
（feeds、配置、四个包的编译与打包、包发现）并同时记录该阶段的退出状态，
失败阶段不会被记成成功耗时。

SDK 构建的完整日志写入 `diagnostics/sdk-build.log`，始终作为 Artifact 上传，
不直接铺满 Actions 页面。

## 安装

~~~sh
apk add libargon2
apk add odac-opennds
apk add device-access
apk add luci-app-device-access
~~~

安装后系统仍是关闭状态，不启用准入控制。

安装不改动网络配置。`odac-opennds` 的 init 脚本会被 `default_postinst`
enable 并 start，但在 ODAC 启用之前它只记录一条日志并返回 0，不启动守护进程、
不改 `network.wan6`、不改 fw4（见 `docs/root-cause.md` 根因 1）。

**已安装官方 `opennds` 包的机器**：ODAC 不接管它，`odac-setup --enable` 的预检
会拒绝。先由管理员决定如何处理现有的 openNDS 实例，并确认它没有占用管理接口：

~~~sh
nft list table ip nds_filter 2>/dev/null | grep iifname
~~~

### 安装后自检

~~~sh
apk info -W /lib/libustream-ssl.so   # TLS 提供者不应改变
apk list --installed 'libustream-*'
uci get opennds.@opennds[0].gatewayinterface   # 应为 odac-unconfigured
uci -q get network.wan6.proto                  # 应与安装前相同
pidof opennds || echo not-running              # 启用前不应运行
~~~

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

`odac-setup` 的顺序是：预检 → **确认管理接口未被 openNDS 捕获** →
安装故障关闭守卫 → 写配置 → `fw4 check` → `fw4 reload` → 启动服务 →
HTTPS 证书指纹比对与门户请求 → `ndsctl` 与 nft 表检查。

任何一步失败都会：回滚 openNDS 的**全部**被改选项、删除本次创建的防火墙段、
停用 ODAC、保留守卫。受控网络保持拒绝。

**启用顺序不可颠倒**：守卫必须在 openNDS 之前就位，否则会出现短暂放行窗口。

HTTPS 检查失败是**硬错误**，不会降级为明文 HTTP 继续。

## 停用与卸载

~~~sh
/usr/libexec/odac-setup --disable
~~~

停用会关闭守卫、停止 ODAC 与（属于 ODAC 的）openNDS、删除 ODAC 创建的
防火墙段。受控网络保持关闭，退役守卫保留。

~~~sh
apk remove device-access
~~~

卸载触发 `odac-remove --retain-guard`：先写入持久化的退役守卫
（`/etc/nftables.d/90-odac-retired.nft`，只作用于受控接口），再停服务、
删除 ODAC 的防火墙段。**数据库与备份保留**，受控网络保持关闭。

退役守卫是唯一在包被移除后仍能保护受控网络的东西。手工移除前必须确认
受控接口已下线：

~~~sh
/usr/libexec/odac-setup --restore-network
~~~

该命令会拒绝在受控接口仍然 up 时移除守卫。

### 数据清理

~~~sh
/usr/libexec/odac-purge --dry-run      # 列出将被删除的数据文件
/usr/libexec/odac-purge --backup       # 生成私有快照
/usr/libexec/odac-purge --confirm --confirm-data
~~~

清理只删除 `/etc/odac/odac.db`（含 WAL/SHM）与 `/etc/config/odac`，
**不**递归清空 `/etc/odac`：管理员的密钥、备份与其它文件都保留。
防火墙段与退役守卫由卸载流程负责，清理不碰它们。
ODAC 仍在运行、或仍有未退役的防火墙段时，清理被拒绝。

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
| --- | --- |
| 守护进程崩溃或被杀 | 守卫租约最长 90 秒后自然过期，转发关闭。procd 会自动重启。 |
| 防火墙重载 | 守卫位于独立 nft 表，`fw4` 重载后仍在。 |
| `fw4` 规则被误删 | 重新执行 `odac-setup --enable` 或 `/usr/libexec/odac-firewall init ...`。 |
| 撤销授权失败 | 设备进入待重试队列，转发保持关闭；每次心跳重试，直到成功。 |
| 需要恢复接入 | `/etc/init.d/odac stop` 后按启用步骤重跑 `odac-setup --enable`。 |
| 数据库损坏 | 用 LuCI 系统设置中的备份文件恢复，或直接放置 `/tmp/odac-restore.json` 后调用 `restore`。 |

## 设备上的已知限制

- OpenWrt 25.12 的 busybox **不含 `stat` applet**。项目脚本已改用 `ls -ld`，但自写的运维脚本若依赖 `stat -c`，需改用 `ls -ld` 或安装 `coreutils-stat`。
- 受控桥必须具有链路；无端口的桥可能通过当前 preflight，但 openNDS 不会就绪。
- 停用命令返回后，openNDS 可能还需数秒退出；期间退役守卫保持关闭。opennds 配置仍可为 enabled=1，启动门控另行检查 ODAC 的 enabled=0。
- VM 诊断启用期间观察到 openNDS 监听 `0.0.0.0:2050`。上游 v10.3.1 的 `start_mhd()` 未指定监听地址，因此符合其通配监听实现；日志打印受控地址不代表 socket 已绑定该地址。请求级接口判断不能代替网络隔离，实际可达范围仍需客户端验证。
- 上游 DNS helper 的 `revert` 对整个 DHCP 配置执行 `uci revert dhcp`，且关闭 option 114 仍会进入删除配置路径；VM 中 DHCP 文件未变不能推广为绝不触碰 DHCP。共享 dnsmasq 的实际影响与失败日志原因未确认，源码调用链见[根因分析](root-cause.md#未验证)。
- 从未启用过的系统上执行 `/etc/init.d/odac stop`（卸载时也会触发）会打印一行
  `odac-firewall: invalid request or unavailable guard`，退出码仍为 0：此时没有守卫表可关闭，不影响卸载。
- uhttpd 的 CGI 模式不实现 `Expect: 100-continue` 握手。用 curl 向门户 CGI 发 POST 时需加 `-H "Expect:"`，否则请求体不会送达 CGI。
- openNDS 10.3.1 在 OpenWrt 25.12 上启动时输出 `uci: Invalid argument`（其 ipset/nftset 配置路径）。原始官方 openNDS 配置同样复现，属上游不兼容，不影响 ODAC 的守卫与授权接口。

## 安全约束

- 未认证设备默认拒绝访问 WAN；认证网络 IPv6 转发被显式拒绝。
- 管理网络与认证网络必须隔离；受控网络无法访问路由器管理接口与 SSH。
- 账号密码只经 HTTPS 传输。未受信任的自签名证书会让客户端无法验证服务器身份；把该证书加入客户端信任，或使用客户端信任的私有 CA 签发的证书，都可以正常工作。
- MAC 白名单只是便捷准入，不能证明设备身份，会被随机 MAC 或 MAC 伪造绕过。
- 限速是 openNDS 的平均速率控制，不保证瞬时带宽上限。
- 会话只保存在内存，路由器重启后需要重新登录；白名单与限速策略保留在数据库中。
