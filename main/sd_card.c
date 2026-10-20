/*
 * microSD card: mount at boot, create the /dashboard folders and process
 * /dashboard/import. Never formats the card, even if mounting fails, and
 * never touches anything outside /dashboard.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "net_time.h"
#include "app_keys.h"
#include "dashpad.h"
#include "ext_apps.h"
#include "sd_card.h"
#include "settings_backup.h"
#include "wifi_list.h"

/* Most cards over 32 GB come formatted as exFAT, which ESP-IDF ships
 * switched off. See "exFAT support" in README.md. */
#if !FF_FS_EXFAT
#error "exFAT is disabled: set FF_FS_EXFAT to 1 in $IDF_PATH/components/fatfs/src/ffconf.h (see README.md, 'exFAT support')"
#endif

#define PIN_MOSI  11
#define PIN_CLK   12
#define PIN_MISO  13

static const char *TAG = "sd_card";

static sdmmc_card_t *s_card;
static bool          s_mounted;   /* __atomic */
static int           s_pdrv;      /* FatFs drive number, set before s_mounted */

bool sd_card_mounted(void)
{
    return __atomic_load_n(&s_mounted, __ATOMIC_ACQUIRE);
}

/* ---------- messages for the screen ---------- */

static char s_notice[160];
static bool s_notice_ready;   /* __atomic; the writer only fills an empty slot */

/* SD task only. Waits (up to 3 s) for the screen to take the previous
 * message, so two messages at startup are both shown. */
