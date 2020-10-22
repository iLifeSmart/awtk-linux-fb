﻿/**
 * File:   lcd_linux_fb.h
 * Author: AWTK Develop Team
 * Brief:  linux framebuffer lcd
 *
 * Copyright (c) 2018 - 2020  Guangzhou ZHIYUAN Electronics Co.,Ltd.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * License file for more details.
 *
 */

/**
 * History:
 * ================================================================
 * 2018-09-07 Li XianJing <xianjimli@hotmail.com> created
 *
 */

#include <signal.h>
#include "fb_info.h"
#include "tkc/mem.h"
#include "base/lcd.h"
#include "tkc/mutex.h"
#include "tkc/thread.h"
#include "tkc/time_now.h"
#include "awtk_global.h"
#include "lcd_mem_others.h"
#include "blend/image_g2d.h"
#include "base/system_info.h"
#include "lcd/lcd_mem_bgr565.h"
#include "lcd/lcd_mem_rgb565.h"
#include "lcd/lcd_mem_bgra8888.h"
#include "lcd/lcd_mem_rgba8888.h"

#ifndef WITH_LINUX_DRM

#define DISPLAY_SLEEP_TIME 16

static fb_info_t s_fb;
static int s_ttyfd = -1;
static tk_mutex_t* s_mutex = NULL;
static bool_t s_app_quited = FALSE;
static tk_thread_t* s_t_display = NULL;
static lcd_begin_frame_t lcd_begin_frame_fun = NULL;

static void on_app_exit(void) {
  fb_info_t* fb = &s_fb;

  s_app_quited = TRUE;
  if (s_ttyfd >= 0) {
    //ioctl(s_ttyfd, KDSETMODE, KD_TEXT);
  }

  log_info("wait for display thread quited \r\n");

  if (s_t_display != NULL) {
    tk_thread_join(s_t_display);
    tk_thread_destroy(s_t_display);
  }

  if (s_mutex != NULL) {
    tk_mutex_destroy(s_mutex);
  }

  fb_close(fb);

  log_debug("on_app_exit\n");
}

static ret_t lcd_linux_init_drawing_fb(lcd_mem_t* mem, bitmap_t* fb) {
  return_value_if_fail(mem != NULL && fb != NULL, RET_BAD_PARAMS);

  memset(fb, 0x00, sizeof(bitmap_t));

  fb->w = mem->base.w;
  fb->h = mem->base.h;
  fb->format = mem->format;
  fb->buffer = mem->offline_gb;
  graphic_buffer_attach(mem->offline_gb, mem->offline_fb, fb->w, fb->h);
  bitmap_set_line_length(fb, mem->line_length);

  return RET_OK;
}

static ret_t lcd_linux_init_online_fb(lcd_mem_t* mem, bitmap_t* fb, uint8_t* buff, uint32_t w, uint32_t h, uint32_t line_length) {
  return_value_if_fail(mem != NULL && fb != NULL && buff != NULL, RET_BAD_PARAMS);

  memset(fb, 0x00, sizeof(bitmap_t));

  fb->w = w;
  fb->h = h;
  fb->format = mem->format;
  fb->buffer = mem->online_gb;
  graphic_buffer_attach(mem->online_gb, buff, w, h);
  bitmap_set_line_length(fb, line_length);

  return RET_OK;
}

