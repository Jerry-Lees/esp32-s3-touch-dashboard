/*
 * Trusted keys, developer mode and app file checks; see app_keys.h.
 * Signatures are ECDSA P-256 over SHA-256 (mbedTLS).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "app_keys.h"

static const char *TAG = "app_keys";

#define NS        "appsec"
#define DER_MAX   128
#define MAX_OK    16                /* approved unsigned apps */
#define MAX_PEND  4

/* Every .pub file in the project's keys/ folder, gathered by main/CMakeLists.txt as
 * "name: <file name>" lines, each followed by that file's PEM text. */
extern const char builtin_keys_start[] asm("_binary_builtin_keys_txt_start");
extern const char builtin_keys_end[]   asm("_binary_builtin_keys_txt_end");
#define MAX_BUILTIN 4

typedef struct {
    char    name[APPK_NAME_MAX];
    uint8_t der_len;
    uint8_t der[DER_MAX];
} stored_key_t;

typedef struct {
    stored_key_t k;
    uint8_t      id[8];
} key_t_;

static SemaphoreHandle_t s_mx;
static key_t_  s_builtin[MAX_BUILTIN];
static int     s_nbuiltin;
static key_t_  s_keys[APPK_MAX_KEYS];
static int     s_nkeys;
static bool    s_dev;
static uint8_t s_ok[MAX_OK][32];
static int     s_nok;
static key_t_  s_pend[MAX_PEND];
static int     s_npend;

static void field(char *out, size_t cap, const uint8_t *in, size_t n, bool ascii);

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

/* ---------- keys ---------- */

static void key_id(const uint8_t *der, size_t len, uint8_t id[8])
{
    uint8_t h[32];
    mbedtls_sha256(der, len, h, 0);
    memcpy(id, h, 8);
}

/* PEM or DER public key -> DER, if it's a 256-bit EC key. */
static bool to_der(const uint8_t *in, size_t len, uint8_t *der, uint8_t *der_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    if (mbedtls_pk_parse_public_key(&pk, in, len) == 0 &&
        mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY && mbedtls_pk_get_bitlen(&pk) == 256) {
        uint8_t buf[DER_MAX];
        int n = mbedtls_pk_write_pubkey_der(&pk, buf, sizeof(buf));   /* writes at the end */
        if (n > 0 && n <= DER_MAX) {
            memcpy(der, buf + sizeof(buf) - n, n);
            *der_len = (uint8_t)n;
            ok = true;
        }
    }
    mbedtls_pk_free(&pk);
    return ok;
}

static void save_keys(void)
{
    stored_key_t buf[APPK_MAX_KEYS];
    for (int i = 0; i < s_nkeys; i++) buf[i] = s_keys[i].k;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = s_nkeys ? nvs_set_blob(h, "keys", buf, sizeof(buf[0]) * s_nkeys)
                            : nvs_erase_key(h, "keys");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving the keys failed: %s", esp_err_to_name(err));
}

static void save_dev(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "dev", s_dev);
    if (s_nok) nvs_set_blob(h, "ok", s_ok, 32 * s_nok);
    else nvs_erase_key(h, "ok");
    nvs_commit(h);
    nvs_close(h);
}

void app_keys_init(void)
{
    if (s_mx) return;
    s_mx = xSemaphoreCreateMutex();

    /* Built-in keys: split the embedded text at each "name: " line. */
    const char *p = builtin_keys_start, *end = builtin_keys_end;
    while (p < end && s_nbuiltin < MAX_BUILTIN) {
        const char *name = strstr(p, "name: ");
        if (!name) break;
        name += 6;
        const char *nl = strchr(name, '\n');
        if (!nl) break;
        const char *next = strstr(nl, "name: ");
        size_t pem_len = (next ? next : end) - (nl + 1);
        static char pem[1024];
        if (pem_len >= sizeof(pem)) pem_len = sizeof(pem) - 1;
        memcpy(pem, nl + 1, pem_len);
        pem[pem_len] = '\0';                      /* PEM parsing wants the NUL counted */
        key_t_ *k = &s_builtin[s_nbuiltin];
        char fname[APPK_NAME_MAX];
        field(fname, sizeof(fname), (const uint8_t *)name, nl - name, true);
        if (to_der((const uint8_t *)pem, pem_len + 1, k->k.der, &k->k.der_len)) {
            /* builtin_app_key.pub is just "Built-in"; others keep their file name. */
            if (!strcmp(fname, "builtin_app_key")) strcpy(k->k.name, "Built-in");
            else snprintf(k->k.name, sizeof(k->k.name), "%s", fname);
            key_id(k->k.der, k->k.der_len, k->id);
            s_nbuiltin++;
        } else {
            ESP_LOGE(TAG, "Built-in key %s isn't an ECDSA P-256 public key", fname);
        }
        p = next ? next : end;
    }
    if (s_nbuiltin == 0) ESP_LOGW(TAG, "No built-in app keys (keys/*.pub)");

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        stored_key_t buf[APPK_MAX_KEYS];
        size_t n = sizeof(buf);
        if (nvs_get_blob(h, "keys", buf, &n) == ESP_OK) {
            for (size_t i = 0; i < n / sizeof(buf[0]); i++) {
                if (buf[i].der_len == 0 || buf[i].der_len > DER_MAX) continue;
                s_keys[s_nkeys].k = buf[i];
                s_keys[s_nkeys].k.name[APPK_NAME_MAX - 1] = '\0';
                key_id(buf[i].der, buf[i].der_len, s_keys[s_nkeys].id);
                s_nkeys++;
            }
        }
        uint8_t dev = 0;
        nvs_get_u8(h, "dev", &dev);
        s_dev = dev == 1;
        n = sizeof(s_ok);
        if (s_dev && nvs_get_blob(h, "ok", s_ok, &n) == ESP_OK) s_nok = n / 32;
        nvs_close(h);
    }
}

