#ifndef __COLOR_H__
#define __COLOR_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static inline int log_color_enabled(void)
{
    return !getenv("NO_COLOR") && isatty(fileno(stdout));
}

#define ANSI_RESET        "\x1b[0m"
#define ANSI_BOLD         "\x1b[1m"
#define FG_RED            "\x1b[31m"
#define FG_GREEN          "\x1b[32m"
#define FG_YELLOW         "\x1b[33m"
#define FG_MAGENTA        "\x1b[35m"
#define FG_CYAN           "\x1b[36m"
#define FG_BRIGHT_BLACK   "\x1b[90m"   // aka FG_GRAY
#define FG_BRIGHT_MAGENTA "\x1b[95m"
#define FG_BRIGHT_WHITE   "\x1b[97m"   // i.e. BRIGHT_GRAY
#define RGB_FG(r,g,b) "\x1b[38;2;" #r ";" #g ";" #b "m"
#define C_INFO     RGB_FG(80, 200, 120)    /* Soft green */
#define C_WARN     RGB_FG(255, 190, 70)    /* your WARN */
#define C_ERR      RGB_FG(255, 90, 90)     /* Soft red */
#define C_DEBUG    RGB_FG(120, 180, 255)   /* your DEBUG */
#define C_TRACE    RGB_FG(170, 140, 255)   /* your TRACE */
#define C_RATE     RGB_FG( 90, 180, 255)   /* Throughput */
#define C_LATENCY  RGB_FG(255, 140, 200)   /* Latency / tail */
#define C_META     RGB_FG(150, 150, 150)   /* Grey meta info */
#define C_HINT     RGB_FG(140, 160, 180)
#define C_UI_VALUE    RGB_FG(180, 220, 255) // Cool blue for values/content
#define COLOR(c) (log_color_enabled() ? (c) : "")
#define C_MINT     RGB_FG(100, 220, 180)   /* Mint green */
#define C_LIME     RGB_FG(160, 220, 100)   /* Lime green */
#define C_SILVER   RGB_FG(180, 190, 200)   /* Silver */

#endif
