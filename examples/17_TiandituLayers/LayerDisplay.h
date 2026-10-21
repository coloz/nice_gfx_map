#pragma once
#include <NICE_GFX.h>
#include <Fonts/UnifontCN16.h>
// Same tested Korvo-1 SUB3 RGB wiring as example16; other ESP32 targets use
// ILI9341. Override NICE_TDT_LAYERS_KORVO=0 to use SPI on S31 as well.
#ifndef NICE_TDT_LAYERS_KORVO
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define NICE_TDT_LAYERS_KORVO 1
#else
#define NICE_TDT_LAYERS_KORVO 0
#endif
#endif
namespace layer_display {
#if NICE_TDT_LAYERS_KORVO
constexpr int Width = 800, Height = 480, Header = 40, Footer = 40;
inline nice_gfx::RgbConfig config() {
  nice_gfx::RgbConfig c; c.reset = -1; c.createIo = false;
  c.rgb.data_width = 16; c.rgb.flags.fb_in_psram = true;
  c.rgb.pclk_gpio_num = static_cast<decltype(c.rgb.pclk_gpio_num)>(40);
  c.rgb.hsync_gpio_num = static_cast<decltype(c.rgb.pclk_gpio_num)>(44);
  c.rgb.vsync_gpio_num = static_cast<decltype(c.rgb.pclk_gpio_num)>(45);
  c.rgb.de_gpio_num = static_cast<decltype(c.rgb.pclk_gpio_num)>(43);
  c.rgb.disp_gpio_num = static_cast<decltype(c.rgb.pclk_gpio_num)>(38);
  constexpr int pins[] = {8,9,10,11,12,13,14,15,16,17,18,19,33,34,35,36};
  for (unsigned i = 0; i < 16; ++i)
    c.rgb.data_gpio_nums[i] = static_cast<decltype(c.rgb.pclk_gpio_num)>(pins[i]);
  c.rgb.clk_src = LCD_CLK_SRC_PLL160M; c.rgb.dma_burst_size = 64; c.rgb.num_fbs = 1;
  c.rgb.bounce_buffer_size_px = 0; c.usePpa = true;
  c.rgb.timings.pclk_hz = 18000000; c.rgb.timings.h_res = Width; c.rgb.timings.v_res = Height;
  c.rgb.timings.hsync_pulse_width = 40; c.rgb.timings.hsync_back_porch = 40; c.rgb.timings.hsync_front_porch = 48;
  c.rgb.timings.vsync_pulse_width = 23; c.rgb.timings.vsync_back_porch = 32; c.rgb.timings.vsync_front_porch = 13;
  c.rgb.timings.flags.hsync_idle_low = false; c.rgb.timings.flags.vsync_idle_low = false;
  c.rgb.timings.flags.de_idle_high = false; c.rgb.timings.flags.pclk_active_neg = true;
  return c;
}
inline NICE_GFX &display() { static nice_gfx::ST7262_800x480_RGB value(config()); return value; }
#else
constexpr int Width = 320, Height = 240, Header = 20, Footer = 20;
inline NICE_GFX &display() { static ILI9341_240x320_SPI value(5,2,4,18,23,19); return value; }
#endif
inline bool begin() {
  auto &d = display(); if (d.beginChecked() != nice_gfx::Status::Ok) return false;
#if !NICE_TDT_LAYERS_KORVO
  d.setRotation(1);
#endif
  d.setFont(nice_gfx::fonts::UnifontCN16); d.fillScreen(0xe71c); return true;
}
inline void bands(const char *title, const char *status) {
  auto &d = display(); d.fillRect(0,0,Width,Header,0x1167); d.fillRect(0,Height-Footer,Width,Footer,0x1167);
  d.setTextColor(0xffff,0x1167); d.drawString(title,4,(Header-16)/2);
  d.drawString(status,4,Height-Footer+(Footer-16)/2);
}
}
