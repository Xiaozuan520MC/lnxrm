/* SMP management interface (C++ header).
 * Provides the SMP Manager class and IPI vector definitions. */
#pragma once
#ifdef __cplusplus

#include <types.h>

/* IPI vectors (above 48 to avoid IRQ range 32..47) */
#define IPI_VECTOR_RESCHEDULE  0xF0
#define IPI_VECTOR_PANIC       0xF1
#define IPI_VECTOR_STOP        0xF2

/* Maximum SIPI attempts before giving up on an AP */
#define AP_SIPI_MAX_RETRIES    3

/* AP startup timeout in milliseconds */
#define AP_STARTUP_TIMEOUT_MS  1000

namespace smp {

class Manager {
public:
    /* Initialize SMP: enumerate and start all APs. Called from start_kernel(). */
    static void init();

    /* Number of online CPUs (including BSP). */
    static u32 online_count();

    /* Send reschedule IPI to a specific CPU. */
    static void ipi_reschedule(u32 cpu_id);

    /* Broadcast reschedule IPI to all online CPUs. */
    static void ipi_reschedule_all();

    /* Send stop IPI to halt a specific CPU. */
    static void ipi_stop(u32 cpu_id);

    /* Broadcast stop IPI to all online CPUs. */
    static void ipi_stop_all();

    /* Send a generic IPI to a specific CPU. */
    static void ipi_send(u32 cpu_id, u32 vector);
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
