// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cell::RefCell,
    rc::{Rc, Weak},
    time::Duration,
};

#[cfg(windows)]
use windows::Win32::Media::{timeBeginPeriod, timeEndPeriod};

/// A quantized `Duration`.  This currently just produces 16 discrete values
/// corresponding to whole milliseconds.  Future implementations might choose
/// a different allocation, such as a logarithmic scale.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct Period(u8);

impl Period {
    const MAX: Self = Self(16);
    const MIN: Self = Self(1);

    #[cfg(windows)]
    fn as_u32(self) -> u32 {
        u32::from(self.0)
    }

    #[cfg(target_os = "macos")]
    fn scaled(self, scale: f64) -> f64 {
        scale * f64::from(self.0)
    }
}

impl From<Duration> for Period {
    fn from(p: Duration) -> Self {
        let rounded = u8::try_from(p.as_millis()).unwrap_or(Self::MAX.0);
        Self(rounded.clamp(Self::MIN.0, Self::MAX.0))
    }
}

/// This counts instances of `Period`, except those of `Period::MAX`.
#[derive(Default)]
struct PeriodSet {
    counts: [usize; (Period::MAX.0 - Period::MIN.0) as usize],
}

impl PeriodSet {
    fn idx(&mut self, p: Period) -> &mut usize {
        debug_assert!(p >= Period::MIN);
        &mut self.counts[usize::from(p.0 - Period::MIN.0)]
    }

    fn add(&mut self, p: Period) {
        if p != Period::MAX {
            *self.idx(p) += 1;
        }
    }

    fn remove(&mut self, p: Period) {
        if p != Period::MAX {
            let p = self.idx(p);
            debug_assert_ne!(*p, 0);
            *p -= 1;
        }
    }

    fn min(&self) -> Option<Period> {
        for (i, v) in self.counts.iter().enumerate() {
            if *v > 0 {
                return Some(Period(u8::try_from(i).ok()? + Period::MIN.0));
            }
        }
        None
    }
}

#[cfg(target_os = "macos")]
#[expect(non_camel_case_types, reason = "These are C types.")]
mod mac {
    use std::ptr::addr_of_mut;

    // These are manually extracted from the many bindings generated
    // by bindgen when provided with the simple header:
    // #include <mach/mach_init.h>
    // #include <mach/mach_time.h>
    // #include <mach/thread_policy.h>
    // #include <pthread.h>

    type __darwin_natural_t = ::std::os::raw::c_uint;
    type __darwin_mach_port_name_t = __darwin_natural_t;
    type __darwin_mach_port_t = __darwin_mach_port_name_t;
    type mach_port_t = __darwin_mach_port_t;
    type thread_t = mach_port_t;
    type natural_t = __darwin_natural_t;
    type thread_policy_flavor_t = natural_t;
    type integer_t = ::std::os::raw::c_int;
    type thread_policy_t = *mut integer_t;
    type mach_msg_type_number_t = natural_t;
    type boolean_t = ::std::os::raw::c_uint;
    type kern_return_t = ::std::os::raw::c_int;

    #[repr(C)]
    #[derive(Debug, Copy, Clone, Default)]
    struct mach_timebase_info {
        numer: u32,
        denom: u32,
    }
    type mach_timebase_info_t = *mut mach_timebase_info;
    type mach_timebase_info_data_t = mach_timebase_info;
    extern "C" {
        fn mach_timebase_info(info: mach_timebase_info_t) -> kern_return_t;
    }

    #[repr(C)]
    #[derive(Debug, Copy, Clone, Default)]
    pub struct thread_time_constraint_policy {
        period: u32,
        computation: u32,
        constraint: u32,
        preemptible: boolean_t,
    }

    const THREAD_TIME_CONSTRAINT_POLICY: thread_policy_flavor_t = 2;
    #[expect(clippy::cast_possible_truncation, reason = "These are C types.")]
    const THREAD_TIME_CONSTRAINT_POLICY_COUNT: mach_msg_type_number_t =
        (size_of::<thread_time_constraint_policy>() / size_of::<integer_t>())
            as mach_msg_type_number_t;

    // These function definitions are taken from a comment in <thread_policy.h>.
    // Why they are inaccessible is unknown, but they work as declared.
    extern "C" {
        fn thread_policy_set(
            thread: thread_t,
            flavor: thread_policy_flavor_t,
            policy_info: thread_policy_t,
            count: mach_msg_type_number_t,
        ) -> kern_return_t;
        fn thread_policy_get(
            thread: thread_t,
            flavor: thread_policy_flavor_t,
            policy_info: thread_policy_t,
            count: *mut mach_msg_type_number_t,
            get_default: *mut boolean_t,
        ) -> kern_return_t;
    }

    enum _opaque_pthread_t {} // An opaque type is fine here.
    type __darwin_pthread_t = *mut _opaque_pthread_t;
    type pthread_t = __darwin_pthread_t;

    extern "C" {
        fn pthread_self() -> pthread_t;
        fn pthread_mach_thread_np(thread: pthread_t) -> mach_port_t;
    }

