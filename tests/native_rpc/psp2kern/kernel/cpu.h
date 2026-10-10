#include <stdint.h>
uint32_t fake_enter_syscall(void);
void fake_exit_syscall(uint32_t);
#define ENTER_SYSCALL(state) ((state) = fake_enter_syscall())
#define EXIT_SYSCALL(state) fake_exit_syscall(state)
