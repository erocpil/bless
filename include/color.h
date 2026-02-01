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

/* 普通颜色（暗色调） */
#define FG_BLACK          "\x1b[30m"
#define FG_RED            "\x1b[31m"
#define FG_GREEN          "\x1b[32m"
#define FG_YELLOW         "\x1b[33m"
#define FG_BLUE           "\x1b[34m"
#define FG_MAGENTA        "\x1b[35m"
#define FG_CYAN           "\x1b[36m"
#define FG_WHITE          "\x1b[37m"

/* 高亮颜色（亮色调） */
#define FG_BRIGHT_BLACK   "\x1b[90m"   // 或叫 FG_GRAY
#define FG_BRIGHT_RED     "\x1b[91m"
#define FG_BRIGHT_GREEN   "\x1b[92m"
#define FG_BRIGHT_YELLOW  "\x1b[93m"
#define FG_BRIGHT_BLUE    "\x1b[94m"
#define FG_BRIGHT_MAGENTA "\x1b[95m"
#define FG_BRIGHT_CYAN    "\x1b[96m"
#define FG_BRIGHT_WHITE   "\x1b[97m"   // 这就是 BRIGHT_GRAY

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
#define C_UI_TITLE    RGB_FG(255, 255, 255) // 纯白，最高优先级
#define C_UI_SECTION  RGB_FG(200, 200, 200) // 浅灰，次级标题
#define C_UI_KEY      RGB_FG(255, 220, 140) // 温暖金色，键名/标签
#define C_UI_VALUE    RGB_FG(180, 220, 255) // 冷色调蓝，数值/内容
#define C_UI_ACCENT   RGB_FG(140, 200, 255) // 亮蓝色，强调/链接
/* 状态指示颜色 */
#define C_UI_SUCCESS  RGB_FG(140, 220, 160)  // 成功/正常 - 柔和绿
#define C_UI_WARN     RGB_FG(255, 200, 120)  // 警告 - 琥珀黄
#define C_UI_ERROR    RGB_FG(255, 140, 140)  // 错误/失败 - 柔和红

/* ===== Dark background safe ===== */
#define C_DARK_GREY   RGB_FG(140, 150, 160) // 中性灰蓝，与整体柔和风格协调
#define C_DARK_GREEN  RGB_FG(100, 180, 130) /* → 🟢 薄荷绿色(偏蓝的绿) */
#define C_DARK_BLUE   RGB_FG(110, 160, 220) /* → 🔵 天空蓝(明亮的蓝) */
#define C_DARK_RED    RGB_FG(220, 100, 100) /* → 🔴 珊瑚红(柔和的红) */
#define C_DARK_YELLOW RGB_FG(220, 190, 120) /* → 🟡 琥珀黄(柔和的红) */
#define C_DARK_PURPLE RGB_FG(170, 130, 210) /* → 🟣 薰衣草紫(偏蓝的紫) */
// #define C_DARK_PURPLE RGB_FG(160, 120, 200) // 更深一点的紫 RGB 总和: 480
// #define C_DARK_PURPLE RGB_FG(200, 130, 190) // 偏红紫（Magenta 方向） RGB 总和: 520
// #define C_DARK_PURPLE RGB_FG(150, 130, 220) // 偏蓝紫（Violet 方向） RGB 总和: 500

#define COLOR(c) (log_color_enabled() ? (c) : "")

// 方案 1：Pastel（柔和）风格
#define C_PASTEL_INFO     RGB_FG(120, 220, 160)   /* 薄荷绿 */
#define C_PASTEL_WARN     RGB_FG(255, 200, 120)   /* 柔和金 */
#define C_PASTEL_ERROR    RGB_FG(255, 130, 130)   /* 柔和红 */
#define C_PASTEL_DEBUG    RGB_FG(140, 200, 255)   /* 柔和蓝 */
#define C_PASTEL_TRACE    RGB_FG(200, 160, 255)   /* 柔和紫 */
#define C_PASTEL_META     RGB_FG(160, 170, 190)   /* 柔和灰 */
// 方案 2：Vibrant（鲜明）风格
#define C_BIBRANT_INFO     RGB_FG(60, 220, 100)    /* 鲜绿 */
#define C_BIBRANT_WARN     RGB_FG(255, 180, 50)    /* 鲜橙 */
#define C_BIBRANT_ERROR    RGB_FG(255, 70, 90)     /* 鲜红 */
#define C_BIBRANT_DEBUG    RGB_FG(80, 180, 255)    /* 鲜蓝 */
#define C_BIBRANT_TRACE    RGB_FG(160, 100, 255)   /* 鲜紫 */
#define C_BIBRANT_META     RGB_FG(120, 140, 160)   /* 钢灰 */
// 方案 3：Warm（暖色）风格
#define C_WARM_INFO     RGB_FG(180, 220, 140)   /* 黄绿 */
#define C_WARM_WARN     RGB_FG(255, 200, 100)   /* 金黄 */
#define C_WARM_ERROR    RGB_FG(255, 110, 100)   /* 珊瑚 */
#define C_WARM_DEBUG    RGB_FG(200, 180, 220)   /* 淡紫 */
#define C_WARM_TRACE    RGB_FG(255, 160, 180)   /* 粉色 */
#define C_WARM_META     RGB_FG(200, 190, 180)   /* 米色 */
// 方案 4：Cool（冷色）风格
#define C_COOL_INFO     RGB_FG(100, 220, 180)   /* 青绿 */
#define C_COOL_WARN     RGB_FG(200, 220, 140)   /* 冷黄 */
#define C_COOL_ERROR    RGB_FG(255, 120, 140)   /* 玫瑰 */
#define C_COOL_DEBUG    RGB_FG(100, 180, 240)   /* 天空蓝 */
#define C_COOL_TRACE    RGB_FG(140, 160, 255)   /* 淡蓝紫 */
#define C_COOL_META     RGB_FG(140, 160, 180)   /* 冷灰 */

