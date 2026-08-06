#pragma once

#ifdef SIMULATOR

#include <string>

class GfxRenderer;

bool runPdfSimulatorAcceptance(GfxRenderer& renderer, std::string& error);

#endif
