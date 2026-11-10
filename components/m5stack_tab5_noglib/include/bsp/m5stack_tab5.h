/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP BSP: M5Stack Tab5
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "bsp/config.h"
#include "driver/i2c_master.h"
#include "bsp/display.h"
#include "esp_io_expander.h"


#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "lvgl.h"
#include "esp_lvgl_port.h"
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0
/**************************************************************************************************
 *  BSP Board Name
 **************************************************************************************************/

/** @defgroup boardname Board Name
 *  @brief BSP Board Name
 *  @{
 */
#define BSP_BOARD_M5STACK_TAB5
/** @} */ // end of boardname

/**************************************************************************************************
 *  BSP Capabilities
 **************************************************************************************************/

/** @defgroup capabilities Capabilities
 *  @brief BSP Capabilities
 *  @{
 */
#define BSP_CAPS_DISPLAY          1
#define BSP_CAPS_TOUCH            1
#define BSP_CAPS_BUTTONS          0
#define BSP_CAPS_KNOB             0
#define BSP_CAPS_AUDIO            0
#define BSP_CAPS_AUDIO_SPEAKER    0
#define BSP_CAPS_AUDIO_MIC        0
#define BSP_CAPS_SDCARD           0
#define BSP_CAPS_LED              0
#define BSP_CAPS_CAMERA           0
#define BSP_CAPS_BAT              0
#define BSP_CAPS_IMU              0
#define BSP_CAPS_HUMITURE         0
/** @} */ // end of capabilities

/**************************************************************************************************
 *  Board pinout
 **************************************************************************************************/

/** @defgroup g01_i2c I2C
 *  @brief I2C BSP API
 *  @{
 */
#define BSP_I2C_SCL     (GPIO_NUM_32)
#define BSP_I2C_SDA     (GPIO_NUM_31)
/** @} */ // end of i2c

/** @defgroup g03_audio Audio
 *  @brief Audio BSP API
 *  @{
 */
#define BSP_I2S_MCLK         (GPIO_NUM_30)
#define BSP_I2S_SCLK         (GPIO_NUM_27)
#define BSP_I2S_LCLK         (GPIO_NUM_29)
#define BSP_I2S_DOUT         (GPIO_NUM_26)
#define BSP_I2S_DSIN         (GPIO_NUM_28)
#define BSP_POWER_AMP_IO     (GPIO_NUM_NC)
#define BSP_SPEAKER_EN       (IO_EXPANDER_PIN_NUM_1)
/** @} */ // end of audio

/** @defgroup g04_display Display and Touch
 *  @brief Display BSP API
 *  @{
 */
#define BSP_LCD_RST          (GPIO_NUM_NC)
#define BSP_LCD_BACKLIGHT    (GPIO_NUM_22)
#define BSP_LCD_EN           (IO_EXPANDER_PIN_NUM_4)
#define BSP_LCD_TOUCH_INT    (GPIO_NUM_23)
#define BSP_TOUCH_EN         (IO_EXPANDER_PIN_NUM_5)
/** @} */ // end of display

/** @defgroup g07_usb USB
 *  @brief USB BSP API
 *  @{
 */
#define BSP_USB_EN           (IO_EXPANDER_PIN_NUM_3)
#define BSP_USB_POS          (GPIO_NUM_20)
#define BSP_USB_NEG          (GPIO_NUM_19)
/** @} */ // end of usb

/** @defgroup g12_camera Camera
 *  @brief Camera BSP API
 *  @{
 */
#define BSP_CAMERA_GPIO_XCLK (GPIO_NUM_NC)
#define BSP_CAMERA_RST       (GPIO_NUM_NC)
#define BSP_CAMERA_EN        (IO_EXPANDER_PIN_NUM_6)
/** @} */ // end of camera

/** @defgroup g02_storage SD Card and SPIFFS
 *  @brief SPIFFS and SD card BSP API
 *  @{
 */
#define BSP_SD_D0            (GPIO_NUM_39)
#define BSP_SD_D1            (GPIO_NUM_40)
#define BSP_SD_D2            (GPIO_NUM_41)
#define BSP_SD_D3            (GPIO_NUM_42)
#define BSP_SD_CMD           (GPIO_NUM_44)
#define BSP_SD_CLK           (GPIO_NUM_43)
#define BSP_SD_DET           (GPIO_NUM_NC)
#define BSP_SD_SPI_CLK       (GPIO_NUM_43)
#define BSP_SD_SPI_MISO      (GPIO_NUM_39)
#define BSP_SD_SPI_MOSI      (GPIO_NUM_44)
#define BSP_SD_SPI_CS        (GPIO_NUM_42)
/** @} */ // end of storage
#define BSP_WIFI_EN          (IO_EXPANDER_PIN_NUM_0)

#ifdef __cplusplus
extern "C" {
#endif

/** \addtogroup g01_i2c
 *  @{
 */

/**************************************************************************************************
 *
 * I2C interface
 *
 **************************************************************************************************/
#define BSP_I2C_NUM     CONFIG_BSP_I2C_NUM

/**
 * @brief Init I2C driver
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 *      - ESP_FAIL              I2C driver installation error
 *
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief Deinit I2C driver and free its resources
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 *
 */
esp_err_t bsp_i2c_deinit(void);

/**
 * @brief Get I2C driver handle
 *
 * @return
 *      - I2C handle
 */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/** @} */ // end of i2c

/** \addtogroup g04_display
 *  @{
 */

/**************************************************************************************************
 *
 * LCD interface
 *
 * LVGL is used as graphics library. LVGL is NOT thread safe, therefore the user must take LVGL mutex
 * by calling bsp_display_lock() before calling any LVGL API (lv_...) and then give the mutex with
 * bsp_display_unlock().
 *
 * Display's backlight must be enabled explicitly by calling bsp_display_backlight_on()
 **************************************************************************************************/

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

/**
 * @brief BSP display (LVGL) configuration structure
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;  /*!< LVGL port configuration */
    uint32_t        buffer_size;    /*!< Size of the buffer for the screen in pixels */
    bool            double_buffer;  /*!< True, if should be allocated two buffers */
    struct {
        unsigned int buff_dma: 1;    /*!< Allocated LVGL buffer will be DMA capable */
        unsigned int buff_spiram: 1; /*!< Allocated LVGL buffer will be in PSRAM */
        unsigned int sw_rotate: 1;   /*!< Use software rotation (slower), The feature is unavailable under avoid-tear mode */
    } flags;
} bsp_display_cfg_t;

