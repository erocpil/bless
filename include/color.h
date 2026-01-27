#ifndef __COLOR_H__
#define __COLOR_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================
 * 颜色检测
 * ========================= */
static inline int log_color_enabled(void)
{
    if (getenv("NO_COLOR")) {
        return 0;
	}

    return isatty(fileno(stdout));
}

/* =========================
 * ANSI & Truecolor
 * ========================= */
/* ANSI 基础 */
#define ANSI_RESET        "\x1b[0m"
#define ANSI_BOLD         "\x1b[1m"
#define ANSI_DIM          "\x1b[2m"

/* 现代好看配色 */
#define FG_GRAY           "\x1b[90m"
#define FG_RED            "\x1b[31m"
#define FG_GREEN          "\x1b[32m"
#define FG_YELLOW         "\x1b[33m"
#define FG_BLUE           "\x1b[34m"
#define FG_MAGENTA        "\x1b[35m"
#define FG_CYAN           "\x1b[36m"

/* 高亮（Bright） */
#define FG_BRIGHT_RED     "\x1b[91m"
#define FG_BRIGHT_GREEN   "\x1b[92m"
#define FG_BRIGHT_YELLOW  "\x1b[93m"
#define FG_BRIGHT_BLUE    "\x1b[94m"
#define FG_BRIGHT_MAGENTA "\x1b[95m"
#define FG_BRIGHT_CYAN    "\x1b[96m"

/* Truecolor foreground */
#define RGB_FG(r,g,b) "\x1b[38;2;" #r ";" #g ";" #b "m"

/* 精心挑选的现代配色 */
#define C_INFO     RGB_FG(80, 200, 120)    /* 柔和绿 */
#define C_WARN     RGB_FG(255, 190, 70)    /* 琥珀黄 */
#define C_ERR      RGB_FG(255, 90, 90)     /* 柔红 */
#define C_DEBUG    RGB_FG(120, 180, 255)   /* 冷蓝 */
#define C_TRACE    RGB_FG(170, 140, 255)   /* Lavender */

/* ===== Status / result ===== */
#define C_OK       RGB_FG( 90, 210, 140)   /* Fresh Green */
#define C_FAIL     RGB_FG(255, 110, 110)   /* Light Red */
#define C_PASS     RGB_FG(100, 200, 160)
#define C_RETRY    RGB_FG(255, 200, 120)
#define C_SKIP     RGB_FG(160, 160, 160)

/* ===== Performance / data ===== */
#define C_RATE     RGB_FG( 90, 180, 255)   /* Throughput */
#define C_LATENCY  RGB_FG(255, 140, 200)   /* Latency / tail */
#define C_COUNTER  RGB_FG(180, 180, 255)
#define C_BYTES    RGB_FG(120, 220, 220)
#define C_PACKET   RGB_FG(160, 200, 255)

/* ===== Metadata ===== */
#define C_META     RGB_FG(150, 150, 150)   /* 灰色元信息 */
#define C_DIMMETA  RGB_FG(120, 120, 120)
#define C_HINT     RGB_FG(140, 160, 180)
#define C_PATH     RGB_FG(130, 180, 150)
#define C_ID       RGB_FG(180, 180, 180)

/* ===== Accent / UI ===== */
#define C_TITLE    RGB_FG(255, 255, 255)
#define C_SECTION  RGB_FG(200, 200, 200)
#define C_KEY      RGB_FG(255, 220, 140)
#define C_VALUE    RGB_FG(180, 220, 255)
#define C_ACCENT   RGB_FG(140, 200, 255)

/* ===== Dark background safe ===== */
#define C_DARK_GREEN  RGB_FG(100, 180, 130)
#define C_DARK_BLUE   RGB_FG(110, 160, 220)
#define C_DARK_RED    RGB_FG(220, 100, 100)
#define C_DARK_YELLOW RGB_FG(220, 190, 120)

#define COLOR(c) (log_color_enabled() ? (c) : "")

#endif
