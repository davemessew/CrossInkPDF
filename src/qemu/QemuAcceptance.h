#pragma once

#ifdef CROSSINK_QEMU
class GfxRenderer;
class MappedInputManager;

void qemuAcceptanceBegin(MappedInputManager& input, GfxRenderer& renderer);
void qemuAcceptanceTick();
#endif
