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
├── docs/                     部署与恢复说明
├── tests/                    核心、边界与平台测试
└── scripts/                  SDK 构建与环境探测辅助
~~~

## 架构

| 组件 | 位置 | 职责 |
|---|---|---|
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

## 构建与测试

SDK 打包入口是 `scripts/build-sdk.sh`。先按官方 `sha256sums` 校验并解压与路由器一致的 Linux x86_64 SDK（25.12.2 `mediatek/filogic`，放在大小写敏感文件系统），确保 SDK 内已有 `.config`：

~~~sh
scripts/build-sdk.sh /path/to/sdk mediatek filogic
~~~

脚本把三个包复制到 `package/odac/`，更新 SDK feed 后编译。手工步骤见 `docs/deployment.md`。

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

## 已知限制

- 启用前必须提供客户端信任的 TLS 证书。未受信任的自签名证书会让客户端无法验证服务器身份；把该证书加入客户端信任，或使用客户端信任的私有 CA 签发的证书，都可以正常工作。
- 上游认证失效到管理员脚本写入状态之间存在短暂窗口，ODAC 不自行探测。
- 首版以「禁止认证网络 IPv6 转发」防绕过，不是完整双栈准入。
- 限速是平均速率控制，不保证瞬时带宽上限。
- 会话只在内存中，路由器重启后需重新登录。
- OpenWrt 25.12 提供 json-c 0.18，其严格模式用带符号 `char` 比较控制字符，会把 UTF-8 高位字节误判为非法，导致中文门户文案无法保存或读回（上游在 0.19 修复）。`src/json_text.c` 在严格解析前把字符串内的原始 UTF-8 转成 `\uXXXX` 转义来规避，保留严格模式对控制字符、注释与尾随垃圾的拒绝。
- 真实路由器上的安装、验收与性能尚未验证。
