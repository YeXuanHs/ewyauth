#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_execute.h"
#include "zend_operators.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern void zend_vm_set_opcode_handler(zend_op *opline);

/* ---------------- state ---------------- */

static void (*o_function_exists)(zend_execute_data *, zval *) = NULL;
static void (*o_session_start)(zend_execute_data *, zval *) = NULL;
static void (*o_curl_setopt)(zend_execute_data *, zval *) = NULL;
static void (*o_curl_setopt_array)(zend_execute_data *, zval *) = NULL;

static int g_enable = 1;
static int g_block_report = 1;
static int g_session_auth = 1;
static char g_log[512];
static char g_msg[128] = "缓存授权验证成功";
static int g_patched = 0;

#define CURLOPT_URL_IDX 10002L

static void ewy_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    if (!g_log[0]) return;
    fp = fopen(g_log, "a");
    if (!fp) return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* ---------------- op_array 补丁 ---------------- */

static void fill_authorized(zval *zv)
{
    array_init(zv);
    add_assoc_bool(zv, "authorized", 1);
    add_assoc_string(zv, "message", g_msg);
    add_assoc_string(zv, "domain", "");
    add_assoc_string(zv, "expected_domain", "");
}

static void fill_ok(zval *zv)
{
    zval d;
    array_init(zv);
    add_assoc_bool(zv, "ok", 1);
    add_assoc_bool(zv, "success", 1);
    add_assoc_string(zv, "result", "pass");
    add_assoc_string(zv, "message", "skipped");
    array_init(&d);
    add_assoc_zval(zv, "data", &d);
}

/*
 * 把函数体压成一条 ZEND_RETURN：
 *   literals[0] -> 要返回的数组
 *   opcodes[0]  -> RETURN CONST(literals[0])
 *   last        -> 1
 */
static int patch_op_array(zend_function *f, int ok_mode)
{
    zend_op_array *oa;
    zend_op *op;

    if (!f || f->type != ZEND_USER_FUNCTION) return 0;

    oa = &f->op_array;
    if (!oa->opcodes || oa->last < 1 || oa->last_literal < 1 || !oa->literals)
        return 0;

    /* 已经压成单条 RETURN 就不再动 */
    if (oa->last == 1 && oa->opcodes[0].opcode == ZEND_RETURN)
        return 0;

    zval_ptr_dtor(&oa->literals[0]);
    if (ok_mode) fill_ok(&oa->literals[0]);
    else         fill_authorized(&oa->literals[0]);

    op = &oa->opcodes[0];
    memset(op, 0, sizeof(zend_op));
    op->opcode = ZEND_RETURN;
    op->op1_type = IS_CONST;
    op->op1.constant = (uint32_t) ((char *) &oa->literals[0] - (char *) op);
    op->op2_type = IS_UNUSED;
    op->result_type = IS_UNUSED;
    op->lineno = oa->line_start;

    zend_vm_set_opcode_handler(op);
    oa->last = 1;
    return 1;
}

