.syntax unified
.thumb
.section .text.fixture_target,"ax",%progbits
.align 1
.global fixture_target
.type fixture_target,%function
.thumb_func
fixture_target:
#if VCO_FIXTURE_VARIANT == 0
    movs r0, #7
#elif VCO_FIXTURE_VARIANT == 1
    movs r0, #11
#else
#error "Unknown fixture variant"
#endif
    bx lr
.size fixture_target, .-fixture_target