    /// Set a thread time policy.
    pub fn set_thread_policy(mut policy: thread_time_constraint_policy) {
        _ = unsafe {
            thread_policy_set(
                pthread_mach_thread_np(pthread_self()),
                THREAD_TIME_CONSTRAINT_POLICY,
                addr_of_mut!(policy).cast(), // horror!
                THREAD_TIME_CONSTRAINT_POLICY_COUNT,
            )
        };
    }

    pub fn get_scale() -> f64 {
        const NANOS_PER_MSEC: f64 = 1_000_000.0;
        let mut timebase_info = mach_timebase_info_data_t::default();
        unsafe {
            mach_timebase_info(&mut timebase_info);
        }
        f64::from(timebase_info.denom) * NANOS_PER_MSEC / f64::from(timebase_info.numer)
    }

    /// Create a realtime policy and set it.
    pub fn set_realtime(base: f64) {
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "These are C types."
        )]
        let policy = thread_time_constraint_policy {
            period: base as u32, // Base interval
            computation: (base * 0.5) as u32,
            constraint: (base * 1.0) as u32,
            preemptible: 1,
        };
        set_thread_policy(policy);
    }

    /// Get the default policy.
    pub fn get_default_policy() -> thread_time_constraint_policy {
        let mut policy = thread_time_constraint_policy::default();
        let mut count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        let mut get_default = 0;
        _ = unsafe {
            thread_policy_get(
                pthread_mach_thread_np(pthread_self()),
                THREAD_TIME_CONSTRAINT_POLICY,
                addr_of_mut!(policy).cast(), // horror!
                &mut count,
                &mut get_default,
            )
        };
        policy
    }
}

/// A handle for a high-resolution timer of a specific period.
pub struct Handle {
    hrt: Rc<RefCell<Time>>,
    active: Period,
    hysteresis: [Period; Self::HISTORY],
    hysteresis_index: usize,
}

impl Handle {
    const HISTORY: usize = 8;

    const fn new(hrt: Rc<RefCell<Time>>, active: Period) -> Self {
        Self {
            hrt,
            active,
            hysteresis: [Period::MAX; Self::HISTORY],
            hysteresis_index: 0,
        }
    }

    /// Update shortcut.  Equivalent to dropping the current reference and
    /// calling `HrTime::get` again with the new period, except that this applies
    /// a little hysteresis that smoothes out fluctuations.
    pub fn update(&mut self, period: Duration) {
        self.hysteresis[self.hysteresis_index] = Period::from(period);
        self.hysteresis_index += 1;
        self.hysteresis_index %= self.hysteresis.len();

        let mut first = Period::MAX;
        let mut second = Period::MAX;
        for i in &self.hysteresis {
            if *i < first {
                second = first;
                first = *i;
            } else if *i < second {
                second = *i;
            }
        }

        if second != self.active {
            let mut b = self.hrt.borrow_mut();
            b.periods.remove(self.active);
            self.active = second;
            b.periods.add(self.active);
            b.update();
        }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.hrt.borrow_mut().remove(self.active);
    }
}

/// Holding an instance of this indicates that high resolution timers are enabled.
pub struct Time {
    periods: PeriodSet,
    active: Option<Period>,

    #[cfg(target_os = "macos")]
    scale: f64,
    #[cfg(target_os = "macos")]
    deflt: mac::thread_time_constraint_policy,
}
impl Time {
    fn new() -> Self {
        Self {
            periods: PeriodSet::default(),
            active: None,

            #[cfg(target_os = "macos")]
            scale: mac::get_scale(),
            #[cfg(target_os = "macos")]
            deflt: mac::get_default_policy(),
        }
    }

    #[cfg(target_os = "macos")]
    fn start(&self) {
        if let Some(p) = self.active {
            mac::set_realtime(p.scaled(self.scale));
        } else {
            mac::set_thread_policy(self.deflt);
        }
    }

    #[cfg(target_os = "windows")]
    fn start(&self) {
        if let Some(p) = self.active {
            _ = unsafe { timeBeginPeriod(p.as_u32()) };
        }
    }

    #[cfg(all(not(target_os = "macos"), not(target_os = "windows")))]
    #[expect(
        clippy::unused_self,
        reason = "Not used on platforms other than macOS and Windows."
    )]
    const fn start(&self) {}

    #[cfg(windows)]
    fn stop(&self) {
        if let Some(p) = self.active {
            _ = unsafe { timeEndPeriod(p.as_u32()) };
        }
    }

    #[cfg(not(target_os = "windows"))]
    #[expect(
        clippy::unused_self,
        reason = "Not used on platforms other than Windows."
    )]
    const fn stop(&self) {}

    fn update(&mut self) {
        let next = self.periods.min();
        if next != self.active {
            self.stop();
            self.active = next;
            self.start();
        }
    }

    fn add(&mut self, p: Period) {
        self.periods.add(p);
        self.update();
    }

    fn remove(&mut self, p: Period) {
        self.periods.remove(p);
        self.update();
    }

    /// Enable high resolution time.  Returns a thread-bound handle that
    /// needs to be held until the high resolution time is no longer needed.
    /// The handle can also be used to update the resolution.
    #[must_use]
    pub fn get(period: Duration) -> Handle {
        thread_local!(static HR_TIME: RefCell<Weak<RefCell<Time>>> = RefCell::default());

        HR_TIME.with(|r| {
            let mut b = r.borrow_mut();
            let hrt = b.upgrade().unwrap_or_else(|| {
                let hrt = Rc::new(RefCell::new(Self::new()));
                *b = Rc::downgrade(&hrt);
                hrt
            });

            let p = Period::from(period);
            hrt.borrow_mut().add(p);
            Handle::new(hrt, p)
        })
    }
}

