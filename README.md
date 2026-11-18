# ewyauth

> Runtime patch for the `ewy` / `25y` 4.0.6 theme runtime (PHP 7.2 – 7.4, NTS).
> A small PHP extension that neutralises the two independent licensing gates and stops the
> admin-path reporting call — **without touching any SourceGuardian-encrypted file**.

[![release](https://github.com/YeXuanHs/ewyauth/actions/workflows/release.yml/badge.svg)](https://github.com/YeXuanHs/ewyauth/actions/workflows/release.yml)

---

## 背景

`ewy`（后台运行时）与 `themes/*`（前台主题）的授权是**两套互不相干的链路**：

| 范围 | 入口 | 判定依据 | 未通过表现 |
|---|---|---|---|
| 前台 `themes/web`、`themes/cart`、`themes/clientarea` | `_auth_check()` → `verifyAuthorization()` | 远程/缓存返回 `authorized === true` | `403 未授权访问` |
| 后台 `ewy/*`（全部页面） | `ewy/header.php` | `$_SESSION['auth'] === true`，否则 `SELECT state FROM auth_25y` | `302 → ./?page=auth` |

两条链路都在 SourceGuardian 加密文件里，源码不可改；但判定动作都是**普通函数调用 / 会话状态**，
因此可以在运行时接管，不需要解密，也不需要伪造远程签名。

## 做什么

| # | 接管点 | 处理 |
|---|---|---|
| 1 | `verifyAuthorization()` | 直接返回 `['authorized' => true, 'message' => ...]`，函数体不执行 → 不连库、不发包 |
| 2 | `session_start()` | 会话启动后写入 `$_SESSION['auth'] = true` → 后台 `header.php` 不再查 `auth_25y`、不再跳绑定页 |
| 3 | `reportAuthorizedInstallation()` + `POST /api/client/report/` | 方法级短路返回 `ok=true`；curl 层再把命中的 URL 改写成不可达地址兜底 → **后台目录名不会上报** |

## 支持的版本

| PHP | 架构 | 状态 |
|---|---|---|
| 7.2 NTS | x86_64 Linux | CI 构建 |
| 7.3 NTS | x86_64 Linux | CI 构建 |
| 7.4 NTS | x86_64 Linux | CI 构建（主验证版本） |

ZTS 未验证。PHP 8 未适配（`zend_execute_ex` 与内部函数 handler 的调用约定不同）。

产物只依赖 `libc.so.6`，最高符号版本 `GLIBC_2.4` —— CentOS 7 / Debian 9 及以上可直接加载，
不受 CI 构建机（Ubuntu 22.04）的 glibc 版本限制。

## 安装

从 [Releases](https://github.com/YeXuanHs/ewyauth/releases) 下载对应 PHP 版本的 `ewyauth-<ver>-nts-linux-x86_64.so`，
放进 `extension_dir`，然后 **在 `ixed.<ver>.lin` 之后**追加：

```ini
extension=/www/server/php/74/lib/php/extensions/no-debug-non-zts-20190902/ixed.7.4.lin
extension=/www/server/php/74/lib/php/extensions/no-debug-non-zts-20190902/ewyauth.so

ewyauth.enable       = 1
ewyauth.block_report = 1
ewyauth.session_auth = 1
ewyauth.force_remote = 0
ewyauth.log          = /tmp/ewyauth.log
```

```bash
systemctl restart php-fpm-74
php --ri ewyauth
```

> 顺序很重要：`ixed` 必须先加载。本扩展只包 `zend_execute_ex`，不依赖 SG 内部实现，
> 但 SG 的解密产物要先存在。

## 配置

| 键 | 默认 | 说明 |
|---|---|---|
| `ewyauth.enable` | `1` | 总开关 |
| `ewyauth.block_report` | `1` | 拦截上报方法 + curl 层兜底 |
| `ewyauth.session_auth` | `1` | 后台 `$_SESSION['auth']` 提权 |
| `ewyauth.force_remote` | `0` | 同时短路 `LicenseSdkV2::verify` / `HuanwareLicenseClient::verify` `bind`（后台「绑定授权」页需要时再开） |
| `ewyauth.trace` | `0` | 记录每次用户函数调用（排障，量大） |
| `ewyauth.log` | 空 | 日志路径，空 = 不写 |
| `ewyauth.message` | `缓存授权验证成功` | 返回给前台的 message 文案 |

## 验证

```bash
# 1. 模块
php --ri ewyauth

# 2. 前台（应为 200，不再是 403）
curl -sI https://your-host/ | head -1

# 3. 后台（应为 200，不再 302 到 ?page=auth）
curl -s -o /dev/null -w '%{http_code} %{redirect_url}\n' https://your-host/<admin-dir>/

# 4. 日志
grep -E 'force verifyAuthorization|session_start|block report' /tmp/ewyauth.log
```

日志样例：

```
[1795044793] session hook installed
[1795044793] session_start: $_SESSION['auth'] = true
[1795044806] force verifyAuthorization() => authorized=true
[1795044793] block reportAuthorizedInstallation() [admin_path not sent]
[1795044793] block curl url: https://license.rain.mo.cn/api/client/report/
```

## 从源码构建

```bash
phpize
./configure --with-php-config=$(which php-config)
make -j"$(nproc)"
ls modules/ewyauth.so
```

或直接执行脚本（按本机 PHP 版本命名产物）：

```bash
./build.sh
# dist/ewyauth-7.4-nts-linux-x86_64.so
```

## CI

`.github/workflows/release.yml`：

- push tag `v*` → 矩阵构建 PHP `7.2 / 7.3 / 7.4`，冒烟测试 `extension_loaded('ewyauth')`，产物打到 Release
- 支持 `workflow_dispatch` 手动构建（只出 artifact，不发 Release）

## 实现要点

- `ewy_execute_ex()`：包 `zend_execute_ex`，按 `function_name` 匹配后**直接写 `execute_data->return_value` 并跳过函数体**
- `ewy_session_start()`：替换 `session_start` 的 `zend_internal_function.handler`，原函数成功后再写 `$_SESSION`
- `ewy_curl_setopt()` / `ewy_curl_setopt_array()`：命中 `CURLOPT_URL(10002)` 且含 `/api/client/report/` 时改写为 `127.0.0.1:1`
- `find_internal()`：从 `CG(function_table)` / `EG(function_table)` 取内部函数并替换 handler 指针（PHP 7 的 ICALL fastcall 路径不经过 `zend_execute_internal`）

## 注意

- 不写数据库、不改磁盘文件、不影响 `ewy` 之外的业务 curl，只拦 `/api/client/report/` 这一条 URI
- 与 opcache 无冲突（不重写 op_array，只包执行入口）
- `cli` 验证通过 ≠ `fpm` 生效，必须重启 FPM 后再确认
- 仅用于你自有或已获授权部署的环境
