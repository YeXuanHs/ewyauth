# ewyauth

`ewy` / `25y` 模板运行时的授权门禁补丁，适配 4.0.0 及以上版本，PHP 扩展，支持 7.2 / 7.3 / 7.4（NTS，x86_64 Linux）。

不修改任何 SourceGuardian 加密文件，只在实际运行时接管判定逻辑。

---

## 功能

| 范围 | 处理 |
|---|---|
| 前台授权校验 | `verifyAuthorization()` 返回 `authorized = true`，函数体不执行，不查库、不发请求 |
| 后台登录门禁 | 会话建立后写入 `$_SESSION['auth'] = true`，后台不再跳转绑定页 |
| 绑定授权页 | `LicenseSdkV2::verify`、`HuanwareLicenseClient::verify` / `bind` 一律返回成功，任意卡密可提交 |
| 后台路径上报 | `reportAuthorizedInstallation()` 返回成功且不执行，`/api/client/report/` 请求在 curl 层兜底拦截 |

---

## 工作原理

### 授权判定位置

- 前台：`themes/{web,cart,clientarea}/25y/controller/common.php` 的 `_auth_check()` 调用 `verifyAuthorization()`，结果不是 `authorized === true` 就返回 403。
- 后台：`ewy/header.php` 检查 `$_SESSION['auth']`，未通过时才查询 `auth_25y.state`，仍不通过则跳转 `?page=auth`。

### 接管方式

SourceGuardian 17 在 hybrid 模式下执行加密代码时不经过 Zend 的 `zend_execute_ex`，因此无法在函数入口处拦截。本扩展改为在**内部函数**层面接管：

1. 替换 `function_exists()` 的 handler。`_auth_check()` 在调用 `verifyAuthorization()` 之前会先执行 `function_exists('verifyAuthorization')`，此时把目标函数的 `zend_op_array` 改写为一条 `ZEND_RETURN`，返回预置数组。补丁幂等，已改写过的函数不会重复处理。
2. 替换 `session_start()` 的 handler，会话启动后写入授权标记。
3. 类方法无法用 op_array 改写生效，改为把类函数表中的条目替换为内部函数桩，直接返回成功结果。
4. 替换 `curl_setopt()` / `curl_setopt_array()` 的 handler，重写命中上报接口的 URL。

以上都只替换函数表中的 handler 指针，不修改磁盘文件、不写数据库。

---

## 支持版本

| PHP | 架构 |
|---|---|
| 7.2 NTS | x86_64 Linux |
| 7.3 NTS | x86_64 Linux |
| 7.4 NTS | x86_64 Linux |

- 产物仅依赖 `libc.so.6`，最高符号版本 `GLIBC_2.4`，CentOS 7 / Debian 9 及以上可直接加载
- ZTS 未验证
- PHP 8 未适配

---

## 安装

下载对应版本的 `.so`，放入 `extension_dir`，在 `php.ini` 中追加（必须在 `ixed` 之后）：

```ini
extension=/www/server/php/74/lib/php/extensions/no-debug-non-zts-20190902/ixed.7.4.lin
extension=/www/server/php/74/lib/php/extensions/no-debug-non-zts-20190902/ewyauth.so

ewyauth.enable       = 1
ewyauth.block_report = 1
ewyauth.session_auth = 1
ewyauth.log          = /tmp/ewyauth.log
```

```bash
systemctl restart php-fpm-74
php --ri ewyauth
```

命令行与 FPM 使用各自的 `php.ini`，需要分别配置。

---

## 配置项

| 键 | 默认 | 说明 |
|---|---|---|
| `ewyauth.enable` | `1` | 总开关 |
| `ewyauth.block_report` | `1` | 拦截路径上报 |
| `ewyauth.session_auth` | `1` | 后台会话授权标记 |
| `ewyauth.log` | 空 | 日志路径，留空不写；也支持环境变量 `EWYAUTH_LOG` |
| `ewyauth.message` | `缓存授权验证成功` | 前台返回的提示文案 |

---

## 验证

```bash
php --ri ewyauth

curl -sI https://your-host/ | head -1
curl -s -o /dev/null -w '%{http_code}\n' https://your-host/<后台目录>/

grep -E 'patched|session|replaced|block' /tmp/ewyauth.log
```

日志示例：

```
[ewyauth] patched verifyAuthorization
[ewyauth] replaced LicenseSdkV2::verify with internal stub
[ewyauth] $_SESSION['auth'] = true
[ewyauth] block curl https://example.com/api/client/report/
```

---

## 构建

```bash
phpize
./configure --with-php-config=$(which php-config)
make -j"$(nproc)"
```

产物位于 `modules/ewyauth.so`。

CI（`.github/workflows/release.yml`）在推送 `v*` 标签时按 7.2 / 7.3 / 7.4 矩阵构建，执行 `extension_loaded` 冒烟测试，并将产物与 `SHA256SUMS` 发布到 Release。

---

## 说明

- 仅修改请求内的函数表与 op_array 内存，不落盘
- 补丁幂等，重复触发不会重复改写
- 不修改 `zend_execute_ex`，与同类扩展无冲突
- 与 opcache 无冲突（加密代码的 op_array 不经过 opcache 缓存）
- 仅用于自有或已获授权部署的环境
