#ifndef __COLOR_H__
#define __COLOR_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================
 * color detection
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
/* ANSI basics */
#define ANSI_RESET        "\x1b[0m"
#define ANSI_BOLD         "\x1b[1m"
#define ANSI_DIM          "\x1b[2m"

/* Normal colors (dark tones) */
#define FG_BLACK          "\x1b[30m"
#define FG_RED            "\x1b[31m"
#define FG_GREEN          "\x1b[32m"
#define FG_YELLOW         "\x1b[33m"
#define FG_BLUE           "\x1b[34m"
#define FG_MAGENTA        "\x1b[35m"
#define FG_CYAN           "\x1b[36m"
#define FG_WHITE          "\x1b[37m"

/* Bright colors */
#define FG_BRIGHT_BLACK   "\x1b[90m"   // aka FG_GRAY
#define FG_BRIGHT_RED     "\x1b[91m"
#define FG_BRIGHT_GREEN   "\x1b[92m"
#define FG_BRIGHT_YELLOW  "\x1b[93m"
#define FG_BRIGHT_BLUE    "\x1b[94m"
#define FG_BRIGHT_MAGENTA "\x1b[95m"
#define FG_BRIGHT_CYAN    "\x1b[96m"
#define FG_BRIGHT_WHITE   "\x1b[97m"   // i.e. BRIGHT_GRAY

/* Truecolor foreground */
#define RGB_FG(r,g,b) "\x1b[38;2;" #r ";" #g ";" #b "m"

/* Curated modern palette */
#define C_INFO     RGB_FG(80, 200, 120)    /* Soft green */
#define C_WARN     RGB_FG(255, 190, 70)    /* Amber */
#define C_ERR      RGB_FG(255, 90, 90)     /* Soft red */
#define C_DEBUG    RGB_FG(120, 180, 255)   /* Cool blue */
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
#define C_META     RGB_FG(150, 150, 150)   /* Grey meta info */
#define C_DIMMETA  RGB_FG(120, 120, 120)
#define C_HINT     RGB_FG(140, 160, 180)
#define C_PATH     RGB_FG(130, 180, 150)
#define C_ID       RGB_FG(180, 180, 180)

/* ===== Accent / UI ===== */
#define C_UI_TITLE    RGB_FG(255, 255, 255) // Pure white, highest priority
#define C_UI_SECTION  RGB_FG(200, 200, 200) // Light grey, secondary heading
#define C_UI_KEY      RGB_FG(255, 220, 140) // Warm gold for keys/labels
#define C_UI_VALUE    RGB_FG(180, 220, 255) // Cool blue for values/content
#define C_UI_ACCENT   RGB_FG(140, 200, 255) // Bright blue for emphasis/links
/* Status indicator colours */
#define C_UI_SUCCESS  RGB_FG(140, 220, 160)  // Success/OK - soft green
#define C_UI_WARN     RGB_FG(255, 200, 120)  // Warning - amber
#define C_UI_ERROR    RGB_FG(255, 140, 140)  // Error/fail - soft red

/* ===== Dark background safe ===== */
#define C_DARK_GREY   RGB_FG(140, 150, 160) // Neutral grey-blue, fits soft palette
#define C_DARK_GREEN  RGB_FG(100, 180, 130) /* -> Mint green (blue-leaning) */
#define C_DARK_BLUE   RGB_FG(110, 160, 220) /* -> Sky blue */
#define C_DARK_RED    RGB_FG(220, 100, 100) /* -> Coral red (soft) */
#define C_DARK_YELLOW RGB_FG(220, 190, 120) /* -> Amber(soft red) */
#define C_DARK_PURPLE RGB_FG(170, 130, 210) /* -> Lavender purple (blue-leaning) */
// #define C_DARK_PURPLE RGB_FG(160, 120, 200) // Deeper purple, RGB sum: 480
// #define C_DARK_PURPLE RGB_FG(200, 130, 190) // Reddish purple (magenta-leaning), RGB sum: 520
// #define C_DARK_PURPLE RGB_FG(150, 130, 220) // Bluish purple (violet-leaning), RGB sum: 500

#define COLOR(c) (log_color_enabled() ? (c) : "")

