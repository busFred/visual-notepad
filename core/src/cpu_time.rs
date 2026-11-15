//! A clock for the tests that guard against code that blows up: the time the
//! calling thread has spent running, not the time that has gone by. Where
//! other programs keep the machine busy, a test thread can wait for a core
//! for most of a minute, and the wall clock then says a layout took twenty
//! times as long as it needs; the processor time is what the code itself
//! costs, and one that blows up still costs minutes of it.
//!
//! Like `std::time::Instant`, but only meaningful on the thread that made it
//! (a test that does its work on another thread starts the clock there).

use std::time::Duration;

#[derive(Clone, Copy)]
pub struct Instant(Duration);

impl Instant {
    pub fn now() -> Self {
        Self(thread_cpu_time())
    }

    pub fn elapsed(&self) -> Duration {
        thread_cpu_time().saturating_sub(self.0)
    }
}

#[cfg(unix)]
fn thread_cpu_time() -> Duration {
    use std::os::raw::{c_int, c_long};

    #[repr(C)]
    struct Timespec {
        seconds: c_long,
        nanoseconds: c_long,
    }
    unsafe extern "C" {
        fn clock_gettime(clock: c_int, to: *mut Timespec) -> c_int;
    }
    // CLOCK_THREAD_CPUTIME_ID
    #[cfg(any(target_os = "macos", target_os = "ios"))]
    const THREAD_CPU_CLOCK: c_int = 16;
    #[cfg(target_os = "freebsd")]
    const THREAD_CPU_CLOCK: c_int = 14;
    #[cfg(not(any(target_os = "macos", target_os = "ios", target_os = "freebsd")))]
    const THREAD_CPU_CLOCK: c_int = 3;

    let mut now = Timespec { seconds: 0, nanoseconds: 0 };
    // SAFETY: `now` is a valid timespec for the call to fill in.
    if unsafe { clock_gettime(THREAD_CPU_CLOCK, &mut now) } != 0 {
        return Duration::ZERO;
    }
    Duration::new(now.seconds as u64, now.nanoseconds as u32)
}

#[cfg(windows)]
fn thread_cpu_time() -> Duration {
    use std::ffi::c_void;

    #[repr(C)]
    #[derive(Default)]
    struct FileTime {
        low: u32,
        high: u32,
    }
    unsafe extern "system" {
        fn GetCurrentThread() -> *mut c_void;
        fn GetThreadTimes(
            thread: *mut c_void,
            created: *mut FileTime,
            exited: *mut FileTime,
            kernel: *mut FileTime,
            user: *mut FileTime,
        ) -> i32;
    }
    let (mut created, mut exited, mut kernel, mut user) = Default::default();
    // SAFETY: the pseudo-handle of the calling thread, and four FILETIMEs to
    // fill in.
    if unsafe { GetThreadTimes(GetCurrentThread(), &mut created, &mut exited, &mut kernel, &mut user) } == 0 {
        return Duration::ZERO;
    }
    // In units of 100 ns (and counted per scheduler tick, 15.6 ms).
    let ticks = |t: &FileTime| (u64::from(t.high) << 32) | u64::from(t.low);
    Duration::from_nanos((ticks(&kernel) + ticks(&user)) * 100)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_work_and_not_waiting() {
        let start = Instant::now();
        std::thread::sleep(Duration::from_millis(300));
        assert!(start.elapsed() < Duration::from_millis(100), "{:?}", start.elapsed());
        let mut sum = 0u64;
        while start.elapsed() < Duration::from_millis(100) {
            sum = std::hint::black_box(sum.wrapping_add(1));
        }
        assert!(sum > 0);
    }
}