impl Drop for Time {
    fn drop(&mut self) {
        self.stop();

        #[cfg(target_os = "macos")]
        {
            if self.active.is_some() {
                mac::set_thread_policy(self.deflt);
            }
        }
    }
}

// Only run these tests in CI on Linux, where the timer accuracies are OK enough to pass the tests,
// but only when not running sanitizers.
#[cfg(all(target_os = "linux", not(neqo_sanitize)))]
#[cfg(test)]
mod test {
    use std::{
        thread::{sleep, spawn},
        time::{Duration, Instant},
    };

    use super::Time;

    #[cfg(not(target_arch = "aarch64"))]
    const ONE_MS: Duration = Duration::from_millis(1);
    const FIVE_MS: Duration = Duration::from_millis(5);
    #[cfg(not(target_arch = "aarch64"))]
    const ONE_MS_AND_A_BIT: Duration = Duration::from_micros(1500);
    /// A limit for when high resolution timers are disabled.
    const GENEROUS: Duration = Duration::from_millis(30);

    fn validate_delays(max_lag: Duration) -> Result<(), ()> {
        const DELAYS: &[u64] = &[1, 2, 3, 5, 8, 10, 12, 15, 20, 25, 30];
        let durations = DELAYS.iter().map(|&d| Duration::from_millis(d));

        let mut s = Instant::now();
        for d in durations {
            sleep(d);
            let e = Instant::now();
            let actual = e.saturating_duration_since(s);
            let lag = actual.saturating_sub(d);
            println!("sleep({d:>4?}) \u{2192} {actual:>11.6?} \u{394}{lag:>10?}");
            if lag > max_lag {
                return Err(());
            }
            s = Instant::now();
        }
        Ok(())
    }

    /// Validate the delays multiple times.  Sometimes a run can stall.
    /// Reliability in CI is more important than reliable timers.
    /// Any failure results in enqueing two additional checks,
    /// up to a limit that is determined based on how small `max_lag` is.
    /// If the count exceeds that limit, fail the test.
    fn check_delays(max_lag: Duration) {
        let max_loops = if max_lag < FIVE_MS {
            5
        } else if max_lag < GENEROUS {
            3
        } else {
            1
        };

        let mut count = 1;
        while count <= max_loops {
            if validate_delays(max_lag).is_ok() {
                count -= 1;
            } else {
                count += 1;
            }
            if count == 0 {
                return;
            }
            sleep(Duration::from_millis(50));
        }
        panic!("timers slipped too often");
    }

    /// Note that you have to run this test alone or other tests will
    /// grab the high resolution timer and this will run faster.
    #[test]
    fn baseline() {
        check_delays(GENEROUS);
    }

    #[cfg(not(target_arch = "aarch64"))] // This test is flaky on linux/arm.
    #[test]
    fn one_ms() {
        let _hrt = Time::get(ONE_MS);
        check_delays(ONE_MS_AND_A_BIT);
    }

    #[test]
    fn multithread_baseline() {
        let thr = spawn(move || {
            baseline();
        });
        baseline();
        thr.join().unwrap();
    }

    #[cfg(not(target_arch = "aarch64"))] // This test is flaky on linux/arm.
    #[test]
    fn one_ms_multi() {
        let thr = spawn(move || {
            one_ms();
        });
        one_ms();
        thr.join().unwrap();
    }

    #[cfg(not(target_arch = "aarch64"))] // This test is flaky on linux/arm.
    #[test]
    fn mixed_multi() {
        let thr = spawn(move || {
            one_ms();
        });
        let _hrt = Time::get(Duration::from_millis(4));
        check_delays(FIVE_MS);
        thr.join().unwrap();
    }

    #[cfg(not(target_arch = "aarch64"))] // This test is flaky on linux/arm.
    #[test]
    fn update() {
        let mut hrt = Time::get(Duration::from_millis(4));
        check_delays(FIVE_MS);
        hrt.update(ONE_MS);
        check_delays(ONE_MS_AND_A_BIT);
    }

    #[cfg(not(target_arch = "aarch64"))] // This test is flaky on linux/arm.
    #[test]
    fn update_multi() {
        let thr = spawn(move || {
            update();
        });
        update();
        thr.join().unwrap();
    }

    #[test]
    fn max() {
        let _hrt = Time::get(Duration::from_secs(1));
        check_delays(GENEROUS);
    }

    #[test]
    #[should_panic(expected = "timers slipped too often")]
    fn slip() {
        // This amount of timer resolution should be unachievable.
        check_delays(Duration::from_nanos(1));
    }
}