static void* display_thread(void* ctx) {
  uint32_t i = 0;
  uint32_t index = 0;
  fb_info_t* fb = &s_fb;
  int fb_nr = fb_number(fb);
  uint32_t size = fb_size(fb);
  lcd_mem_t* lcd = (lcd_mem_t*)ctx;
  lcd_orientation_t o = LCD_ORIENTATION_0;
  struct fb_var_screeninfo vi = (fb->var);

  log_info("display_thread start\n");
  while (!s_app_quited) {
    int32_t sleep_time = 0;
    uint32_t diff_time = 0;
    uint32_t start = time_now_ms();
    uint8_t* buff = fb->fbmem0 + size * i;

    vi.yoffset = i * fb_height(fb);
    tk_mutex_lock(s_mutex);
    o = system_info()->lcd_orientation;
    if (o == LCD_ORIENTATION_0) {
      ret_t ret = RET_FAIL;
#ifdef WITH_G2D
      bitmap_t online_fb;
      bitmap_t offline_fb;
      rect_t r = {0, 0, fb_width(fb), fb_height(fb)};

      lcd_linux_init_drawing_fb(lcd, &offline_fb);
      lcd_linux_init_online_fb(lcd, &online_fb, buff, fb_width(fb), fb_height(fb), fb_line_length(fb));

      ret = image_copy(&online_fb, &offline_fb, &r, r.x, r.y);
#endif /*WITH_G2D*/
      if (ret != RET_OK) {
        memcpy(buff, lcd->offline_fb, size);
      }
    } else {
      rect_t r = {0};
      bitmap_t online_fb;
      bitmap_t offline_fb;
      if (o == LCD_ORIENTATION_180) {
        r.x = 0;
        r.y = 0;
        r.w = fb_width(fb);
        r.h = fb_height(fb);
      } else {
        r.x = 0;
        r.y = 0;
        r.w = fb_height(fb);
        r.h = fb_width(fb);
      }

      lcd_linux_init_drawing_fb(lcd, &offline_fb);
      lcd_linux_init_online_fb(lcd, &online_fb, buff, fb_width(fb), fb_height(fb), fb_line_length(fb));

      image_rotate(&online_fb, &offline_fb, &r, o);
    }
    tk_mutex_unlock(s_mutex);

    if (ioctl(fb->fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
      perror("active fb swap failed");
    }
    diff_time = time_now_ms() - start;
    sleep_time = DISPLAY_SLEEP_TIME - diff_time;
    if (sleep_time > 0) {
      sleep_ms(sleep_time);
    }

#if 0
    log_info("index=%u: i=%u cost=%u yoffset=%u buff=%p size=%u\n", index, i, 
        (time_now_ms() - start), vi.yoffset,
        buff, size);
#endif
    index++;
    i = index % fb_nr;
  }
  log_info("display_thread end\n");

  return NULL;
}

static void on_signal_int(int sig) {
  tk_quit();
}

static ret_t lcd_mem_linux_sync(lcd_t* lcd) {
  fb_info_t* fb = (fb_info_t*)(lcd->impl_data);

  fb_sync(fb);

  return RET_OK;
}

static lcd_t* lcd_linux_create_flushable(fb_info_t* fb) {
  lcd_t* lcd = NULL;
  int w = fb_width(fb);
  int h = fb_height(fb);
  int line_length = fb_line_length(fb);

  int bpp = fb_bpp(fb);
  int size = fb_size(fb);
  uint8_t* online_fb = (uint8_t*)(fb->fbmem0);

  fb->fbmem1 = (uint8_t*)malloc(size);
  return_value_if_fail(fb->fbmem1 != NULL, NULL);

  if (bpp == 16) {
    if (fb_is_bgra5551(fb)) {
      lcd = lcd_mem_bgra5551_create(fb);
    } else if (fb_is_bgr565(fb)) {
      lcd = lcd_mem_bgr565_create_double_fb(w, h, online_fb, fb->fbmem1);
    } else if (fb_is_rgb565(fb)) {
      lcd = lcd_mem_rgb565_create_double_fb(w, h, online_fb, fb->fbmem1);
    } else {
      assert(!"not supported framebuffer format.");
    }
  } else if (bpp == 32) {
    if (fb_is_bgra8888(fb)) {
      lcd = lcd_mem_bgra8888_create_double_fb(w, h, online_fb, fb->fbmem1);
    } else if (fb_is_rgba8888(fb)) {
      lcd = lcd_mem_rgba8888_create_double_fb(w, h, online_fb, fb->fbmem1);
    } else {
      assert(!"not supported framebuffer format.");
    }
  } else if (bpp == 24) {
    assert(!"not supported framebuffer format.");
  } else {
    assert(!"not supported framebuffer format.");
  }

  if (lcd != NULL) {
    lcd->impl_data = fb;
    lcd->sync = lcd_mem_linux_sync;
    lcd_mem_set_line_length(lcd, line_length);
  }

  return lcd;
}

static ret_t lcd_mem_linux_lock(lcd_t* lcd, rect_t* dirty_rect) {
  ret_t ret = RET_OK;
  if (lcd->draw_mode != LCD_DRAW_OFFLINE) {
    tk_mutex_lock(s_mutex);
    if (lcd_begin_frame_fun != NULL) {
      ret = lcd_begin_frame_fun(lcd, dirty_rect);
    }
  }

  return ret;
}

static ret_t lcd_mem_linux_unlock(lcd_t* lcd) {
  if (lcd->draw_mode != LCD_DRAW_OFFLINE) {
    tk_mutex_unlock(s_mutex);
  }

  return RET_OK;
}

static lcd_t* lcd_linux_create_swappable(fb_info_t* fb) {
  lcd_t* lcd = NULL;
  int w = fb_width(fb);
  int h = fb_height(fb);
  int bpp = fb_bpp(fb);
  int line_length = fb_line_length(fb);
  uint8_t* buff = (uint8_t*)TKMEM_ALLOC(fb_size(fb));

  if (bpp == 16) {
    if (fb_is_bgr565(fb)) {
      lcd = lcd_mem_bgr565_create_single_fb(w, h, buff);
    } else if (fb_is_rgb565(fb)) {
      lcd = lcd_mem_rgb565_create_single_fb(w, h, buff);
    } else {
      assert(!"not supported framebuffer format.");
    }
  } else if (bpp == 32) {
    if (fb_is_bgra8888(fb)) {
      lcd = lcd_mem_bgra8888_create_single_fb(w, h, buff);
    } else if (fb_is_rgba8888(fb)) {
      lcd = lcd_mem_rgba8888_create_single_fb(w, h, buff);
    } else {
      assert(!"not supported framebuffer format.");
    }
  } else if (bpp == 24) {
    assert(!"not supported framebuffer format.");
  } else {
    assert(!"not supported framebuffer format.");
  }

  if (lcd != NULL) {
    lcd_begin_frame_fun = lcd->begin_frame;

    lcd->swap = lcd_mem_linux_unlock;
    lcd->flush = lcd_mem_linux_unlock;
    lcd->begin_frame = lcd_mem_linux_lock;
    ((lcd_mem_t*)lcd)->own_offline_fb = TRUE;
    lcd_mem_set_line_length(lcd, line_length);

    s_mutex = tk_mutex_create();

    s_t_display = tk_thread_create(display_thread, lcd);
    tk_thread_start(s_t_display);
  }

  return lcd;
}

static lcd_t* lcd_linux_create(fb_info_t* fb) {
  if (fb_is_1fb(fb)) {
    return lcd_linux_create_flushable(fb);
  } else {
    return lcd_linux_create_swappable(fb);
  }
}

static bool_t check_if_run_in_vmware() {
  bool_t run_in_vmware = FALSE;

  FILE* dmidecode = popen("dmidecode -s system-product-name", "r");
  if (dmidecode) {
    char system_product_name[32] = {0};
    char* result = fgets(system_product_name, sizeof(system_product_name) - 1, dmidecode);

    if (result && strstr(result, "VMware")) {
      run_in_vmware = TRUE;
    }
    pclose(dmidecode);
  }
  return run_in_vmware;
}

lcd_t* lcd_linux_fb_create(const char* filename) {
  lcd_t* lcd = NULL;
  fb_info_t* fb = &s_fb;
  return_value_if_fail(filename != NULL, NULL);

  if (fb_open(fb, filename) == 0) {
    s_ttyfd = open("/dev/tty1", O_RDWR);
    if (s_ttyfd >= 0) {
      ioctl(s_ttyfd, KDSETMODE, KD_GRAPHICS);
    }

    // fix FBIOPUT_VSCREENINFO block issue when run in vmware double fb mode
    if (check_if_run_in_vmware()) {
      log_info("run in vmware and fix FBIOPUT_VSCREENINFO block issue\n");
      fb->var.activate = FB_ACTIVATE_INV_MODE;
      fb->var.pixclock = 60;
    }

    lcd = lcd_linux_create(fb);
  }

  atexit(on_app_exit);
  signal(SIGINT, on_signal_int);

  return lcd;
}

#endif /*no WITH_LINUX_DRM*/
