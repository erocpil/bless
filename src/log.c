#include <string.h>
#include "log.h"
#include "color.h"
#include "system.h"

static const theme_config THEMES[] = {
	{
		.name   = "default",
		.green  = C_INFO,
		.yellow = C_WARN,
		.red    = C_ERR,
		.blue   = C_DEBUG,
		.purple = C_TRACE,
		.grey   = C_SILVER,
	},
	{
		.name   = "Dark",
		.green  = C_DARK_GREEN,
		.yellow = C_DARK_YELLOW,
		.red    = C_DARK_RED,
		.blue   = C_DARK_BLUE,
		.purple = C_DARK_PURPLE,
		.grey   = C_DARK_GREY,
	},
	{
		.name   = "Pastel",
		.green  = C_PASTEL_INFO,
		.yellow = C_PASTEL_WARN,
		.red    = C_PASTEL_ERROR,
		.blue   = C_PASTEL_DEBUG,
		.purple = C_PASTEL_TRACE,
		.grey   = C_PASTEL_META,
	},
	{
		.name   = "Vibrant",
		.green  = C_BIBRANT_INFO,
		.yellow = C_BIBRANT_WARN,
		.red    = C_BIBRANT_ERROR,
		.blue   = C_BIBRANT_DEBUG,
		.purple = C_BIBRANT_TRACE,
		.grey   = C_BIBRANT_META,
	},
	{
		.name   = "Warm",
		.green  = C_WARM_INFO,
		.yellow = C_WARM_WARN,
		.red    = C_WARM_ERROR,
		.blue   = C_WARM_DEBUG,
		.purple = C_WARM_TRACE,
		.grey   = C_WARM_META,
	},
	{
		.name   = "Cool",
		.green  = C_COOL_INFO,
		.yellow = C_COOL_WARN,
		.red    = C_COOL_ERROR,
		.blue   = C_COOL_DEBUG,
		.purple = C_COOL_TRACE,
		.grey   = C_COOL_META,
	},
	{
		.name   = "Accent-UI",
		.green  = C_UI_SUCCESS,
		.yellow = C_UI_WARN,
		.red    = C_UI_ERROR,
		.blue   = C_UI_ACCENT,
		.purple = C_UI_VALUE,
		.grey   = C_UI_SECTION,
	},
	{
		.name   = "Solarized",
		.green  = SOL_GREEN,
		.yellow = SOL_YELLOW,
		.red    = SOL_RED,
		.blue   = SOL_BLUE,
		.purple = SOL_VIOLET,
		.grey   = SOL_GREY,
	},
	{
		.name   = "Nord",
		.green  = NORD_GREEN,
		.yellow = NORD_YELLOW,
		.red    = NORD_RED,
		.blue   = NORD_BLUE,
		.purple = NORD_PURPLE,
		.grey   = NORD_GREY,
	},
	{
		.name   = "Nracula",
		.green  = DRAC_GREEN,
		.yellow = DRAC_YELLOW,
		.red    = DRAC_RED,
		.blue   = DRAC_BLUE,
		.purple = DRAC_PURPLE,
		.grey   = DRAC_GREY,
	},
	{
		.name   = "test",
		.green  = RGB_FG(180, 150, 123),
		.yellow = RGB_FG(141, 150, 140),
		.red    = RGB_FG(198, 114, 164),
		.blue   = RGB_FG(39, 133, 153),
		.purple = RGB_FG(189, 117, 149),
		.grey   = RGB_FG(155, 185, 185),
	},
	{ .name = NULL }
};

const theme_config *g_current_theme = &THEMES[0];

static inline int log_set_theme(const char *name)
{
	/* if no theme matched, use default */
	g_current_theme = &THEMES[0];

	if (!name) {
		return 0;
	}

	for (const theme_config *t = THEMES; t->name != NULL; t++) {
		if (strcasecmp(t->name, name) == 0) {
			g_current_theme = t;
			return 1;
		}
	}

	return 0;
}

inline void log_list_theme(void)
{
	printf("Available themes:\n");
	for (const theme_config *t = THEMES; t->name != NULL; t++) {
		const char *mark = (t == g_current_theme) ? " *" : "";
		printf("  - %s%s\n", t->name, mark);
	}
}

void log_show_all_theme(void)
{
	LOG_TRACE("Theme %p", THEMES);
	for (const theme_config *t = THEMES; t->name != NULL; t++) {
		LOG_META("  %-*s : %sHINT%s %sPATH%s %sINFO%s %sWARNING%s %sERROR%s %sTRACE%s",
				SYSTEM_THEME_LEN_MAX,
				t->name,
				t->green, ANSI_RESET,
				t->purple, ANSI_RESET,
				t->blue, ANSI_RESET,
				t->yellow, ANSI_RESET,
				t->red, ANSI_RESET,
				t->grey, ANSI_RESET);
	}
}

void log_show_theme()
{
	LOG_TRACE("Current theme %s", g_current_theme->name);
	LOG_HINT(" HINT");
	LOG_PATH(" PATH");
	LOG_INFO(" INFO");
	LOG_WARN(" WARN");
	LOG_ERR(" ERROR");
	LOG_TRACE(" TRACE");
}

// 获取当前主题名称
inline const char *log_get_theme_name(void)
{
	return g_current_theme->name;
}

void log_init(const char *theme)
{
	log_set_theme(theme);
}
