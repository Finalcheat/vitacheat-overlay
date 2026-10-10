#ifndef TEST_NATIVE_EVENTS_HOOKS_H
#define TEST_NATIVE_EVENTS_HOOKS_H
#ifndef VCO_PROCESS_EVENTS_TESTING
#error "Fault injection requires the test-only compilation define"
#endif
#include <stdint.h>
int vco_process_events_test_hold_queue(void);
void vco_process_events_test_release_queue(void);
void vco_process_events_test_seed_generation(uint32_t);
#endif