static void patch_if_present(const char *name, size_t len, int ok_mode)
{
    char low[128];
    zend_string *key;
    zend_function *f = NULL;
    size_t i;

    if (len >= sizeof(low)) return;

    /* 函数表里的 key 一律小写 */
    for (i = 0; i < len; i++) {
        char c = name[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
    }
    low[len] = 0;

    key = zend_string_init(low, len, 0);
    if (EG(function_table)) f = zend_hash_find_ptr(EG(function_table), key);
    if (!f && CG(function_table)) f = zend_hash_find_ptr(CG(function_table), key);
    zend_string_release(key);

    if (f && patch_op_array(f, ok_mode)) {
        g_patched++;
        ewy_log("[ewyauth] patched %s", name);
    }
}

/* ---------------- function_exists 钩子 ---------------- */

static void ewy_function_exists(zend_execute_data *execute_data, zval *return_value)
{
    zval *arg;

    o_function_exists(execute_data, return_value);

    if (!g_enable || !EG(function_table)) return;
    if (ZEND_CALL_NUM_ARGS(execute_data) < 1) return;

    arg = ZEND_CALL_ARG(execute_data, 1);
    if (!arg || Z_TYPE_P(arg) != IS_STRING) return;

    /* 每次 function_exists 都尝试补一次（幂等，已打过补丁的直接返回） */
    patch_if_present("verifyAuthorization", sizeof("verifyAuthorization") - 1, 0);
    patch_if_present("reportAuthorizedInstallation",
                     sizeof("reportAuthorizedInstallation") - 1, 1);
}

/* ---------------- session_start 钩子（后台门禁） ---------------- */

static void ewy_session_start(zend_execute_data *execute_data, zval *return_value)
{
    zval *sess;

    o_session_start(execute_data, return_value);

    if (!g_enable || !g_session_auth) return;
    if (!return_value || Z_TYPE_P(return_value) != IS_TRUE) return;

    zend_is_auto_global_str(ZEND_STRL("_SESSION"));
    sess = zend_hash_str_find(&EG(symbol_table), "_SESSION", sizeof("_SESSION") - 1);
    if (!sess) return;
    ZVAL_DEREF(sess);
    if (Z_TYPE_P(sess) != IS_ARRAY) return;

    {
        zval t;
        ZVAL_TRUE(&t);
        zend_hash_str_update(Z_ARRVAL_P(sess), "auth", sizeof("auth") - 1, &t);
        ewy_log("[ewyauth] $_SESSION['auth'] = true");
    }
}

/* ---------------- curl 钩子（上报兜底） ---------------- */

static void rewrite_url(zval *zv)
{
    zval tmp;
    if (!zv || Z_TYPE_P(zv) != IS_STRING) return;
    if (!strstr(Z_STRVAL_P(zv), "/api/client/report/")) return;
    ewy_log("[ewyauth] block curl %s", Z_STRVAL_P(zv));
    ZVAL_STRING(&tmp, "http://127.0.0.1:1/__blocked__");
    zval_ptr_dtor(zv);
    ZVAL_COPY_VALUE(zv, &tmp);
}

static void ewy_curl_setopt(zend_execute_data *execute_data, zval *return_value)
{
    if (g_enable && g_block_report && ZEND_CALL_NUM_ARGS(execute_data) >= 3) {
        zval *opt = ZEND_CALL_ARG(execute_data, 2);
        zval *val = ZEND_CALL_ARG(execute_data, 3);
        if (Z_TYPE_P(opt) == IS_LONG && Z_LVAL_P(opt) == CURLOPT_URL_IDX) rewrite_url(val);
    }
    o_curl_setopt(execute_data, return_value);
}

static void ewy_curl_setopt_array(zend_execute_data *execute_data, zval *return_value)
{
    if (g_enable && g_block_report && ZEND_CALL_NUM_ARGS(execute_data) >= 2) {
        zval *arr = ZEND_CALL_ARG(execute_data, 2);
        if (Z_TYPE_P(arr) == IS_ARRAY) {
            zval *u = zend_hash_index_find(Z_ARRVAL_P(arr), (zend_ulong) CURLOPT_URL_IDX);
            if (u) rewrite_url(u);
        }
    }
    o_curl_setopt_array(execute_data, return_value);
}

/* ---------------- 内部函数 handler 替换 ---------------- */

static zend_function *find_internal(const char *name, size_t len)
{
    zend_function *f = NULL;
    if (CG(function_table)) f = zend_hash_str_find_ptr(CG(function_table), name, len);
    if (!f && EG(function_table)) f = zend_hash_str_find_ptr(EG(function_table), name, len);
    return (f && f->type == ZEND_INTERNAL_FUNCTION) ? f : NULL;
}

static void hook_one(const char *name, size_t len,
                     void (**orig)(zend_execute_data *, zval *),
                     void (*repl)(zend_execute_data *, zval *))
{
    zend_function *f = find_internal(name, len);
    if (!f || !f->internal_function.handler) return;
    if (!*orig) *orig = f->internal_function.handler;
    f->internal_function.handler = repl;
}

static void install_hooks(void)
{
    if (!o_function_exists)
        hook_one("function_exists", sizeof("function_exists") - 1,
                 &o_function_exists, ewy_function_exists);

    if (g_session_auth && !o_session_start)
        hook_one("session_start", sizeof("session_start") - 1,
                 &o_session_start, ewy_session_start);

    if (g_block_report) {
        if (!o_curl_setopt)
            hook_one("curl_setopt", sizeof("curl_setopt") - 1,
                     &o_curl_setopt, ewy_curl_setopt);
        if (!o_curl_setopt_array)
            hook_one("curl_setopt_array", sizeof("curl_setopt_array") - 1,
                     &o_curl_setopt_array, ewy_curl_setopt_array);
    }
}

/* ---------------- ini / 模块 ---------------- */

static char *ini_s(const char *n) { return zend_ini_string((char *) n, strlen(n), 0); }

static int ini_b(const char *n)
{
    char *v = ini_s(n);
    if (!v || !*v) return 0;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcasecmp(v, "no"))
        return 0;
    return 1;
}

