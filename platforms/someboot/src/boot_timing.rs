//! Optional pre-MMU boot-phase timing (gated by the `boot-timing` cargo feature).
//!
//! The aarch64 early boot runs with the D-cache OFF from entry until `setup_sctlr`
//! turns the MMU + caches on, so every DRAM access in that window is uncached. To
//! decide whether enabling the MMU earlier is worth its (boot-bricking) risk, this
//! module brackets the pre-MMU phases with reads of the always-on ARM generic-timer
//! counter (register-only, so it works before the MMU is up) and reports the deltas
//! once we are safely running cached (`prime_entry`).
//!
//! Zero cost when the feature is off: `mark`/`report` compile to nothing.

/// Phase boundaries captured during pre-MMU boot. `Count` sizes the mark array.
#[derive(Clone, Copy)]
pub enum Mark {
    /// Start of the measured window (just before `primary_init_early`).
    Start = 0,
    FdtBegin = 1,
    FdtEnd = 2,
    PercpuBegin = 3,
    PercpuEnd = 4,
    TableBegin = 5,
    TableEnd = 6,
    /// MMU about to be enabled (just before `setup_sctlr`).
    MmuOn = 7,
    Count = 8,
}

#[cfg(feature = "boot-timing")]
mod imp {
    use super::Mark;
    use crate::ArchTrait;

    // Captured single-core with the MMU off, so a plain `static mut` is correct:
    // there is no other CPU yet, and exclusive/atomic loads (LDXR/LDAXR) are
    // unusable before the MMU is enabled anyway.
    static mut MARKS: [usize; Mark::Count as usize] = [0; Mark::Count as usize];

    pub fn mark(m: Mark) {
        unsafe {
            MARKS[m as usize] = crate::arch::Arch::systimer_tick();
        }
    }

    pub fn report() {
        let freq = (crate::arch::Arch::systimer_freq()).max(1) as u64;
        let at = |m: Mark| unsafe { MARKS[m as usize] };
        let ms = |a: usize, b: usize| (b.wrapping_sub(a) as u64 * 1000) / freq;
        let total = ms(at(Mark::Start), at(Mark::MmuOn));
        let fdt = ms(at(Mark::FdtBegin), at(Mark::FdtEnd));
        let percpu = ms(at(Mark::PercpuBegin), at(Mark::PercpuEnd));
        let table = ms(at(Mark::TableBegin), at(Mark::TableEnd));
        let other = total.saturating_sub(fdt + percpu + table);
        println!(
            "BOOT_TIMING total_uncached={total}ms fdt_parse={fdt}ms \
             percpu_copy={percpu}ms table_build={table}ms other={other}ms (freq={freq}Hz)"
        );
    }
}

#[cfg(not(feature = "boot-timing"))]
mod imp {
    use super::Mark;

    #[inline(always)]
    pub fn mark(_m: Mark) {}
    #[inline(always)]
    pub fn report() {}
}

pub use imp::{mark, report};
