/*
 * Tab5 battery. See tab5_bat.h.
 *
 * The INA226 power monitor sits at 0x41 on the Tab5's internal I2C bus (the BSP bus, shared with
 * the touch controller and the IO expanders) and measures the battery pack on its bus input:
 * 1.25 mV per bit. The pack is 2S, so the charge estimate uses half the voltage per cell, from
 * 3.3 V (empty) to 4.15 V (full), as M5Unified does.
 */

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "tab5_bat.h"

static const char *TAG = "tab5_bat";

#define INA226_ADDR         0x41
#define INA226_CONFIG       0x00
#define INA226_SHUNT_V      0x01
#define INA226_BUS_V        0x02
#define NO_BATTERY_MV       5500        /* below a flat 2S pack: nothing inserted */

static i2c_master_dev_handle_t s_dev;   /* the INA226, NULL if tab5_bat_init() did not find it */

/* A 16-bit register (big endian); false if the monitor does not answer */
static bool read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t v[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, v, sizeof(v), 50) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)((v[0] << 8) | v[1]);
    return true;
}

/* Pack voltage in millivolts (1.25 mV per bit), 0 if it can't be read */
static int read_mv(void)
{
    uint16_t v;
    return read_reg(INA226_BUS_V, &v) ? v * 5 / 4 : 0;
}

bool tab5_bat_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus || i2c_master_probe(bus, INA226_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "Battery monitor (INA226) not found");
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }
    /* Average 16 samples, 1.1 ms conversions, shunt and bus continuously */
    const uint8_t conf[3] = { INA226_CONFIG, 0x45, 0x27 };
    i2c_master_transmit(s_dev, conf, sizeof(conf), 50);
    ESP_LOGI(TAG, "Battery monitor found, pack %d mV", read_mv());
    return true;
}

int tab5_bat_mv(void)
{
    const int mv = s_dev ? read_mv() : 0;
    return mv < NO_BATTERY_MV ? 0 : mv;
}

int tab5_bat_discharge_ma(void)
{
    uint16_t v;
    if (!s_dev || !read_reg(INA226_SHUNT_V, &v)) {
        return 0;
    }
    /* 2.5 uV per bit across the 5 mOhm shunt: 0.5 mA per bit, charging negative (as in M5Unified) */
    return (int16_t)v / 2;
}

int tab5_bat_percent(int mv)
{
    if (mv <= 0) {
        return -1;
    }
    const int cell = mv / 2;
    const int pct = (cell - 3300) * 100 / (4150 - 3300);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}