PHP_INI_BEGIN()
    PHP_INI_ENTRY("ewyauth.enable",       "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.block_report", "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.session_auth", "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.log",          "",  PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.message", "缓存授权验证成功", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

PHP_MINIT_FUNCTION(ewyauth)
{
    const char *p;

    REGISTER_INI_ENTRIES();

    g_enable = ini_b("ewyauth.enable");
    g_block_report = ini_b("ewyauth.block_report");
    g_session_auth = ini_b("ewyauth.session_auth");

    p = ini_s("ewyauth.log");
    if (p && *p) {
        strncpy(g_log, p, sizeof(g_log) - 1);
    } else {
        p = getenv("EWYAUTH_LOG");
        if (p && *p) strncpy(g_log, p, sizeof(g_log) - 1);
    }

    p = ini_s("ewyauth.message");
    if (p && *p) strncpy(g_msg, p, sizeof(g_msg) - 1);

    if (g_enable) install_hooks();

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(ewyauth)
{
    zend_function *f;

    if (o_function_exists) {
        f = find_internal("function_exists", sizeof("function_exists") - 1);
        if (f) f->internal_function.handler = o_function_exists;
        o_function_exists = NULL;
    }
    if (o_session_start) {
        f = find_internal("session_start", sizeof("session_start") - 1);
        if (f) f->internal_function.handler = o_session_start;
        o_session_start = NULL;
    }
    if (o_curl_setopt) {
        f = find_internal("curl_setopt", sizeof("curl_setopt") - 1);
        if (f) f->internal_function.handler = o_curl_setopt;
        o_curl_setopt = NULL;
    }
    if (o_curl_setopt_array) {
        f = find_internal("curl_setopt_array", sizeof("curl_setopt_array") - 1);
        if (f) f->internal_function.handler = o_curl_setopt_array;
        o_curl_setopt_array = NULL;
    }

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(ewyauth)
{
    /* curl 扩展可能晚于本扩展 MINIT；每请求兜底一次（只换 handler） */
    if (g_enable) install_hooks();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(ewyauth)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "ewyauth", g_enable ? "on" : "off");
    php_info_print_table_row(2, "hook function_exists", o_function_exists ? "on" : "off");
    php_info_print_table_row(2, "hook session_start", o_session_start ? "on" : "off");
    php_info_print_table_row(2, "hook curl", (o_curl_setopt || o_curl_setopt_array) ? "on" : "off");
    php_info_print_table_row(2, "patched", g_patched ? "yes" : "no");
    php_info_print_table_end();
}

zend_module_entry ewyauth_module_entry = {
    STANDARD_MODULE_HEADER,
    "ewyauth",
    NULL,
    PHP_MINIT(ewyauth),
    PHP_MSHUTDOWN(ewyauth),
    PHP_RINIT(ewyauth),
    NULL,
    PHP_MINFO(ewyauth),
    "1.1.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_EWYAUTH
ZEND_GET_MODULE(ewyauth)
#endif