void sd_card_notice(const char *fmt, ...)
{
    for (int i = 0; i < 30 && __atomic_load_n(&s_notice_ready, __ATOMIC_ACQUIRE); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (__atomic_load_n(&s_notice_ready, __ATOMIC_ACQUIRE)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_notice, sizeof(s_notice), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_notice);
    __atomic_store_n(&s_notice_ready, true, __ATOMIC_RELEASE);
}

bool sd_card_is_secret_name(const char *name)
{
    if (strncasecmp(name, "wifi", 4) != 0) return false;
    for (const char *p = name; *p; p++) {
        if (!strncasecmp(p, ".json", 5)) return true;
    }
    return false;
}

bool sd_card_take_notice(char *out, size_t len)
{
    if (!__atomic_load_n(&s_notice_ready, __ATOMIC_ACQUIRE)) return false;
    strlcpy(out, s_notice, len);
    __atomic_store_n(&s_notice_ready, false, __ATOMIC_RELEASE);
    return true;
}

/* ---------- /dashboard folder layout ---------- */

static const char *const FOLDERS[] = {
    SD_DIR, SD_DIR "/import", SD_DIR "/pictures", SD_DIR "/screenshots",
    SD_DIR "/backup", SD_DIR "/apps",
};

static const char README_TXT[] =
    "ESP32-S3 Touch Dashboard - SD card folders\r\n"
    "==========================================\r\n"
    "The display creates these folders and only changes files in them (it\r\n"
    "rewrites this README when it's out of date). See README.md on GitHub\r\n"
    "(Jerry-Lees/esp32-s3-touch-dashboard).\r\n"
    "\r\n"
    "import/       One-time inbox. Files here are read at startup, applied,\r\n"
    "              then erased. wifi.json (Wi-Fi networks, up to 10) is\r\n"
    "              overwritten with blanks and deleted after import, so\r\n"
    "              passwords don't stay on the card. settings*.json restores\r\n"
    "              a settings backup. dashpad*.json replaces the DashPad\r\n"
    "              layout.\r\n"
    "pictures/     Pictures for the Photos app and the photo screensaver:\r\n"
    "              JPEG, PNG, GIF or BMP (folders inside are included).\r\n"
    "screenshots/  Screenshots: long-press the top bar on any screen.\r\n"
    "backup/       Settings backups (Settings > About > Back up settings). They\r\n"
    "              never include Wi-Fi networks. To restore one, copy it into\r\n"
    "              import/ and restart the display.\r\n"
    "apps/         Apps (.app files). They run only if signed by a trusted key\r\n"
    "              (Settings > Apps). A .pub key file in import/ is offered\r\n"
    "              for trust on screen at the next start.\r\n"
    "\r\n"
    "Files can be copied to and from these folders over the USB cable,\r\n"
    "without taking the card out: tools/sdcard.py in the project.\r\n";

static void make_folders(void)
{
    for (size_t i = 0; i < sizeof(FOLDERS) / sizeof(FOLDERS[0]); i++) {
        if (mkdir(FOLDERS[i], 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "Couldn't create %s", FOLDERS[i]);
        }
    }
    /* (Re)write README.txt unless it's already this version. */
    char old[sizeof(README_TXT) + 1];
    size_t n = 0;
    FILE *f = fopen(SD_DIR "/README.txt", "rb");
    if (f) {
        n = fread(old, 1, sizeof(old), f);
        fclose(f);
    }
    if (n != sizeof(README_TXT) - 1 || memcmp(old, README_TXT, n) != 0) {
        f = fopen(SD_DIR "/README.txt", "wb");
        if (f) {
            fwrite(README_TXT, 1, sizeof(README_TXT) - 1, f);
            fclose(f);
        }
    }
}

/* ---------- import/wifi.json ---------- */

#define WIFI_JSON_MAX 8192

/* Overwrites the whole file with zeros, then deletes it, so the passwords
 * can't be recovered with an undelete tool. */
static bool secure_delete(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    static const uint8_t zeros[512];
    for (long done = 0; done < size; ) {
        size_t n = size - done < (long)sizeof(zeros) ? size - done : sizeof(zeros);
        if (fwrite(zeros, 1, n, f) != n) break;
        done += n;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return unlink(path) == 0;
}

/* Blanks every string in the parsed JSON before it's freed. */
static void wipe_json(cJSON *j)
{
    for (; j; j = j->next) {
        if (cJSON_IsString(j) && j->valuestring) {
            memset(j->valuestring, 0, strlen(j->valuestring));
        }
        wipe_json(j->child);
    }
}

static bool valid_password(const char *p)
{
    size_t n = strlen(p);
    if (n == 0 || (n >= 8 && n <= 63)) return true;   /* open, or a WPA passphrase */
    if (n != 64) return false;
    for (size_t i = 0; i < n; i++) {                   /* 64 = raw hex key */
        if (!isxdigit((unsigned char)p[i])) return false;
    }
    return true;
}

/* Parses and checks the file; fills nets and returns the count, or -1 with
 * a reason in err. */
static int parse_wifi_json(const char *text, wifi_net_t *nets, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        snprintf(err, errlen, "it isn't valid JSON (check commas and quotes)");
        return -1;
    }
    int n = -1;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (!cJSON_IsArray(list)) {
        snprintf(err, errlen, "it needs a \"networks\" list");
    } else if (cJSON_GetArraySize(list) == 0) {
        snprintf(err, errlen, "the \"networks\" list is empty");
    } else if (cJSON_GetArraySize(list) > WIFI_LIST_MAX) {
        snprintf(err, errlen, "it has more than %d networks", WIFI_LIST_MAX);
    } else {
        n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, list) {
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(it, "ssid");
            const cJSON *pass = cJSON_GetObjectItemCaseSensitive(it, "password");
            if (!cJSON_IsString(ssid) || !ssid->valuestring[0] ||
                strlen(ssid->valuestring) > WIFI_SSID_MAX) {
                snprintf(err, errlen, "network %d needs an \"ssid\" of 1-%d characters",
                         n + 1, WIFI_SSID_MAX);
                n = -1;
                break;
            }
            if (pass && !cJSON_IsString(pass)) {
                snprintf(err, errlen, "the password for \"%s\" must be in quotes",
                         ssid->valuestring);
                n = -1;
                break;
            }
            const char *pw = pass ? pass->valuestring : "";
            if (!valid_password(pw)) {
                snprintf(err, errlen, "the password for \"%s\" must be 8-63 characters "
                         "(or empty for an open network)", ssid->valuestring);
                n = -1;
                break;
            }
            strlcpy(nets[n].ssid, ssid->valuestring, sizeof(nets[n].ssid));
            strlcpy(nets[n].pass, pw, sizeof(nets[n].pass));
            n++;
        }
    }
    wipe_json(root);
    cJSON_Delete(root);
    return n;
}

