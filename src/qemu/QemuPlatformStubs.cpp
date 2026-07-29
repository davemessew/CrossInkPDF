#ifdef CROSSINK_QEMU

#include <hal/adc_types.h>

// ESP32-C3 QEMU does not complete the ADC calibration event. ESP-IDF invokes
// this calibration from a global constructor before setup(), so the target
// otherwise spins forever before it can emit the first acceptance marker.
extern "C" void __wrap_adc_calc_hw_calibration_code(
    adc_unit_t, adc_atten_t) {}

#endif
