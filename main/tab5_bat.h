/*
 * Tab5 battery: voltage of the Tab5's own NP-F battery pack (2S Li-ion), from its INA226 monitor.
 */

#pragma once

#include <stdbool.h>

/* Finds the INA226 on the BSP's I2C bus; false if it does not answer */
bool tab5_bat_init(void);

/* Pack voltage in millivolts, 0 if unknown (no monitor, read error, or no battery inserted) */
int tab5_bat_mv(void);

/* Current out of the pack in mA while it powers the Tab5, negative while it charges, 0 if unknown */
int tab5_bat_discharge_ma(void);

/* Charge estimated from the voltage, 0..100, or -1 if unknown */
int tab5_bat_percent(int mv);