int app_keys_count(void)
{
    lock();
    int n = s_nkeys + s_nbuiltin;
    unlock();
    return n;
}

bool app_keys_get(int i, appk_key_t *out)
{
    bool ok = true;
    lock();
    if (i >= 0 && i < s_nbuiltin) {
        strcpy(out->name, s_builtin[i].k.name);
        memcpy(out->id, s_builtin[i].id, 8);
        out->builtin = true;
    } else {
        i -= s_nbuiltin;
        if (i >= 0 && i < s_nkeys) {
            snprintf(out->name, sizeof(out->name), "%s", s_keys[i].k.name);
            memcpy(out->id, s_keys[i].id, 8);
            out->builtin = false;
        } else {
            ok = false;
        }
    }
    unlock();
    return ok;
}

bool app_keys_remove(int i)
{
    lock();
    i -= s_nbuiltin;
    bool ok = i >= 0 && i < s_nkeys;
    if (ok) {
        memmove(&s_keys[i], &s_keys[i + 1], sizeof(s_keys[0]) * (s_nkeys - i - 1));
        s_nkeys--;
        save_keys();
    }
    unlock();
    return ok;
}

void app_keys_fingerprint(const uint8_t id[8], char *out, size_t len)
{
    snprintf(out, len, "%02X%02X %02X%02X %02X%02X %02X%02X",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

/* ---------- developer mode ---------- */

bool app_keys_dev_mode(void)
{
    lock();
    bool on = s_dev;
    unlock();
    return on;
}

void app_keys_set_dev_mode(bool on)
{
    lock();
    s_dev = on;
    if (!on) {
        memset(s_ok, 0, sizeof(s_ok));
        s_nok = 0;
    }
    save_dev();
    unlock();
}

bool app_keys_approve(const uint8_t sha[32])
{
    lock();
    bool ok = s_dev;
    if (ok) {
        if (s_nok == MAX_OK) {                  /* forget the oldest */
            memmove(s_ok[0], s_ok[1], 32 * (MAX_OK - 1));
            s_nok--;
        }
        memcpy(s_ok[s_nok++], sha, 32);
        save_dev();
    }
    unlock();
    return ok;
}

/* ---------- checking an app ---------- */

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void field(char *out, size_t cap, const uint8_t *in, size_t n, bool ascii)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap && in[i]; i++) {
        out[i] = (ascii && (in[i] < 0x20 || in[i] > 0x7E)) ? '?' : (char)in[i];
    }
    out[i] = '\0';
}

static bool verify(const key_t_ *key, const uint8_t hash[32], const uint8_t *sig, size_t len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, key->k.der, key->k.der_len) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, len) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

