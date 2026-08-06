#pragma once

#include "EpubProductionTestState.h"

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ::epub_production_test::recordError(__VA_ARGS__)
#define LOG_INF(...) ((void)0)