static void import_wifi(void)
{
    /* Windows may hide a ".txt" that Notepad added. */
    static const char *const names[] = { SD_DIR "/import/wifi.json",
                                         SD_DIR "/import/wifi.json.txt" };
    const char *path = NULL;
    struct stat st;
    for (size_t i = 0; i < 2 && !path; i++) {
        if (stat(names[i], &st) == 0) path = names[i];
    }
    if (!path) return;

    if (st.st_size <= 0 || st.st_size > WIFI_JSON_MAX) {
        sd_card_notice("wifi.json not imported: the file is %s", st.st_size <= 0 ? "empty" : "too big");
        return;
    }
    char *text = heap_caps_calloc(1, st.st_size + 1, MALLOC_CAP_SPIRAM);
    static wifi_net_t nets[WIFI_LIST_MAX];
    if (!text) return;
    FILE *f = fopen(path, "r");
    size_t len = f ? fread(text, 1, st.st_size, f) : 0;
    if (f) fclose(f);

    char err[120] = "";
    int n = len > 0 ? parse_wifi_json(text, nets, err, sizeof(err)) : -1;
    memset(text, 0, st.st_size + 1);
    free(text);

    if (n < 0) {
        sd_card_notice("wifi.json not imported: %s", err[0] ? err : "couldn't read it");
    } else {
        int added = 0;
        for (int i = 0; i < n; i++) added += !wifi_list_contains(nets[i].ssid);
        if (wifi_list_count() + added > WIFI_LIST_MAX) {
            sd_card_notice("wifi.json not imported: that would make %d saved networks (max %d). "
                   "Remove some in Settings > Network first.",
                   wifi_list_count() + added, WIFI_LIST_MAX);
        } else if (!wifi_list_add(nets, n)) {
            sd_card_notice("wifi.json not imported: saving failed");
        } else {
            bool deleted = secure_delete(path);
            sd_card_notice("Imported %d Wi-Fi network%s from the SD card%s", n, n == 1 ? "" : "s",
                   deleted ? "" : " (couldn't delete wifi.json: delete it by hand!)");
            net_time_networks_changed();
        }
    }
    memset(nets, 0, sizeof(nets));
}

static void mount_task(void *arg)
{
    (void)arg;
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = GPIO_NUM_NC;          /* CS is CH422G EXIO4, kept low */
    slot.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false, /* never wipe the user's card */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &s_card);
    if (err == ESP_FAIL) {
        ESP_LOGW(TAG, "Card found, but it has no FAT32 or exFAT file system (NTFS and "
                      "ext4 aren't supported)");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SD card found (%s)", esp_err_to_name(err));
    } else {
        sdmmc_card_print_info(stdout, s_card);
        s_pdrv = ff_diskio_get_pdrv_card(s_card);
        __atomic_store_n(&s_mounted, true, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Mounted at " SD_MOUNT_POINT);
        make_folders();
        import_wifi();
        app_keys_import_dir(SD_DIR "/import");   /* asked on screen, see launcher.c */
        dashpad_import_dir(SD_DIR "/import");
        ext_apps_rescan();
        if (settings_backup_import(sd_card_notice)) {
            vTaskDelay(pdMS_TO_TICKS(4000));      /* time to read the message */
            esp_restart();                       /* every app reloads its settings */
        }
    }
    vTaskDelete(NULL);
}

bool sd_card_fatfs_path(const char *path, char *out, size_t len)
{
    size_t n = strlen(SD_MOUNT_POINT);
    if (!sd_card_mounted() || strncmp(path, SD_MOUNT_POINT, n) != 0 ||
        (path[n] != 0 && path[n] != '/')) {
        return false;
    }
    return snprintf(out, len, "%d:%s", s_pdrv, path[n] ? path + n : "/") < (int)len;
}

void sd_card_start(void)
{
    xTaskCreate(mount_task, "sd_card", 8192, NULL, 2, NULL);
}
