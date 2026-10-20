/*
 * microSD card: mount at boot and log what's on it. Never formats the card,
 * even if mounting fails.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"

/* Most cards over 32 GB come formatted as exFAT, which ESP-IDF ships
 * switched off. See "exFAT support" in README.md. */
#if !FF_FS_EXFAT
#error "exFAT is disabled: set FF_FS_EXFAT to 1 in $IDF_PATH/components/fatfs/src/ffconf.h (see README.md, 'exFAT support')"
#endif

#define PIN_MOSI  11
#define PIN_CLK   12
#define PIN_MISO  13
#define SHOW_MAX  2048   /* log the contents of small files up to this size */

static const char *TAG = "sd_card";

static sdmmc_card_t *s_card;
static bool          s_mounted;   /* __atomic */

bool sd_card_mounted(void)
{
    return __atomic_load_n(&s_mounted, __ATOMIC_ACQUIRE);
}

/* Logs the files in the card's top folder; small ones with their text. */
static void log_root(void)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open the card's top folder");
        return;
    }
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char path[300];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", de->d_name);
        struct stat st;
        bool is_dir = de->d_type == DT_DIR;
        long size = (!is_dir && stat(path, &st) == 0) ? (long)st.st_size : 0;
        count++;
        ESP_LOGI(TAG, "  %s%s  (%s%ld bytes)", de->d_name, is_dir ? "/" : "",
                 is_dir ? "folder, " : "", size);
        if (is_dir || size <= 0 || size > SHOW_MAX) continue;

        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[SHOW_MAX + 1];
        size_t n = fread(buf, 1, SHOW_MAX, f);
        fclose(f);
        buf[n] = '\0';
        printf("----- start of %s -----\n%s\n----- end of %s -----\n", de->d_name, buf,
               de->d_name);
    }
    closedir(dir);
    ESP_LOGI(TAG, "%d item(s) in the top folder", count);
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
        __atomic_store_n(&s_mounted, true, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Mounted at " SD_MOUNT_POINT);
        log_root();
    }
    vTaskDelete(NULL);
}

void sd_card_start(void)
{
    xTaskCreate(mount_task, "sd_card", 6144, NULL, 2, NULL);
}
