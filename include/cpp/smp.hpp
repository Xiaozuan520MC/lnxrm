/* SMP management interface (C++ header).
 * Provides the SMP Manager class and IPI vector definitions. */
#pragma once
#ifdef __cplusplus

#include <types.h>

/* IPI vectors (above 48 to avoid IRQ range 32..47) */
#define IPI_VECTOR_RESCHEDULE 0xF0
#define IPI_VECTOR_STOP       0xF2

/* Maximum SIPI attempts before giving up on an AP */
#define AP_SIPI_MAX_RETRIES 3

/* AP startup timeout in milliseconds */
#define AP_STARTUP_TIMEOUT_MS 1000

namespace smp
{

class Manager
{
  public:
    /* Initialize SMP: enumerate and start all APs. Called from start_kernel(). */
    static void init();
};

} // namespace smp

/* C linkage for assembly and C callers */
extern "C" {
#endif

void smp_init(void);
void ap_main(unsigned int ap_id);

#ifdef __cplusplus
}
#endif
