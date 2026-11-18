# ewyauth

`ewy` / `25y` 4.0.6 模板运行时的授权门禁补丁，一个 PHP 扩展（7.2 / 7.3 / 7.4，NTS，x86_64 Linux）。

不改任何一个 SourceGuardian 加密文件，不伪造远程签名，不劫持 DNS，只在运行时接管三个入口。

---

## 一、原理

### 1.1 授权判定在哪

这套模板的授权不是一处，而是**两条互不相干的链路**：

| 范围 | 入口 | 判定依据 | 未通过表现 |
|---|---|---|---|
| 前台（`themes/web`、`themes/cart`、`themes/clientarea`） | 控制器 `_auth_check()` → `verifyAuthorization()` | 远程/缓存校验结果 `authorized === true` | `403 未授权访问` |
| 后台（`ewy/*` 全部页面） | `ewy/header.php` 顶部 | `$_SESSION['auth'] === true`；不满足才 `SELECT state FROM auth_25y LIMIT 1`，`state == 1` 才置位 | `302 → ./?page=auth`（绑定页） |

两条链路的源头都是 SourceGuardian 加密的 PHP，磁盘上是密文，改不了。但它们的**判定动作**分别是：

- 一次普通的用户函数调用（`verifyAuthorization()`）
- 一次会话状态判断（`$_SESSION['auth']`）

这两样都在 Zend 引擎层，可以在函数执行前拦截。

### 1.2 为什么能拿到函数名

SourceGuardian 17 在 `sourceguardian.enable_vm_hybrid=1` 下，会把加密字节码还原成**标准 `zend_op_array`** 交给 Zend 执行 —— opcode 没有被替换、handler 没有被劫持、**函数名和类名完整保留**。所以加密与否不影响按函数名匹配。

### 1.3 三层接管

| 层 | 手段 | 用途 |
|---|---|---|
| 用户函数 | 包 `zend_execute_ex` | 命中 `verifyAuthorization` / `reportAuthorizedInstallation` 后直接写返回值、跳过函数体 |
| 内部函数 | 替换 `zend_internal_function.handler` 指针 | `session_start`（写会话标志）、`curl_setopt` / `curl_setopt_array`（改 URL） |
| 返回值 | 直接写 `execute_data->return_value` | 伪造 `['authorized' => true]` / `['ok' => true]`，不需要函数真的执行 |

> 为什么内部函数要换 handler 指针而不是包 `zend_execute_internal`：PHP 7 的 ICALL 走 fastcall 快路径，
> 即使设置了 `zend_execute_internal` 也拦不到，只有换 `handler` 才生效。

---

## 二、做了什么

三个接管点，一一对应三条链路。

### 2.1 前台门禁：`verifyAuthorization()`

```c
if (zstr_is(fn, "verifyAuthorization")) {
    ret_authorized(ed);   /* 写 return_value 后直接 return，函数体不执行 */
    return;
}
```

返回值固定为：

```php
['authorized' => true, 'message' => '缓存授权验证成功', 'domain' => '', 'expected_domain' => '']
```

因为函数体压根没跑，所以**不读 `ewy/data.php`、不连 MySQL、不查 `auth_25y`、不发任何 HTTP 请求**。
前台 `_auth_check()` 只判断 `authorized === true`，拿到就直接放行。

### 2.2 后台门禁：`session_start()`

后台判断的第一句是 `$_SESSION['auth'] !== true`，不满足才去查库。所以只要会话一建立就把标志写进去：

```c
static void ewy_session_start(zend_execute_data *ed, zval *rv)
{
    orig_session_start(ed, rv);
    if (Z_TYPE_P(rv) != IS_TRUE) return;

    zend_is_auto_global_str(ZEND_STRL("_SESSION"));
    zval *sess = zend_hash_str_find(&EG(symbol_table), "_SESSION", sizeof("_SESSION") - 1);
    if (sess) {
        ZVAL_DEREF(sess);
        if (Z_TYPE_P(sess) == IS_ARRAY) {
            zval t;
            ZVAL_TRUE(&t);
            zend_hash_str_update(Z_ARRVAL_P(sess), "auth", sizeof("auth") - 1, &t);
        }
    }
}
```

`header.php` 判断时 `$_SESSION['auth']` 已经是 `true`，`SELECT state FROM auth_25y` 那条查询根本不会执行，
也不会 `302` 到绑定页。

### 2.3 上报拦截：`reportAuthorizedInstallation()`

这套模板通过 `POST /api/client/report/` 把**后台目录名**（`custom_info.admin_path`）回传授权站，
另外两个客户端分别发往 `license.rain.mo.cn` / `license-out.huanware.cn` / `license-cf.xkshop.top` 和 `auth.鱼饼饼.online`。

做了两道拦截：

