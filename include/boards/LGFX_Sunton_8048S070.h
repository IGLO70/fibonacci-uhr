#pragma once

// ============================================================
// LovyanGFX-Konfiguration fuer Sunton ESP32-8048S070
// ESP32-S3, 7 Zoll, 800x480, 16-Bit RGB565
//
// Erster Hardwaretest:
//   - nur RGB-Display
//   - Touch noch deaktiviert
//   - Backlight wird in main.cpp direkt ueber GPIO2 geschaltet
// ============================================================

#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/touch/Touch_GT911.hpp>

class LGFX : public lgfx::LGFX_Device
{
public:
    lgfx::Bus_RGB   _bus_instance;
    lgfx::Panel_RGB _panel_instance;
    lgfx::Touch_GT911 _touch_instance;

    LGFX(void)
    {
        // --------------------------------------------------------
        // Panelgroesse
        // --------------------------------------------------------
        {
            auto cfg = _panel_instance.config();

            cfg.memory_width  = 800;
            cfg.memory_height = 480;
            cfg.panel_width   = 800;
            cfg.panel_height  = 480;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;

            _panel_instance.config(cfg);
        }

        // RGB-Framebuffer im PSRAM anlegen.
        {
            auto cfg = _panel_instance.config_detail();

            cfg.use_psram = 1;

            _panel_instance.config_detail(cfg);
        }

        // --------------------------------------------------------
        // 16-Bit RGB565 Bus
        //
        // Pinbelegung fuer das 7-Zoll ESP32-8048S070.
        // --------------------------------------------------------
        {
            auto cfg = _bus_instance.config();

            // Bei Bus_RGB muss der Panel-Zeiger vor init gesetzt sein.
            cfg.panel = &_panel_instance;

            // Blau B0..B4
            cfg.pin_d0 = GPIO_NUM_15;
            cfg.pin_d1 = GPIO_NUM_7;
            cfg.pin_d2 = GPIO_NUM_6;
            cfg.pin_d3 = GPIO_NUM_5;
            cfg.pin_d4 = GPIO_NUM_4;

            // Gruen G0..G5
            cfg.pin_d5  = GPIO_NUM_9;
            cfg.pin_d6  = GPIO_NUM_46;
            cfg.pin_d7  = GPIO_NUM_3;
            cfg.pin_d8  = GPIO_NUM_8;
            cfg.pin_d9  = GPIO_NUM_16;
            cfg.pin_d10 = GPIO_NUM_1;

            // Rot R0..R4
            cfg.pin_d11 = GPIO_NUM_14;
            cfg.pin_d12 = GPIO_NUM_21;
            cfg.pin_d13 = GPIO_NUM_47;
            cfg.pin_d14 = GPIO_NUM_48;
            cfg.pin_d15 = GPIO_NUM_45;

            // RGB-Steuersignale
            cfg.pin_henable = GPIO_NUM_41;  // DE
            cfg.pin_vsync   = GPIO_NUM_40;
            cfg.pin_hsync   = GPIO_NUM_39;
            cfg.pin_pclk    = GPIO_NUM_42;

            // Bewusst konservativer Startwert.
            cfg.freq_write = 14000000;

            // Fuer das 7-Zoll-Sunton verwendete Start-Timings.
            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 80;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 16;

            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 22;
            cfg.vsync_pulse_width = 13;
            cfg.vsync_back_porch  = 10;

            cfg.pclk_idle_high = 1;

            _bus_instance.config(cfg);
        }

        _panel_instance.setBus(&_bus_instance);

        // --------------------------------------------------------
        // GT911 Touch
        // Erfolgreich am realen Board gefunden:
        //   SDA  = GPIO19
        //   SCL  = GPIO20
        //   RST  = GPIO38
        //   I2C  = 0x5D
        // --------------------------------------------------------
        {
            auto cfg = _touch_instance.config();

            cfg.x_min = 0;
            cfg.x_max = 799;
            cfg.y_min = 0;
            cfg.y_max = 479;

            cfg.bus_shared = false;
            cfg.offset_rotation = 0;

            cfg.i2c_port = 0;
            cfg.i2c_addr = 0x5D;

            cfg.pin_sda = GPIO_NUM_19;
            cfg.pin_scl = GPIO_NUM_20;
            cfg.pin_int = GPIO_NUM_NC;
            cfg.pin_rst = GPIO_NUM_38;

            cfg.freq = 400000;

            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};
