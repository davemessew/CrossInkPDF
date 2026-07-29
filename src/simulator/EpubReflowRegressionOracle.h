#pragma once

#ifdef SIMULATOR

#include <string>

class GfxRenderer;

bool runEpubReflowRegressionOracle(GfxRenderer& renderer, const char* bookPath, const char* passName,
                                   std::string& error);

#endif
