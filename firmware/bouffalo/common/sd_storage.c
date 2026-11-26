// SPDX-License-Identifier: Apache-2.0
#include "sd_storage.h"

#include "sd_pins.h"

#if defined(CONFIG_FATFS_SDH_SDCARD)

#include "bflb_gpio.h"
#include "fatfs_diskio_register.h"

static FATFS sd_filesystem;
static bool driver_registered;
static bool filesystem_mounted;
static FRESULT mount_result = FR_NOT_READY;

static void sd_storage_gpio_init_1line(void) {
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    /* Board selects CLK/CMD/DAT0; DAT1-3 stay available to the display. */
    bflb_gpio_init(gpio, MPX_SD_CMD_PIN, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio, MPX_SD_CLK_PIN, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio, MPX_SD_DATA_PIN, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
}

bool sd_storage_mount(void) {
    if (filesystem_mounted) {
        return true;
    }

    sd_storage_gpio_init_1line();
    if (!driver_registered) {
        fatfs_sdh_driver_register();
        driver_registered = true;
    }

    mount_result = f_mount(&sd_filesystem, SD_STORAGE_MOUNT_PATH, 1);
    filesystem_mounted = mount_result == FR_OK;
    return filesystem_mounted;
}

bool sd_storage_reconnect(void) {
    if (filesystem_mounted) {
        (void)f_unmount(SD_STORAGE_MOUNT_PATH);
        filesystem_mounted = false;
    }

    return sd_storage_mount();
}

void sd_storage_release(void) {
    if (filesystem_mounted) {
        (void)f_unmount(SD_STORAGE_MOUNT_PATH);
        filesystem_mounted = false;
    }
    if (driver_registered) {
        fatfs_sdh_driver_unregister();
        driver_registered = false;
    }
    mount_result = FR_NOT_READY;
}

bool sd_storage_is_mounted(void) { return filesystem_mounted; }

FRESULT sd_storage_last_result(void) { return mount_result; }

#else

bool sd_storage_mount(void) { return false; }

bool sd_storage_reconnect(void) { return false; }

void sd_storage_release(void) {}

bool sd_storage_is_mounted(void) { return false; }

FRESULT sd_storage_last_result(void) { return FR_NOT_READY; }

#endif
