#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_execute.h"
#include "zend_operators.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---------------- state ---------------- */

static void (*orig_execute_ex)(zend_execute_data *ed) = NULL;
static void (*orig_curl_setopt)(zend_execute_data *ed, zval *rv) = NULL;
static void (*orig_curl_setopt_array)(zend_execute_data *ed, zval *rv) = NULL;
static void (*orig_session_start)(zend_execute_data *ed, zval *rv) = NULL;

static int g_on = 1;
static int g_block_report = 1;
static int g_force_remote = 0;
static int g_trace = 0;
static char g_logfile[512] = {0};
static char g_msg[256] = "缓存授权验证成功";
static int g_hooked_curl = 0;
static int g_session_auth = 1;
static int g_hooked_session = 0;

static zend_function *find_internal(const char *name, size_t nlen);

static void ewy_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    if (!g_logfile[0]) return;
    fp = fopen(g_logfile, "a");
    if (!fp) return;
    fprintf(fp, "[%ld] ", (long) time(NULL));
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static int zstr_is(zend_string *s, const char *lit)
{
    if (!s) return 0;
    return zend_binary_strcasecmp(ZSTR_VAL(s), ZSTR_LEN(s), lit, strlen(lit)) == 0;
}

/* ---------------- return rewriting ---------------- */

static void ret_authorized(zend_execute_data *ed)
{
    zval *rv = ed->return_value;
    if (!rv) return;
    zval_ptr_dtor(rv);
    array_init(rv);
    add_assoc_bool(rv, "authorized", 1);
    add_assoc_string(rv, "message", g_msg);
    add_assoc_string(rv, "domain", "");
    add_assoc_string(rv, "expected_domain", "");
}

static void ret_ok(zend_execute_data *ed, const char *msg)
{
    zval *rv = ed->return_value;
    zval data;
    if (!rv) return;
    zval_ptr_dtor(rv);
    array_init(rv);
    add_assoc_bool(rv, "ok", 1);
    add_assoc_bool(rv, "success", 1);
    add_assoc_string(rv, "result", "pass");
    add_assoc_string(rv, "message", (char *) msg);
    array_init(&data);
    add_assoc_zval(rv, "data", &data);
}

/* ---------------- execute_ex hook ---------------- */

static void ewy_execute_ex(zend_execute_data *ed)
{
    if (g_on && ed && ed->func && ed->func->type == ZEND_USER_FUNCTION) {
        zend_string *fn = ed->func->common.function_name;

        if (g_trace && fn) ewy_log("call %s", ZSTR_VAL(fn));

        if (zstr_is(fn, "verifyAuthorization")) {
            ret_authorized(ed);
            ewy_log("force verifyAuthorization() => authorized=true");
            return;
        }

        if (zstr_is(fn, "reportAuthorizedInstallation")) {
            ret_ok(ed, "skipped");
            ewy_log("block reportAuthorizedInstallation() [admin_path not sent]");
            return;
        }

        if (g_force_remote && fn) {
            zend_class_entry *ce = ed->func->common.scope;
            if (ce && (zstr_is(ce->name, "LicenseSdkV2") || zstr_is(ce->name, "HuanwareLicenseClient"))) {
                if (zstr_is(fn, "verify") || zstr_is(fn, "bind")) {
                    ret_ok(ed, "skipped");
                    ewy_log("force %s::%s() => ok", ZSTR_VAL(ce->name), ZSTR_VAL(fn));
                    return;
                }
            }
        }
    }
    orig_execute_ex(ed);
}

/* ---------------- session hook ---------------- */

/*
 * backend gate: ewy/header.php
 *   if ($_SESSION['auth'] !== true) { SELECT state FROM auth_25y ... }
 *   header("Location: ./?page=auth"); exit;
 * set the flag right after the session starts, so the DB probe never runs.
 */
static void ewy_session_start(zend_execute_data *ed, zval *rv)
{
    orig_session_start(ed, rv);

    if (!g_on || !g_session_auth) return;
    if (!rv || Z_TYPE_P(rv) != IS_TRUE) return;

    zend_is_auto_global_str(ZEND_STRL("_SESSION"));
    {
        zval *sess = zend_hash_str_find(&EG(symbol_table), "_SESSION", sizeof("_SESSION") - 1);
        if (sess) {
            ZVAL_DEREF(sess);
            if (Z_TYPE_P(sess) == IS_ARRAY) {
                zval t;
                ZVAL_TRUE(&t);
                zend_hash_str_update(Z_ARRVAL_P(sess), "auth", sizeof("auth") - 1, &t);
                ewy_log("session_start: $_SESSION['auth'] = true");
            }
        }
    }
}

static void hook_session(void)
{
    zend_function *f;
    if (g_hooked_session) return;

    f = find_internal("session_start", sizeof("session_start") - 1);
    if (f && f->internal_function.handler) {
        if (!orig_session_start) orig_session_start = (void *) f->internal_function.handler;
        f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) ewy_session_start;
        g_hooked_session = 1;
        ewy_log("session hook installed");
    }
}

/* ---------------- curl hook ---------------- */

#define CURLOPT_URL_IDX 10002L

static void rewrite_url(zval *zv)
{
    zval tmp;
    if (!zv || Z_TYPE_P(zv) != IS_STRING) return;
    if (!strstr(Z_STRVAL_P(zv), "/api/client/report/")) return;
    ewy_log("block curl url: %s", Z_STRVAL_P(zv));
    ZVAL_STRING(&tmp, "http://127.0.0.1:1/__blocked__");
    zval_ptr_dtor(zv);
    ZVAL_COPY_VALUE(zv, &tmp);
}

