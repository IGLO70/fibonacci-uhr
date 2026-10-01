#pragma once

// ============================================================
// Zentrale Hardwareauswahl.
// Das passende Makro wird ausschliesslich in platformio.ini gesetzt.
// ============================================================

#if defined(BOARD_WT32_SC01)

  #include <TFT_eSPI.h>

  using DisplayDevice = TFT_eSPI;
  using DisplaySprite = TFT_eSprite;

  static constexpr const char* BOARD_NAME = "WT32-SC01";
  static constexpr uint32_t DISPLAY_FRAME_INTERVAL_MS = 30;
  static constexpr bool BOARD_HAS_EXTERNAL_RTC = true;
  static constexpr bool BOARD_COMPACT_NEON = false;

  static constexpr int BOARD_I2C_SDA = 18;
  static constexpr int BOARD_I2C_SCL = 19;
  static constexpr uint8_t BOARD_TOUCH_ADDR = 0x38;
  static constexpr uint8_t BOARD_TOUCH_STATUS_REG = 0x02;

#elif defined(BOARD_SUNTON_8048S070)

  #include "boards/LGFX_Sunton_8048S070.h"

  using DisplayDevice = LGFX;
  using DisplaySprite = LGFX_Sprite;

  static constexpr const char* BOARD_NAME = "Sunton ESP32-8048S070";
  static constexpr uint32_t DISPLAY_FRAME_INTERVAL_MS = 200;
  static constexpr bool BOARD_HAS_EXTERNAL_RTC = false;
  static constexpr bool BOARD_COMPACT_NEON = true;

  static constexpr int BOARD_BACKLIGHT_PIN = 2;

#else
  #error "Keine unterstuetzte Hardware gewaehlt. Bitte PlatformIO-Environment wt32-sc01 oder sunton-8048s070 verwenden."
#endif

// ============================================================
// Gemeinsame Font-Abstraktion
//
// WT32 / TFT_eSPI:
//   bewaehrte numerische Bitmap-Fontnummern bleiben unveraendert.
//
// Sunton / LovyanGFX:
//   aktuelle IFont-API; dadurch keine Deprecated-Warnungen.
// ============================================================

template <typename Gfx>
inline size_t boardDrawString(Gfx& gfx, const char* text,
                              int32_t x, int32_t y, uint8_t font)
{
#if defined(BOARD_WT32_SC01)
    return gfx.drawString(text, x, y, font);
#elif defined(BOARD_SUNTON_8048S070)
    const lgfx::IFont* selectedFont =
        (font >= 4) ? &fonts::Font4 : &fonts::Font2;
    return gfx.drawString(text, x, y, selectedFont);
#endif
}

template <typename Gfx>
inline size_t boardDrawString(Gfx& gfx, const String& text,
                              int32_t x, int32_t y, uint8_t font)
{
    // Beide Grafikbibliotheken koennen mit dem C-String arbeiten.
    return boardDrawString(gfx, text.c_str(), x, y, font);
}

template <typename Gfx>
inline size_t boardDrawNumber(Gfx& gfx, long value,
                              int32_t x, int32_t y, uint8_t font)
{
#if defined(BOARD_WT32_SC01)
    return gfx.drawNumber(value, x, y, font);
#elif defined(BOARD_SUNTON_8048S070)
    const lgfx::IFont* selectedFont =
        (font >= 4) ? &fonts::Font4 : &fonts::Font2;
    return gfx.drawNumber(value, x, y, selectedFont);
#endif
}