appk_status_t app_keys_check(const uint8_t *d, size_t len, appk_info_t *info,
                             char *why, size_t why_len)
{
    memset(info, 0, sizeof(*info));
    why[0] = '\0';
    if (len < APPK_HDR || memcmp(d, "DASHAPP1", 8) || rd16(d + 8) != APPK_HDR) {
        snprintf(why, why_len, "not a dashboard app file");
        return APPK_REFUSED;
    }
    uint16_t flags = rd16(d + 10);
    uint32_t elf_len = rd32(d + 12);
    field(info->name, sizeof(info->name), d + 16, 32, true);
    field(info->version, sizeof(info->version), d + 48, 16, true);
    field(info->symbol, sizeof(info->symbol), d + 64, 8, false);
    info->color = rd32(d + 72) & 0xFFFFFF;
    memcpy(info->key_id, d + 76, 8);
    info->is_signed = flags & 1;
    if (!info->name[0]) snprintf(info->name, sizeof(info->name), "App");
    if (elf_len < 52 || elf_len > len - APPK_HDR || memcmp(d + APPK_HDR, "\x7f" "ELF", 4)) {
        snprintf(why, why_len, "the file is damaged");
        return APPK_REFUSED;
    }
    info->elf = d + APPK_HDR;
    info->elf_len = elf_len;
    /* Optional tile picture after the code, covered by the signature. */
    field(info->folder, sizeof(info->folder), d + 84, 16, true);
    uint16_t iw = rd16(d + 100), ih = rd16(d + 102);
    uint32_t icon_len = rd32(d + 104);
    if (icon_len != (uint32_t)iw * ih * 3 || iw > APPK_ICON_MAX || ih > APPK_ICON_MAX ||
        icon_len > len - APPK_HDR - elf_len) {
        snprintf(why, why_len, "the file is damaged");
        return APPK_REFUSED;
    }
    if (icon_len) {
        info->icon = d + APPK_HDR + elf_len;
        info->icon_w = iw;
        info->icon_h = ih;
    }
    size_t body = APPK_HDR + elf_len + icon_len;
    mbedtls_sha256(d, body, info->sha, 0);

    if (info->is_signed) {
        if (len < body + 2 || len != body + 2 + rd16(d + body) || rd16(d + body) > 80) {
            snprintf(why, why_len, "the signature is missing or damaged");
            return APPK_REFUSED;
        }
        const uint8_t *sig = d + body + 2;
        size_t sig_len = rd16(d + body);
        lock();
        const key_t_ *key = NULL;
        for (int i = 0; !key && i < s_nbuiltin; i++) {
            if (!memcmp(s_builtin[i].id, info->key_id, 8)) key = &s_builtin[i];
        }
        for (int i = 0; !key && i < s_nkeys; i++) {
            if (!memcmp(s_keys[i].id, info->key_id, 8)) key = &s_keys[i];
        }
        key_t_ k;
        if (key) k = *key;
        unlock();
        if (!key) {
            char fp[24];
            app_keys_fingerprint(info->key_id, fp, sizeof(fp));
            snprintf(why, why_len, "signed by a key that isn't trusted (%s)", fp);
            return APPK_REFUSED;
        }
        snprintf(info->signer, sizeof(info->signer), "%s", k.k.name);
        if (!verify(&k, info->sha, sig, sig_len)) {
            snprintf(why, why_len, "the signature doesn't match: the file was changed");
            return APPK_REFUSED;
        }
        return APPK_OK;
    }

    if (len != body) {
        snprintf(why, why_len, "the file is damaged");
        return APPK_REFUSED;
    }
    lock();
    bool dev = s_dev, ok = false;
    for (int i = 0; dev && i < s_nok; i++) {
        if (!memcmp(s_ok[i], info->sha, 32)) ok = true;
    }
    unlock();
    if (!dev) {
        snprintf(why, why_len, "unsigned (runs only in developer mode)");
        return APPK_REFUSED;
    }
    return ok ? APPK_OK_DEV : APPK_NEEDS_APPROVAL;
}

/* ---------- adding keys ---------- */

void app_keys_import_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < 5 || strcasecmp(de->d_name + n - 4, ".pub")) continue;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        uint8_t pem[1025];
        size_t got = 0;
        FILE *f = fopen(path, "rb");
        if (f) {
            got = fread(pem, 1, sizeof(pem) - 1, f);
            fclose(f);
        }
        unlink(path);                           /* public, nothing to wipe */
        pem[got] = '\0';
        key_t_ k = {0};
        if (!to_der(pem, got + 1, k.k.der, &k.k.der_len)) {
            ESP_LOGW(TAG, "%s isn't an ECDSA P-256 public key; ignored", de->d_name);
            continue;
        }
        /* Name: the file name without .pub, printable ASCII. */
        field(k.k.name, sizeof(k.k.name), (const uint8_t *)de->d_name, n - 4, true);
        key_id(k.k.der, k.k.der_len, k.id);
        lock();
        bool known = false;
        for (int i = 0; i < s_nbuiltin; i++) known |= !memcmp(s_builtin[i].id, k.id, 8);
        for (int i = 0; i < s_nkeys; i++) known |= !memcmp(s_keys[i].id, k.id, 8);
        if (!known && s_npend < MAX_PEND) s_pend[s_npend++] = k;
        unlock();
    }
    closedir(dp);
}

bool app_keys_pending(char *name, size_t name_len, uint8_t id[8])
{
    lock();
    bool any = s_npend > 0;
    if (any) {
        snprintf(name, name_len, "%s", s_pend[0].k.name);
        memcpy(id, s_pend[0].id, 8);
    }
    unlock();
    return any;
}

bool app_keys_answer(bool yes, const char *name)
{
    bool ok = true;
    lock();
    if (s_npend > 0) {
        if (yes) {
            if (s_nkeys < APPK_MAX_KEYS) {
                s_keys[s_nkeys] = s_pend[0];
                char clean[APPK_NAME_MAX];
                field(clean, sizeof(clean), (const uint8_t *)(name ? name : ""),
                      name ? strlen(name) : 0, true);
                if (clean[0]) strcpy(s_keys[s_nkeys].k.name, clean);
                s_nkeys++;
                save_keys();
            } else {
                ok = false;
            }
        }
        memmove(&s_pend[0], &s_pend[1], sizeof(s_pend[0]) * (s_npend - 1));
        s_npend--;
    }
    unlock();
    return ok;
}
