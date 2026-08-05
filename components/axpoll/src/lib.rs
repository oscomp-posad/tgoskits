//! A library for polling I/O events and waking up tasks.

#![no_std]
#![deny(missing_docs)]

extern crate alloc;

use alloc::boxed::Box;
use core::{
    mem::MaybeUninit,
    task::{Context, Waker},
};

use ax_kspin::SpinNoIrq;
use bitflags::bitflags;
use linux_raw_sys::general::*;
use spin::Once;

bitflags! {
    /// I/O events.
    #[derive(Debug, Clone, Copy)]
    pub struct IoEvents: u32 {
        /// Available for read
        const IN     = POLLIN;
        /// Urgent data for read
        const PRI    = POLLPRI;
        /// Available for write
        const OUT    = POLLOUT;

        /// Error condition
        const ERR    = POLLERR;
        /// Hang up
        const HUP    = POLLHUP;
        /// Invalid request
        const NVAL   = POLLNVAL;

        /// Equivalent to [`IN`](Self::IN)
        const RDNORM = POLLRDNORM;
        /// Priority band data can be read
        const RDBAND = POLLRDBAND;
        /// Equivalent to [`OUT`](Self::OUT)
        const WRNORM = POLLWRNORM;
        /// Priority data can be written
        const WRBAND = POLLWRBAND;

        /// Message
        const MSG    = POLLMSG;
        /// Remove
        const REMOVE = POLLREMOVE;
        /// Stream socket peer closed connection, or shut down writing half of connection.
        const RDHUP  = POLLRDHUP;

        /// Events that are always polled even without specifying them.
        const ALWAYS_POLL = Self::ERR.bits() | Self::HUP.bits();
    }
}

/// Trait for types that can be polled for I/O events.
pub trait Pollable {
    /// Polls for I/O events.
    fn poll(&self) -> IoEvents;

    /// Registers wakers for I/O events.
    fn register(&self, context: &mut Context<'_>, events: IoEvents);
}

const POLL_SET_CAPACITY: usize = 64;

struct Entry {
    waker: Waker,
    interests: IoEvents,
}

impl Entry {
    fn wake(self) {
        self.waker.wake();
    }
}

struct Inner {
    entries: Box<[MaybeUninit<Entry>]>,
    cursor: usize,
}

impl Inner {
    fn new() -> Self {
        Self {
            entries: Box::new_uninit_slice(POLL_SET_CAPACITY),
            cursor: 0,
        }
    }

    fn len(&self) -> usize {
        self.cursor.min(POLL_SET_CAPACITY)
    }

    fn register(&mut self, waker: &Waker, interests: IoEvents) -> Option<Entry> {
        let slot = self.cursor % POLL_SET_CAPACITY;
        let replaced = if self.cursor >= POLL_SET_CAPACITY {
            let old = unsafe { self.entries[slot].assume_init_read() };
            let replaced = (!old.waker.will_wake(waker)).then_some(old);
            self.cursor = ((slot + 1) % POLL_SET_CAPACITY) + POLL_SET_CAPACITY;
            replaced
        } else {
            self.cursor += 1;
            None
        };
        self.entries[slot].write(Entry {
            waker: waker.clone(),
            interests,
        });
        replaced
    }
}

impl Drop for Inner {
    fn drop(&mut self) {
        for i in 0..self.len() {
            unsafe { self.entries[i].assume_init_read() }.wake();
        }
    }
}

/// A data structure for waking up tasks that are waiting for I/O events.
pub struct PollSet(Once<SpinNoIrq<Inner>>);

impl Default for PollSet {
    fn default() -> Self {
        Self::new()
    }
}

impl PollSet {
    /// Creates a new empty [`PollSet`].
    pub const fn new() -> Self {
        Self(Once::new())
    }

    /// Registers a waker for the requested I/O events.
    ///
    /// # Safety
    ///
    /// This method is task/deferred-context only. Callers must not invoke it
    /// from hard IRQ, NMI, or trap callbacks, and must not hold locks that may
    /// be re-entered by the registered waker or by poll wakeup paths.
    pub unsafe fn register(&self, waker: &Waker, interests: IoEvents) {
        let replaced = {
            self.0
                .call_once(|| SpinNoIrq::new(Inner::new()))
                .lock()
                .register(waker, interests)
        };
        if let Some(entry) = replaced {
            entry.wake();
        }
    }

    /// Wakes up registered wakers whose interests intersect `ready`.
    ///
    /// # Safety
    ///
    /// This method is task/deferred-context only. Callers must not invoke it
    /// from hard IRQ, NMI, or trap callbacks. The readiness state represented
    /// by `ready` must be published before this method is called, and callers
    /// must not hold locks that may be re-entered by waker execution or poll
    /// wakeup paths.
    pub unsafe fn wake(&self, ready: IoEvents) -> usize {
        self.wake_drain(ready)
    }

    /// Wakes up registered wakers whose interests intersect `ready` from IRQ context.
    ///
    /// Identical to [`wake`](Self::wake) but callable from hard IRQ context: it
    /// shares the allocation-free in-place drain, so device IRQ handlers can
    /// acknowledge the device and then wake matching poll waiters without
    /// allocating.
    pub fn wake_from_irq(&self, ready: IoEvents) -> usize {
        self.wake_drain(ready)
    }

    /// Allocation-free wake: drains ready entries into a stack buffer, compacts
    /// the kept entries in place, and wakes outside the lock.
    ///
    /// This is the hot path for pipe/socket IPC (hackbench-style workloads).
    /// It must not allocate — every heap allocation here would serialize all
    /// CPUs on the global allocator lock, which collapses super-linearly under
    /// many-task messaging. `Entry` is `[Waker (2 ptr) + IoEvents (u32)]`, so
    /// the `POLL_SET_CAPACITY`-wide stack buffer is small and bounded.
    fn wake_drain(&self, ready: IoEvents) -> usize {
        let Some(inner) = self.0.get() else {
            return 0;
        };
        let mut ready_entries = [const { MaybeUninit::<Entry>::uninit() }; POLL_SET_CAPACITY];
        let ready_len = {
            let mut inner = inner.lock();
            let len = inner.len();
            if len == 0 {
                return 0;
            }

            let mut ready_len = 0;
            let mut keep_len = 0;
            for i in 0..len {
                let entry = unsafe { inner.entries[i].assume_init_read() };
                if entry.interests.intersects(ready) {
                    ready_entries[ready_len].write(entry);
                    ready_len += 1;
                } else {
                    inner.entries[keep_len].write(entry);
                    keep_len += 1;
                }
            }
            inner.cursor = keep_len;
            ready_len
        };

        for entry in ready_entries.iter_mut().take(ready_len) {
            unsafe { entry.assume_init_read() }.wake();
        }
        ready_len
    }
}

impl Drop for PollSet {
    fn drop(&mut self) {
        // Ensure all entries are dropped
        unsafe { self.wake(IoEvents::all()) };
    }
}
