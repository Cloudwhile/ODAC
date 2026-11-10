# Environment evidence and safe collection

ODAC targets OpenWrt 25.12.x with APK packages. Build with the official Linux x86_64 SDK for the exact release, target and subtarget of the router, after checking the archive against the release `sha256sums`.

Validated SDK targets for 25.12.2:

- `mediatek/filogic` for Cudy TR3000 v1 (`aarch64_cortex-a53`).
- `x86/64` for a staging virtual machine.

Both SDKs report `VERSION_NUMBER` 25.12.2 and `CONFIG_USE_APK=y`. A staging virtual machine is suitable for package and service checks; it is not a substitute for acceptance on the production router.

Run `scripts/probe-environment.sh` locally on a staging router. It prints release/target/architecture, selected APK versions, native API help, memory/filesystem totals and explicitly selected non-secret UCI values. It never dumps all UCI, environment, process arguments, logs, ndsctl client JSON, upstream login scripts, credentials or cookies. It does not install packages, mutate UCI, restart services or contact external endpoints. Review even this reduced report before sharing; interface names and gateway/hostname are operational metadata.

Dependency source checks against official openwrt-25.12 branches found libsqlite3, libjson-c, curl, opennds (routing feed, 10.3.1-r3), conntrack (packages/net/conntrack-tools) and nftables-json (provides nftables). TLS additionally requires libustream-mbedtls plus operator certificates; curl and openssl-util are used by setup/preflight only, and the backend does not link libcurl. Argon2 is absent in both official base/packages trees; the project's separate `argon2` recipe builds the `libargon2` package. Feed branch state does not establish the firmware's exact installed versions; collect the probe first.

The WAN adapter must not interpret upstream helper process presence as ONLINE. Account data and TLS private keys remain root-only.
