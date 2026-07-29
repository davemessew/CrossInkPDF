#pragma once

#ifdef CROSSINK_QEMU
class MappedInputManager;

void qemuAcceptanceBegin(MappedInputManager& input);
void qemuAcceptanceTick();
#endif
