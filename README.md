# ewyauth

`ewy` / `25y` 4.0.6 模板运行时的授权门禁补丁，一个 PHP 扩展（7.2 / 7.3 / 7.4，NTS，x86_64 Linux）。

不改任何一个 SourceGuardian 加密文件，不伪造远程签名，不劫持 DNS。

---

## 一、原理

### 1.1 授权判定在哪

这套模板的授权是**两条互不相干的链路**：

| 范围 | 入口 | 判定依据 | 未通过表现 |
|---|---|---|---|
| 前台（`themes/web`、`themes/cart`、`themes/clientarea`） | 控制器 `_auth_check()` → `verifyAuthorization()` | 远程/缓存校验结果 `authorized === true` | `403 未授权访问` |
| 后台（`ewy/*` 全部页面） | `ewy/header.php` | `$_SESSION['auth'] === true`，否则查 `auth_25y.state` | `302 → ./?page=auth` |

源头都是 SG 加密文件，磁盘上是密文。

### 1.2 为什么不能钩 `zend_execute_ex`

按理说包一层 `zend_execute_ex` 就能在函数执行前拦截。**但在这套 SG 上不成立**：

SourceGuardian 17 在 `enable_vm_hybrid=1` 下虽然产出标准 `zend_op_array`，**执行却走它自己的解释器** —— SG→SG 的函数调用完全不经过 Zend 的 `execute_ex`。实测：整条 `_auth_check() → verifyAuthorization()` 调用链里，`zend_execute_ex` 一次都没被调用；而用 PHP 代码主动去调同一个 SG 函数时，钩子又能命中。

结论：**函数名级的执行钩子对 SG 内部调用无效**，必须换层面。

### 1.3 走 `function_exists` + op_array 补丁

`_auth_check()` 的实现里，调用前一句正好是判断：

```php
if (function_exists("verifyAuthorization")) {
    $cv8 = verifyAuthorization();     // ← 紧接着就调了
```

而 `function_exists()` 是**内部函数**，SG 调内部函数是走函数表 handler 的 —— 这个钩得住。

所以：钩 `function_exists()`，在它被调用的那一刻，把目标函数的 `zend_op_array` **原地压成一条 `ZEND_RETURN`**：

```
literals[0]  -> ['authorized' => true, ...]
opcodes[0]   -> ZEND_RETURN  CONST(literals[0])
last         -> 1
```

补丁做完后再调 `verifyAuthorization()`，函数体根本不执行，直接返回我们的数组。这跟 Swoole 那套把 `templateAuthorization` 改成 `ZEND_RETURN true` 是同一招，只是触发点从"函数入口"换成了"函数被问到的那一刻"。

补丁是**幂等**的：`last == 1 && opcode == ZEND_RETURN` 就直接跳过，不会重复改。

---

## 二、做了什么

| # | 接管点 | 处理 |
|---|---|---|
| 1 | `function_exists()` | 每次调用都尝试给 `verifyAuthorization` / `reportAuthorizedInstallation` 打 op_array 补丁（幂等） |
| 2 | `session_start()` | 替换 handler，会话建立后写 `$_SESSION['auth'] = true` → 后台 `header.php` 不再查 `auth_25y`、不再跳绑定页 |
| 3 | `curl_setopt` / `curl_setopt_array` | 命中 `CURLOPT_URL` 且含 `/api/client/report/` 时改写成不可达地址，作为上报拦截的兜底 |

`reportAuthorizedInstallation` 一并压成返回 `['ok' => true, 'result' => 'pass']`，方法体不执行 → **后台目录名不会上报**。

---

## 三、最终效果

| 项目 | 打补丁前 | 打补丁后 |
|---|---|---|
| 前台首页 | `403 未授权访问` | `200` |
| 会员中心 / 购物车 | `403` | `200` |
| 后台任意页面 | `302 → ./?page=auth` | `200` |
| 授权站请求 | 每次校验都发（含后台目录名） | **零外联** |
| 数据库 | 需 `auth_25y` 有行且域名匹配 | **不需要**（函数体不执行，不查库） |
| 加密文件 | — | 一个字节都没改 |

实测（真实 SG 加密的 `verify.php`，且 `ewy/data.php` 不存在）：

```
返回值: {"authorized":true,"message":"缓存授权验证成功","domain":"","expected_domain":""}
```

线上压测（9178）：

```
前台 200 200 200 200 200 200 200 200 200 200     ← 10/10
后台 302 → /ewy?page=login                        ← 正常未登录跳转
日志 每次请求恰好一条 patched verifyAuthorization
FPM 无 SIGSEGV
```

把 `auth_cache_25y` 整表清空后前台仍然 `200` —— 说明确实不依赖授权数据。

---

## 四、安装

从 Releases 下载对应版本的 `.so`，放进 `extension_dir`，在 `php.ini` 里追加（**必须在 `ixed` 之后**）：

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

命令行和 FPM 是两份配置，两边都要加。

---

## 五、配置项

| 键 | 默认 | 说明 |
|---|---|---|
| `ewyauth.enable` | `1` | 总开关 |
| `ewyauth.block_report` | `1` | 上报拦截（op_array 补丁 + curl 兜底） |
| `ewyauth.session_auth` | `1` | 后台 `$_SESSION['auth']` 提权 |
| `ewyauth.log` | 空 | 日志路径，留空不写（也支持环境变量 `EWYAUTH_LOG`） |
| `ewyauth.message` | `缓存授权验证成功` | 返回给前台的 message 文案 |

---

## 六、验证

```bash
php --ri ewyauth                                    # hook function_exists 应为 on

curl -sI https://your-host/ | head -1               # 200
curl -s -o /dev/null -w '%{http_code}\n' https://your-host/<后台目录>/   # 200

grep -E 'patched|session|block' /tmp/ewyauth.log
```

日志样例：

```
[ewyauth] patched verifyAuthorization
[ewyauth] patched reportAuthorizedInstallation
[ewyauth] $_SESSION['auth'] = true
[ewyauth] block curl https://license.rain.mo.cn/api/client/report/
```

---

## 七、支持范围

| PHP | 架构 | 状态 |
|---|---|---|
| 7.2 NTS | x86_64 Linux | CI 构建 |
| 7.3 NTS | x86_64 Linux | 全部行为测试通过 |
| 7.4 NTS | x86_64 Linux | 全部行为测试通过（主验证版本） |

- ZTS 未验证
- PHP 8 未适配（内部函数 handler 与 `zend_op` 布局不同）
- 产物只依赖 `libc.so.6`，最高符号版本 `GLIBC_2.4`，CentOS 7 / Debian 9 以上可直接加载

---

## 八、从源码构建

```bash
phpize
./configure --with-php-config=$(which php-config)
make -j"$(nproc)"
ls modules/ewyauth.so
```

CI（`.github/workflows/release.yml`）打 `v*` 标签时矩阵编译 7.2 / 7.3 / 7.4，做 `extension_loaded` 冒烟测试，产物连同 `SHA256SUMS` 发到 Release。

---

## 九、注意

- 只在请求内改 op_array 内存，不写磁盘、不写数据库
- 补丁幂等，同一请求重复触发不会重复改写
- 与 sukehapp 之类同样钩 `zend_execute_ex` 的扩展没有冲突（本扩展不碰 `zend_execute_ex`）
- 与 opcache 无冲突（SG 的 op_array 不走 opcache 缓存）
- 仅用于你自有或已获授权部署的环境
