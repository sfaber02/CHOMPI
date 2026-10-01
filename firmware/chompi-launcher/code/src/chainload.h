/** @file chainload.h
 *  @brief Hands control from the launcher to another CHOMPI firmware image.
 *
 *  CHOMPI apps are BOOT_SRAM images: the bootloader copies them out of QSPI into
 *  SRAM_EXEC at 0x24000000 and jumps there. Every app therefore runs at the same
 *  address, which is what makes it possible for one app to start another.
 *
 *  The catch is that the launcher is itself running from SRAM_EXEC, so the copy
 *  would overwrite the code performing it. The copy and the jump are done by a
 *  small position-independent routine that is relocated into ITCMRAM first.
 */
#pragma once
#include "daisy_seed.h"

namespace chompi
{
    /** Where every BOOT_SRAM app runs from. */
    static constexpr uint32_t kAppStart = 0x24000000U;

    /** Scratch in ITCMRAM for the relocated trampoline.
     *
     *  ITCMRAM is a separate 64K block at 0x00000000, so it survives the
     *  SRAM_EXEC copy. It needs no enabling: the stock bootloader stages
     *  program code here too (`itcmram_program` in its bootloader.cpp).
     *
     *  This has to be outside 0x24000000..0x24080000 entirely. A loaded image
     *  can exceed SRAM_EXEC's 232K and spill into the SRAM region behind it,
     *  because the linker gives SRAM's initialised data a load address inside
     *  SRAM_EXEC (`> SRAM AT > SRAM_EXEC`). The whole 512K block is fair game
     *  for the incoming firmware. */
    static constexpr uint32_t kTrampolineAddr = 0x00000000U;

    /** Generous upper bound; the real size comes from the linker below. The
     *  routine is ~62 bytes as built, and ITCMRAM has 64K going spare. */
    static constexpr uint32_t kTrampolineMax = 256U;

    /** The trampoline.
     *
     *  Runs with interrupts off and caches disabled, from ITCMRAM, after the
     *  launcher has already stopped every peripheral that could touch memory.
     *
     *  Written as a naked function in pure assembly on purpose: the compiler must
     *  not emit a literal pool or any PC-relative data reference, because the code
     *  is executed from an address it was not linked for. Every constant is built
     *  with movw/movt immediates and every input arrives in a register.
     *
     *  r0 = source (SDRAM), r1 = destination (SRAM_EXEC), r2 = length in bytes
     */
    __attribute__((naked, noinline)) static void Trampoline(uint32_t src,
                                                            uint32_t dst,
                                                            uint32_t len)
    {
        __asm volatile(
            /* Word-wise copy of the firmware image into SRAM_EXEC. The image is
               padded to a word boundary by the caller, so a 4-byte step is safe. */
            "   adds   r2, r2, #3          \n" /* round length up... */
            "   bics   r2, r2, #3          \n" /* ...to a whole number of words */
            "   mov    r3, #0              \n"
            "1:                            \n"
            "   cmp    r3, r2              \n"
            "   bge    2f                  \n"
            "   ldr    r12, [r0, r3]       \n"
            "   str    r12, [r1, r3]       \n"
            "   adds   r3, r3, #4          \n"
            "   b      1b                  \n"
            "2:                            \n"
            "   dsb                        \n"
            "   isb                        \n"

            /* Reset the chip rather than branching into the new image.
             *
             *  boot_info (in backup SRAM, set by the caller and untouched by
             *  the copy above) already tells the bootloader to jump here, and
             *  SRAM survives a warm reset. The bootloader's startup_process()
             *  runs before anything else, sees the request, clears it so it
             *  cannot fire twice, and jumps -- from completely clean hardware.
             *
             *  SCB->AIRCR = 0xE000ED0C, value VECTKEY | SYSRESETREQ. */
            "   movw   r3, #0xED0C         \n"
            "   movt   r3, #0xE000         \n"
            "   movw   r0, #0x0004         \n"
            "   movt   r0, #0x05FA         \n"
            "   str    r0, [r3]            \n"
            "   dsb                        \n"

            /* The reset takes a few cycles to land. */
            "3:  b      3b                 \n"

            /* Marks the end of the routine so its length is measured rather
               than guessed. The function is naked, so nothing follows. */
            "   .global chompi_trampoline_end \n"
            "chompi_trampoline_end:        \n"
            ::: "memory");
    }

    /** Supplied by the label at the tail of the assembly above. */
    extern "C" char chompi_trampoline_end[];

    /** Exact byte length of the trampoline, straight from the linker. */
    static inline uint32_t TrampolineSize()
    {
        /* Bit 0 of a Thumb function pointer is the interworking flag, not an
           address bit; mask it off to find the actual instructions. */
        const uint32_t start = (uint32_t)&Trampoline & ~1U;
        const uint32_t end   = (uint32_t)chompi_trampoline_end;
        return end - start;
    }