1. **方法级**：命中 `reportAuthorizedInstallation` 直接返回 `['ok' => true, 'result' => 'pass', 'data' => []]`，方法体不执行
2. **传输级**：兜底改写 curl 的 `CURLOPT_URL`（10002），凡命中 `/api/client/report/` 一律替换为 `http://127.0.0.1:1/__blocked__`

`curl_setopt` 和 `curl_setopt_array` 两个函数都接管，业务代码用哪种写法都拦得住。

---

## 三、最终效果

| 项目 | 打补丁前 | 打补丁后 |
|---|---|---|
| 前台首页 | `403 未授权访问` | `200` 正常渲染 |
| 会员中心 / 购物车 | `403` | `200` |
| 后台任意页面 | `302 → ./?page=auth` | `200` 正常进入 |
| 授权站请求 | 每次校验都发（含后台目录名） | **零外联** |
| 数据库 | 需 `auth_25y` 有行、域名匹配 | 不需要，无查询 |
| 加密文件 | — | **一个字节都没改** |

日志（`ewyauth.log`）：

```
[1795044793] session hook installed
[1795044793] session_start: $_SESSION['auth'] = true
[1795044806] force verifyAuthorization() => authorized=true
[1795044793] block reportAuthorizedInstallation() [admin_path not sent]
[1795044793] block curl url: https://license.rain.mo.cn/api/client/report/
```

实测（真实 SG 加密的 `verify.php`，完整走一遍包含 `require ewy/data.php` 的流程）：

```
返回值: {"authorized":true,"message":"缓存授权验证成功","domain":"","expected_domain":""}
```

在 `ewy/data.php` 不存在的情况下依然通过 —— 说明确实没有走数据库分支。

---

## 四、安装

从 Releases 下载对应版本的 `ewyauth-<版本>-nts-linux-x86_64.so`，放进 `extension_dir`。

在 `php.ini` 里追加，**顺序必须在 `ixed.<ver>.lin` 之后**：

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

命令行和 FPM 是两份配置，两边都要加；改完必须重启 FPM。

---

## 五、配置项

| 键 | 默认 | 说明 |
|---|---|---|
| `ewyauth.enable` | `1` | 总开关 |
| `ewyauth.block_report` | `1` | 拦截 `reportAuthorizedInstallation()` + curl 层兜底 |
| `ewyauth.session_auth` | `1` | 后台 `$_SESSION['auth']` 提权 |
| `ewyauth.force_remote` | `0` | 同时短路 `LicenseSdkV2::verify` / `HuanwareLicenseClient::verify`、`bind`。后台「绑定授权」页需要提交卡密时才打开 |
| `ewyauth.trace` | `0` | 记录每个用户函数调用，排障用，量很大 |
| `ewyauth.log` | 空 | 日志路径，留空不写日志 |
| `ewyauth.message` | `缓存授权验证成功` | 返回给前台的 message 文案 |

---

## 六、验证

```bash
# 1. 模块是否加载
php --ri ewyauth

# 2. 前台（200，不再 403）
curl -sI https://your-host/ | head -1

# 3. 后台（200，不再 302 到 ?page=auth）
curl -s -o /dev/null -w '%{http_code} %{redirect_url}\n' https://your-host/<后台目录>/

# 4. 日志三行关键信息
grep -E 'force verifyAuthorization|session_start|block report' /tmp/ewyauth.log

# 5. 确认没有外联（应无输出）
grep -E 'license\.rain|huanware|鱼饼饼' /tmp/ewyauth.log
```

---

## 七、支持范围

| PHP | 架构 | 状态 |
|---|---|---|
| 7.2 NTS | x86_64 Linux | 编译、加载通过（行为测试待补） |
| 7.3 NTS | x86_64 Linux | 全部行为测试通过 |
| 7.4 NTS | x86_64 Linux | 全部行为测试通过（主验证版本） |

- ZTS 未验证
- PHP 8 未适配（`zend_execute_ex` 与内部函数 handler 的调用约定不同）
- 产物只依赖 `libc.so.6`，最高符号版本 `GLIBC_2.4`，CentOS 7 / Debian 9 及以上可直接加载

---

## 八、从源码构建

仓库根目录即扩展目录，`config.m4` 与 `ewyauth.c` 平铺：

```bash
phpize
./configure --with-php-config=$(which php-config)
make -j"$(nproc)"
ls modules/ewyauth.so
```

CI（`.github/workflows/release.yml`）在打 `v*` 标签时矩阵编译 7.2 / 7.3 / 7.4，
每个版本做 `extension_loaded` 冒烟测试，产物连同 `SHA256SUMS` 一起发到 Release；
也支持 `workflow_dispatch` 手动构建（只出 artifact，不发 Release）。

---

## 九、注意

- 不写数据库、不改磁盘文件，只动三个入口
- 不拦业务 curl，只拦 `/api/client/report/` 这一条 URI
- 与 opcache 无冲突：不重写 op_array，只包执行入口
- 仅用于你自有或已获授权部署的环境