// Scheme 1: Pastel
#define C_PASTEL_INFO     RGB_FG(120, 220, 160)   /* Mint green */
#define C_PASTEL_WARN     RGB_FG(255, 200, 120)   /* Soft gold */
#define C_PASTEL_ERROR    RGB_FG(255, 130, 130)   /* Soft red */
#define C_PASTEL_DEBUG    RGB_FG(140, 200, 255)   /* Soft blue */
#define C_PASTEL_TRACE    RGB_FG(200, 160, 255)   /* Soft purple */
#define C_PASTEL_META     RGB_FG(160, 170, 190)   /* Soft grey */
// Scheme 2: Vibrant
#define C_VIBRANT_INFO     RGB_FG(60, 220, 100)    /* Vivid green */
#define C_VIBRANT_WARN     RGB_FG(255, 180, 50)    /* Vivid orange */
#define C_VIBRANT_ERROR    RGB_FG(255, 70, 90)     /* Vivid red */
#define C_VIBRANT_DEBUG    RGB_FG(80, 180, 255)    /* Vivid blue */
#define C_VIBRANT_TRACE    RGB_FG(160, 100, 255)   /* Vivid purple */
#define C_VIBRANT_META     RGB_FG(120, 140, 160)   /* Steel grey */
// Scheme 3: Warm
#define C_WARM_INFO     RGB_FG(180, 220, 140)   /* Yellow-green */
#define C_WARM_WARN     RGB_FG(255, 200, 100)   /* Golden */
#define C_WARM_ERROR    RGB_FG(255, 110, 100)   /* Coral */
#define C_WARM_DEBUG    RGB_FG(200, 180, 220)   /* Pale purple */
#define C_WARM_TRACE    RGB_FG(255, 160, 180)   /* Pink */
#define C_WARM_META     RGB_FG(200, 190, 180)   /* Beige */
// Scheme 4: Cool
#define C_COOL_INFO     RGB_FG(100, 220, 180)   /* Teal */
#define C_COOL_WARN     RGB_FG(200, 220, 140)   /* Cool yellow */
#define C_COOL_ERROR    RGB_FG(255, 120, 140)   /* Rose */
#define C_COOL_DEBUG    RGB_FG(100, 180, 240)   /* Sky blue */
#define C_COOL_TRACE    RGB_FG(140, 160, 255)   /* Pale blue-purple */
#define C_COOL_META     RGB_FG(140, 160, 180)   /* Cool grey */

/* Success/positive -- greens */
#define C_SUCCESS  RGB_FG(80, 200, 120)    /* your INFO */
#define C_MINT     RGB_FG(100, 220, 180)   /* Mint green */
#define C_LIME     RGB_FG(160, 220, 100)   /* Lime green */

/* Warning -- orange-yellow */
#define C_WARN     RGB_FG(255, 190, 70)    /* your WARN */
#define C_AMBER    RGB_FG(255, 170, 90)    /* Amber */
#define C_GOLD     RGB_FG(255, 200, 100)   /* Gold */
#define C_PEACH    RGB_FG(255, 180, 130)   /* Peach */

/* Error/important -- reds */
#define C_ERROR    RGB_FG(255, 90, 90)     /* your ERR */
#define C_CORAL    RGB_FG(255, 120, 100)   /* Coral red */
#define C_ROSE     RGB_FG(255, 110, 130)   /* Rose red */

/* Info/calm -- blues */
#define C_DEBUG    RGB_FG(120, 180, 255)   /* your DEBUG */
#define C_SKY      RGB_FG(100, 200, 255)   /* Sky blue */
#define C_OCEAN    RGB_FG(80, 160, 220)    /* Ocean blue */
#define C_STEEL    RGB_FG(130, 160, 200)   /* Steel blue */

/* special/auxiliary -- purple/pink */
#define C_TRACE    RGB_FG(170, 140, 255)   /* your TRACE */
#define C_VIOLET   RGB_FG(180, 120, 240)   /* Violet */
#define C_MAUVE    RGB_FG(200, 150, 220)   /* Pale purple */
#define C_PINK     RGB_FG(255, 140, 180)   /* Pink */

/* neutral -- grey/teal */
#define C_SLATE    RGB_FG(140, 160, 180)   /* Slate grey */
#define C_TEAL     RGB_FG(100, 200, 200)   /* Teal */
#define C_AQUA     RGB_FG(120, 220, 220)   /* Aqua */
#define C_SILVER   RGB_FG(180, 190, 200)   /* Silver */

/* Solarized style */
#define SOL_GREEN   RGB_FG(133, 153, 0)
#define SOL_YELLOW  RGB_FG(181, 137, 0)
#define SOL_RED     RGB_FG(220, 50, 47)
#define SOL_BLUE    RGB_FG(38, 139, 210)
#define SOL_VIOLET  RGB_FG(108, 113, 196)
#define SOL_GREY    RGB_FG(147, 161, 161) // Solarized base0, classic palette

/* Nord style */
#define NORD_GREEN  RGB_FG(163, 190, 140)
#define NORD_YELLOW RGB_FG(235, 203, 139)
#define NORD_RED    RGB_FG(191, 97, 106)
#define NORD_BLUE   RGB_FG(136, 192, 208)
#define NORD_PURPLE RGB_FG(180, 142, 173)
#define NORD_GREY   RGB_FG(216, 222, 233) // Nord Snow Storm (nord4), cool light grey

/* Dracula style */
#define DRAC_GREEN  RGB_FG(80, 250, 123)
#define DRAC_YELLOW RGB_FG(241, 250, 140)
#define DRAC_RED    RGB_FG(255, 85, 85)
#define DRAC_BLUE   RGB_FG(139, 233, 253)
#define DRAC_PURPLE RGB_FG(189, 147, 249)
#define DRAC_GREY   RGB_FG(98, 114, 164) // Dracula Comment, purple-grey

#endif
