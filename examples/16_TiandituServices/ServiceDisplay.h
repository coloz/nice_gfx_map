#pragma once
#include <NICE_GFX.h>
#include <Fonts/UnifontCN16.h>

// ESP32-S31 defaults to the tested Korvo-1 SUB3 RGB wiring. Other ESP32 boards
// use the same ILI9341 wiring as example 02. Define this to 0 to use that SPI
// display on S31 as well. This serial-controlled example does not use touch.
#ifndef NICE_TDT_USE_KORVO
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define NICE_TDT_USE_KORVO 1
#else
#define NICE_TDT_USE_KORVO 0
#endif
#endif
namespace service_display {
inline nice_gfx::Status lastError=nice_gfx::Status::NotInitialized;
#if NICE_TDT_USE_KORVO
constexpr int Width=800,Height=480,Header=40,Footer=40;
inline nice_gfx::RgbConfig config(){
  nice_gfx::RgbConfig c;c.reset=-1;c.createIo=false;
  c.rgb.data_width=16;c.rgb.flags.fb_in_psram=true;
  c.rgb.pclk_gpio_num=static_cast<decltype(c.rgb.pclk_gpio_num)>(40);
  c.rgb.hsync_gpio_num=static_cast<decltype(c.rgb.hsync_gpio_num)>(44);
  c.rgb.vsync_gpio_num=static_cast<decltype(c.rgb.vsync_gpio_num)>(45);
  c.rgb.de_gpio_num=static_cast<decltype(c.rgb.de_gpio_num)>(43);
  c.rgb.disp_gpio_num=static_cast<decltype(c.rgb.disp_gpio_num)>(38);
  constexpr int pins[]={8,9,10,11,12,13,14,15,16,17,18,19,33,34,35,36};
  for(unsigned i=0;i<16;++i)c.rgb.data_gpio_nums[i]=static_cast<decltype(c.rgb.pclk_gpio_num)>(pins[i]);
  c.rgb.clk_src=LCD_CLK_SRC_PLL160M;c.rgb.dma_burst_size=64;c.rgb.num_fbs=1;
  c.rgb.bounce_buffer_size_px=0;c.usePpa=true;
  c.rgb.timings.pclk_hz=18000000;c.rgb.timings.h_res=Width;c.rgb.timings.v_res=Height;
  c.rgb.timings.hsync_pulse_width=40;c.rgb.timings.hsync_back_porch=40;c.rgb.timings.hsync_front_porch=48;
  c.rgb.timings.vsync_pulse_width=23;c.rgb.timings.vsync_back_porch=32;c.rgb.timings.vsync_front_porch=13;
  c.rgb.timings.flags.hsync_idle_low=false;c.rgb.timings.flags.vsync_idle_low=false;
  c.rgb.timings.flags.de_idle_high=false;c.rgb.timings.flags.pclk_active_neg=true;
  return c;
}
inline NICE_GFX &display(){static nice_gfx::ST7262_800x480_RGB instance(config());return instance;}
#else
constexpr int Width=320,Height=240,Header=20,Footer=20;
inline NICE_GFX &display(){static ILI9341_240x320_SPI instance(5,2,4,18,23,19);return instance;}
#endif
constexpr uint16_t Ink=0x1167,Paper=0xf7be,Lime=0xd78d;
inline bool begin(){auto &d=display();lastError=d.beginChecked();if(lastError!=nice_gfx::Status::Ok)return false;
#if !NICE_TDT_USE_KORVO
  d.setRotation(1);
#endif
  d.setFont(nice_gfx::fonts::UnifontCN16);d.fillScreen(Paper);return true;
}
inline void header(const char *value){auto &d=display();d.fillRect(0,0,Width,Header,Ink);d.setTextColor(0xffff,Ink);d.drawString(value,4,(Header-16)/2);}
inline void footer(const char *value){auto &d=display();d.fillRect(0,Height-Footer,Width,Footer,Ink);d.setTextColor(Lime,Ink);d.drawString(value,4,Height-Footer+(Footer-16)/2);}
inline void clear(){display().fillRect(0,Header,Width,Height-Header-Footer,Paper);}
inline void text(const char *value,int row){auto &d=display();d.setTextColor(Ink,Paper);d.drawString(value,8,Header+8+row*22);}
}
