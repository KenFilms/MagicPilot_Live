idf_component_register(
    SRCS "main.c" "overlay.c" "gfx.c" "fonts.c" "ui.c" "ui_draw.c" "cam.c" "cam_fmt.c" "cam_ble.c" "tab5_bat.c"
         "sd_presets.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_lcd_touch esp_driver_jpeg esp_driver_ppa esp_driver_i2c esp_timer bt nvs_flash esp_hosted
             fatfs sdmmc esp_driver_sdmmc json usb_host_uvc
)