/* 成功/正向类 - 绿色系 */
#define C_SUCCESS  RGB_FG(80, 200, 120)    /* 你的 INFO */
#define C_MINT     RGB_FG(100, 220, 180)   /* 薄荷绿 */
#define C_LIME     RGB_FG(160, 220, 100)   /* 青柠绿 */

/* 警告类 - 橙黄色系 */
#define C_WARN     RGB_FG(255, 190, 70)    /* 你的 WARN */
#define C_AMBER    RGB_FG(255, 170, 90)    /* 琥珀色 */
#define C_GOLD     RGB_FG(255, 200, 100)   /* 金色 */
#define C_PEACH    RGB_FG(255, 180, 130)   /* 桃色 */

/* 错误/重要类 - 红色系 */
#define C_ERROR    RGB_FG(255, 90, 90)     /* 你的 ERR */
#define C_CORAL    RGB_FG(255, 120, 100)   /* 珊瑚红 */
#define C_ROSE     RGB_FG(255, 110, 130)   /* 玫瑰红 */

/* 信息/冷静类 - 蓝色系 */
#define C_DEBUG    RGB_FG(120, 180, 255)   /* 你的 DEBUG */
#define C_SKY      RGB_FG(100, 200, 255)   /* 天空蓝 */
#define C_OCEAN    RGB_FG(80, 160, 220)    /* 海洋蓝 */
#define C_STEEL    RGB_FG(130, 160, 200)   /* 钢蓝色 */

/* 特殊/辅助类 - 紫/粉色系 */
#define C_TRACE    RGB_FG(170, 140, 255)   /* 你的 TRACE */
#define C_VIOLET   RGB_FG(180, 120, 240)   /* 紫罗兰 */
#define C_MAUVE    RGB_FG(200, 150, 220)   /* 淡紫色 */
#define C_PINK     RGB_FG(255, 140, 180)   /* 粉红色 */

/* 中性类 - 灰/青色系 */
#define C_SLATE    RGB_FG(140, 160, 180)   /* 板岩灰 */
#define C_TEAL     RGB_FG(100, 200, 200)   /* 青色 */
#define C_AQUA     RGB_FG(120, 220, 220)   /* 水绿色 */
#define C_SILVER   RGB_FG(180, 190, 200)   /* 银色 */

/* Solarized 风格 */
#define SOL_GREEN   RGB_FG(133, 153, 0)
#define SOL_YELLOW  RGB_FG(181, 137, 0)
#define SOL_RED     RGB_FG(220, 50, 47)
#define SOL_BLUE    RGB_FG(38, 139, 210)
#define SOL_VIOLET  RGB_FG(108, 113, 196)
#define SOL_GREY    RGB_FG(147, 161, 161) // Solarized base0，经典配色

/* Nord 风格 */
#define NORD_GREEN  RGB_FG(163, 190, 140)
#define NORD_YELLOW RGB_FG(235, 203, 139)
#define NORD_RED    RGB_FG(191, 97, 106)
#define NORD_BLUE   RGB_FG(136, 192, 208)
#define NORD_PURPLE RGB_FG(180, 142, 173)
#define NORD_GREY   RGB_FG(216, 222, 233) // Nord Snow Storm (nord4)，偏亮的冷灰

/* Dracula 风格 */
#define DRAC_GREEN  RGB_FG(80, 250, 123)
#define DRAC_YELLOW RGB_FG(241, 250, 140)
#define DRAC_RED    RGB_FG(255, 85, 85)
#define DRAC_BLUE   RGB_FG(139, 233, 253)
#define DRAC_PURPLE RGB_FG(189, 147, 249)
#define DRAC_GREY   RGB_FG(98, 114, 164) // Dracula Comment，紫灰色调

#endif
