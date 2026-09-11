/* gpsp64: cache-index placement for the hot dispatch chain.
 *
 * The VR4300's instruction cache is 16KB, direct-mapped, 32-byte lines:
 * a line's index is address bits 13:5, so two functions whose addresses
 * are congruent mod 16KB share cache lines and evict each other on every
 * call, no matter how far apart they are in memory.
 *
 * That is not a hypothetical here.  An I-cache miss histogram bucketed at
 * 1KB (ares in homebrew mode, see docs/CACHE_PROFILING.md) put 42% of all
 * instruction misses in four buckets, and those four buckets are three
 * functions that call each other on the hottest path in the emulator:
 *
 *   mips_update_gba   0x800e6400  216B   index 0x2400..0x24d7
 *   update_gba        0x8000e1c0 3264B   index 0x21c0..0x2e7f
 *   update_scanline   0x801220e0 4000B   index 0x20e0..0x307f
 *
 * update_gba is entirely inside update_scanline's index range, and
 * mips_update_gba -- the trampoline recompiled code jumps to every time
 * the cycle counter runs out -- is inside both.  The chain runs
 * mips_update_gba -> update_gba -> update_scanline at least once per
 * emulated scanline, and each step was evicting the one before it.
 *
 * Nothing chose that.  Function addresses fall out of link order, so the
 * collision appeared by accident and moves on its own whenever .text
 * changes size: measured over otherwise-identical builds, the same code
 * ran at 23.0, 24.0 and 26.0 ms/frame purely from where these functions
 * happened to land.  Alignment attributes do not fix it either -- they
 * round an address up, they do not choose a cache index, and two builds
 * with the same alignment landed 16KB apart.
 *
 * The durable fix is contiguity, not address.  Code laid out back to back
 * cannot alias itself: consecutive bytes get consecutive indices.  So the
 * chain is collected into one linker input-section group, which n64.ld
 * already gathers in a single place via
 *
 *     KEEP(*(keep.text.*))
 *
 * inside .text.  Every member lands in that one group, contiguously, in
 * whatever order the link produces -- and as long as the group stays
 * under 16KB (it is ~8.5KB) no member can collide with another, wherever
 * the group as a whole ends up.  This needs no linker script of our own
 * and no alignment luck, so it cannot silently regress the way the
 * accidental layout did.
 *
 * noinline is part of the contract: a function that gets inlined into a
 * caller is not in the section any more, and the group would silently
 * lose a member.
 *
 * tools/hotchain.py checks a linked ELF and reports any index overlap
 * that survives.
 *
 * Do not grow this group on the theory that more hot code in it is
 * better.  It was tried: adding the next seven functions the cycle
 * profile named -- sound_timer, the three block_lookup_address entries,
 * check_and_raise_interrupts, write_io_register32, translate_icache_sync,
 * 1,888 bytes in all -- took the group from 62% of the cache to 74% and
 * made the emulation path *slower* at all three .text layouts swept
 * (+5.6%, +0.7%, +1.3%).  The group's footprint is index space claimed
 * against everything outside it, and what is already inside is the
 * hottest code in the emulator, so the marginal cost of each addition is
 * paid by update_gba and update_scanline.  62% is a measured operating
 * point, not a budget with room in it.
 */

#ifndef N64_HOTCHAIN_H
#define N64_HOTCHAIN_H

/* -DN64_HOTCHAIN_OFF reverts to whatever placement the linker picks on its
 * own.  Kept because the only way to show this is worth anything is to
 * build it both ways at a matched .text size -- padding .text to move the
 * group also moves every global, so a pad sweep measures the D-cache
 * lottery as much as the I-cache one, and an A/B at one fixed size does
 * not. */
#if defined(N64) && !defined(N64_HOTCHAIN_OFF)
  #define N64_HOTCHAIN __attribute__((section("keep.text.gpsp_hotchain"), noinline))
#else
  #define N64_HOTCHAIN
#endif

#endif