    /** @brief Does the trampoline fit in the space reserved for it?
     *
     *  Checked once at startup rather than at the point of no return. A
     *  truncated trampoline would run off the end of itself into whatever
     *  happens to be next in ITCMRAM, so this must never be allowed to pass
     *  silently if a future compiler emits something longer.
     */
    static bool CheckTrampolineFits() { return TrampolineSize() <= kTrampolineMax; }

    /** @brief Start the firmware image staged at @p image.
     *
     *  Does not return: the chip resets into the new firmware.
     *
     *  This mirrors how the stock bootloader starts an application. It does
     *  not branch into the image either -- it records the request in backup
     *  SRAM and resets, letting its own startup_process() do the jump from
     *  clean hardware on the way back up (see LoadProgramAndJump() in the
     *  bootloader's bootloader.cpp, where the direct-jump version is still
     *  there, commented out, next to the note that masking interrupts
     *  "seem[s] to cause errors for the target application").
     *
     *  Trying to hand over by branching means unwinding every peripheral and
     *  DMA stream by hand and getting all of it right; a reset does it
     *  atomically and for free.
     *
     *  @param image  staged image, in SDRAM, beginning with its vector table
     *  @param length image size in bytes
     */
    [[noreturn]] static void ChainLoad(const uint8_t *image, uint32_t length)
    {
        /* Past this point nothing may interrupt us: the code doing the copy is
           about to be overwritten. */
        __disable_irq();

        /* Stop every interrupt source outright. A pending IRQ that fired during
           shutdown would otherwise vector into half-copied memory. */
        for (uint32_t i = 0; i < 8; i++)
        {
            NVIC->ICER[i] = 0xFFFFFFFFU;
            NVIC->ICPR[i] = 0xFFFFFFFFU;
        }
        SysTick->CTRL = 0;

        /* The M7 caches would otherwise hold stale copies of both the code we are
           replacing and the data we are writing. Clean what is dirty, then run
           uncached so the trampoline's stores are visible to the instruction
           fetches that follow. */
        SCB_CleanDCache();
        SCB_DisableDCache();
        SCB_InvalidateICache();
        SCB_DisableICache();

        /* Relocate the trampoline into ITCMRAM, clear of the region it overwrites. */
        volatile uint8_t *tramp     = (volatile uint8_t *)kTrampolineAddr;
        const uint8_t    *tramp_src = (const uint8_t *)((uint32_t)&Trampoline & ~1U);

        uint32_t tramp_len = TrampolineSize();
        if (tramp_len > kTrampolineMax)
            tramp_len = kTrampolineMax; /* cannot happen; see CheckTrampolineFits() */

        for (uint32_t i = 0; i < tramp_len; i++)
            tramp[i] = tramp_src[i];

        __DSB();
        __ISB();

        /* Tell the bootloader where to jump once we come back up. This lives
           in backup SRAM, so it survives the reset and is not disturbed by the
           copy the trampoline is about to do. Staged here rather than inside
           the trampoline because the code that writes it is still intact. */
        boot_info.status = System::BootInfo::Type::JUMP;
        boot_info.data   = kAppStart;

        __DSB();

        /* Call into ITCMRAM, setting the Thumb bit on the way. */
        typedef void (*TrampolineFn)(uint32_t, uint32_t, uint32_t);
        TrampolineFn go = (TrampolineFn)(kTrampolineAddr | 1U);
        go((uint32_t)image, kAppStart, length);

        __builtin_unreachable();
    }

    /** @brief Sanity-check an image before handing control to it.
     *
     *  A BOOT_SRAM vector table opens with a stack pointer that must land inside
     *  RAM and a reset handler that must point into SRAM_EXEC. Rejecting anything
     *  else is what keeps a truncated download or a stray .bin from locking the
     *  unit up somewhere we cannot recover from without a power cycle.
     */
    static bool ImageLooksValid(const uint8_t *image, uint32_t length)
    {
        if (length < 8 || length > 0x80000U)
            return false;

        const uint32_t stack_ptr = *(const uint32_t *)(image + 0);
        const uint32_t entry = *(const uint32_t *)(image + 4);

        /* Stack lives in DTCMRAM or in the D1 SRAM block. */
        const bool stack_ok = (stack_ptr >= 0x20000000U && stack_ptr <= 0x20020000U) ||
                              (stack_ptr >= 0x24000000U && stack_ptr <= 0x24080000U);

        /* Entry point must be inside the region we are about to copy into, and
           must be a Thumb address. */
        const bool entry_ok = (entry >= kAppStart) &&
                              (entry < kAppStart + length) &&
                              ((entry & 1U) == 1U);

        return stack_ok && entry_ok;
    }

} // namespace chompi