/**
 * @brief Initialize display
 *
 * This function initializes SPI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Initialize display
 *
 * This function initializes SPI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @param cfg display configuration
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/**
 * @brief Get pointer to input device (touch, buttons, ...)
 *
 * @note The LVGL input device is initialized in bsp_display_start() function.
 *
 * @return Pointer to LVGL input device or NULL when not initialized
 */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Take LVGL mutex
 *
 * @param timeout_ms Timeout in [ms]. 0 will block indefinitely.
 * @return true  Mutex was taken
 * @return false Mutex was NOT taken
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Give LVGL mutex
 *
 */
void bsp_display_unlock(void);

/**
 * @brief Set display enter sleep mode
 *
 * All the display (LCD, backlight, touch) will enter sleep mode.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_SUPPORTED if this function is not supported by the panel
 */
esp_err_t bsp_display_enter_sleep(void);

/**
 * @brief Set display exit sleep mode
 *
 * All the display (LCD, backlight, touch) will exit sleep mode.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NOT_SUPPORTED if this function is not supported by the panel
 */
esp_err_t bsp_display_exit_sleep(void);

/**
 * @brief Rotate screen
 *
 * Display must be already initialized by calling bsp_display_start()
 *
 * @param[in] disp Pointer to LVGL display
 * @param[in] rotation Angle of the display rotation
 */
void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/** @} */ // end of display

/** @defgroup g99_others Others
 *  @brief Other BSP API
 *  @{
 */

/**************************************************************************************************
 *
 * BSP Features
 *
 * This module provides an interface to enable and disable various features on this board.
 *
 * The bsp_feature_enable function is used to enable or disable a specified feature.
 *
 * Parameters:
 * - feature: The feature to enable or disable, of type bsp_feature_t enum.
 * - enable: A boolean value, true to enable the feature, false to disable it.
 *
 * Return value:
 * - esp_err_t: Error code indicating the result of the operation.
 *
 **************************************************************************************************/
typedef enum {
    BSP_FEATURE_LCD,
    BSP_FEATURE_TOUCH,
    BSP_FEATURE_SPEAKER,
    BSP_FEATURE_CAMERA,
    BSP_FEATURE_USB,
    BSP_FEATURE_WIFI
} bsp_feature_t;

/**
 * @brief Enable selected feature
 *
 * @param feature  Feature for enablement
 * @param enable   Enable/disable selected feature
 * @return
 *     - ESP_OK Success
 *     - ESP_ERR_INVALID_ARG Parameter error
 */
esp_err_t bsp_feature_enable(bsp_feature_t feature, bool enable);

/** @} */ // end of others

/** \addtogroup g07_usb
 *  @{
 */

/**************************************************************************************************
 *
 * USB
 *
 **************************************************************************************************/

/**
 * @brief Power modes of USB Host connector
 */
typedef enum bsp_usb_host_power_mode_t {
    BSP_USB_HOST_POWER_MODE_USB_DEV, //!< Power from USB DEV port
} bsp_usb_host_power_mode_t;

/**
 * @brief Start USB host
 *
 * This is a one-stop-shop function that will configure the board for USB Host mode
 * and start USB Host library
 *
 * @param[in] mode        USB Host connector power mode (Not used on this board)
 * @param[in] limit_500mA Limit output current to 500mA (Not used on this board)
 * @return
 *     - ESP_OK                 On success
 *     - ESP_ERR_INVALID_ARG    Parameter error
 *     - ESP_ERR_NO_MEM         Memory cannot be allocated
 */
esp_err_t bsp_usb_host_start(bsp_usb_host_power_mode_t mode, bool limit_500mA);

/**
 * @brief Stop USB host
 *
 * USB Host lib will be uninstalled and power from connector removed.
 *
 * @return
 *     - ESP_OK              On success
 *     - ESP_ERR_INVALID_ARG Parameter error
 */
esp_err_t bsp_usb_host_stop(void);

/** @} */ // end of usb

/** @defgroup g99_others Others
 *  @brief Other BSP API
 *  @{
 */

/**************************************************************************************************
 *
 * IO Expander Interface
 * IO expander: pi4ioe5v6408
 *
 **************************************************************************************************/
#define BSP_IO_EXPANDER_ADDRESS    (ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_LOW)
#define BSP_IO_EXPANDER_ADDRESS_1  (ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_HIGH)

/**
 * @brief Init IO expander
 * @note If the device was already initialized, users can also call it to get handle
 *
 * @return Pointer to device handle or NULL when error occurred
 */
esp_io_expander_handle_t bsp_io_expander_init(void);

/**
 * @brief Init second IO expander
 * @note If the device was already initialized, users can also call it to get handle
 *
 * @return Pointer to device handle or NULL when error occurred
 */
esp_io_expander_handle_t bsp_io_expander1_init(void);

/** @} */ // end of others

#ifdef __cplusplus
}
#endif
