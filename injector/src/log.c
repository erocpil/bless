#include "log.h"

int g_log_format_json;

void log_set_format_json(int enabled)
{
    g_log_format_json = enabled;
}