static void ewy_curl_setopt(zend_execute_data *ed, zval *rv)
{
    if (g_on && g_block_report && ZEND_CALL_NUM_ARGS(ed) >= 3) {
        zval *opt = ZEND_CALL_ARG(ed, 2);
        zval *val = ZEND_CALL_ARG(ed, 3);
        if (Z_TYPE_P(opt) == IS_LONG && Z_LVAL_P(opt) == CURLOPT_URL_IDX) rewrite_url(val);
    }
    orig_curl_setopt(ed, rv);
}

static void ewy_curl_setopt_array(zend_execute_data *ed, zval *rv)
{
    if (g_on && g_block_report && ZEND_CALL_NUM_ARGS(ed) >= 2) {
        zval *arr = ZEND_CALL_ARG(ed, 2);
        if (Z_TYPE_P(arr) == IS_ARRAY) {
            zval *u = zend_hash_index_find(Z_ARRVAL_P(arr), (zend_ulong) CURLOPT_URL_IDX);
            if (u) rewrite_url(u);
        }
    }
    orig_curl_setopt_array(ed, rv);
}

static zend_function *find_internal(const char *name, size_t nlen)
{
    zend_function *f = NULL;
    if (CG(function_table)) f = zend_hash_str_find_ptr(CG(function_table), name, nlen);
    if (!f && EG(function_table)) f = zend_hash_str_find_ptr(EG(function_table), name, nlen);
    return (f && f->type == ZEND_INTERNAL_FUNCTION) ? f : NULL;
}

static void hook_curl(void)
{
    zend_function *f;

    if (g_hooked_curl) return;

    f = find_internal("curl_setopt", sizeof("curl_setopt") - 1);
    if (f && f->internal_function.handler) {
        if (!orig_curl_setopt) orig_curl_setopt = (void *) f->internal_function.handler;
        f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) ewy_curl_setopt;
    }

    f = find_internal("curl_setopt_array", sizeof("curl_setopt_array") - 1);
    if (f && f->internal_function.handler) {
        if (!orig_curl_setopt_array) orig_curl_setopt_array = (void *) f->internal_function.handler;
        f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) ewy_curl_setopt_array;
    }

    if (orig_curl_setopt || orig_curl_setopt_array) {
        g_hooked_curl = 1;
        ewy_log("curl hooks installed (setopt=%p, setopt_array=%p)",
                (void *) orig_curl_setopt, (void *) orig_curl_setopt_array);
    }
}

/* ---------------- ini ---------------- */

static char *ini_s(const char *n) { return zend_ini_string((char *) n, strlen(n), 0); }

static int ini_b(const char *n)
{
    char *v = ini_s(n);
    if (!v || !*v) return 0;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcasecmp(v, "no"))
        return 0;
    return 1;
}

static void load_ini(void)
{
    char *v;

    g_on = ini_b("ewyauth.enable");
    g_block_report = ini_b("ewyauth.block_report");
    g_force_remote = ini_b("ewyauth.force_remote");
    g_trace = ini_b("ewyauth.trace");
    g_session_auth = ini_b("ewyauth.session_auth");

    v = ini_s("ewyauth.log");
    if (v && *v) {
        strncpy(g_logfile, v, sizeof(g_logfile) - 1);
        g_logfile[sizeof(g_logfile) - 1] = 0;
    }

    v = ini_s("ewyauth.message");
    if (v && *v) {
        strncpy(g_msg, v, sizeof(g_msg) - 1);
        g_msg[sizeof(g_msg) - 1] = 0;
    }
}

/* ---------------- module ---------------- */

PHP_INI_BEGIN()
    PHP_INI_ENTRY("ewyauth.enable",       "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.block_report", "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.force_remote", "0", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.session_auth", "1", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.trace",        "0", PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.log",          "",  PHP_INI_SYSTEM, NULL)
    PHP_INI_ENTRY("ewyauth.message", "缓存授权验证成功", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

PHP_MINIT_FUNCTION(ewyauth)
{
    REGISTER_INI_ENTRIES();
    load_ini();

    orig_execute_ex = zend_execute_ex;
    zend_execute_ex = ewy_execute_ex;

    hook_curl();
    hook_session();

    ewy_log("module init: enable=%d block_report=%d force_remote=%d",
            g_on, g_block_report, g_force_remote);

    return SUCCESS;
}

PHP_RINIT_FUNCTION(ewyauth)
{
    /* curl may be loaded after us; retry once per request until installed */
    if (g_on && g_block_report) hook_curl();
    if (g_on && g_session_auth) hook_session();
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(ewyauth)
{
    zend_function *f;

    if (orig_execute_ex) {
        zend_execute_ex = orig_execute_ex;
        orig_execute_ex = NULL;
    }

    if (orig_curl_setopt) {
        f = find_internal("curl_setopt", sizeof("curl_setopt") - 1);
        if (f) f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) orig_curl_setopt;
        orig_curl_setopt = NULL;
    }

    if (orig_session_start) {
        f = find_internal("session_start", sizeof("session_start") - 1);
        if (f) f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) orig_session_start;
        orig_session_start = NULL;
    }

    if (orig_curl_setopt_array) {
        f = find_internal("curl_setopt_array", sizeof("curl_setopt_array") - 1);
        if (f) f->internal_function.handler = (void (*)(zend_execute_data *, zval *)) orig_curl_setopt_array;
        orig_curl_setopt_array = NULL;
    }

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(ewyauth)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "ewyauth", "enabled");
    php_info_print_table_row(2, "verifyAuthorization", "forced to authorized=true");
    php_info_print_table_row(2, "reportAuthorizedInstallation", g_block_report ? "blocked" : "passthrough");
    php_info_print_table_row(2, "log", g_logfile[0] ? g_logfile : "(off)");
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
    "0.1",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_EWYAUTH
ZEND_GET_MODULE(ewyauth)
#endif
