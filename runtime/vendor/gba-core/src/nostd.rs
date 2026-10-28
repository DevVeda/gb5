//! Just enough of `std` for a PS5 title (gba2ps5 patch): no TLS, no env, no stdio.
//! Trace env vars read as unset, prints compile away, OnceLock is a plain lazy cell.
#![allow(unused_imports, unused_macros, dead_code)]
pub use core::{f32, f64, fmt, mem, ptr};
pub mod prelude {
    pub use alloc::{borrow::ToOwned, boxed::Box, format, string::{String, ToString}, vec, vec::Vec};
    pub use super::float::F64Ext;
}
pub mod collections {
    pub use alloc::collections::VecDeque;
}
pub mod env {
    pub fn var_os(_: &str) -> Option<alloc::string::String> { None }
    pub fn var(_: &str) -> Result<alloc::string::String, ()> { Err(()) }
}
pub mod sync {
    // ponytail: the runtime is single-threaded; make this a real Once if a second thread ever touches gba-core
    pub struct OnceLock<T>(core::cell::UnsafeCell<Option<T>>);
    unsafe impl<T> Sync for OnceLock<T> {}
    impl<T> OnceLock<T> {
        pub const fn new() -> Self { Self(core::cell::UnsafeCell::new(None)) }
        pub fn get_or_init(&self, f: impl FnOnce() -> T) -> &T { unsafe { (*self.0.get()).get_or_insert_with(f) } }
    }
}
macro_rules! eprintln { () => {}; ($($t:tt)*) => {{ let _ = format_args!($($t)*); }}; }
macro_rules! println { () => {}; ($($t:tt)*) => {{ let _ = format_args!($($t)*); }}; }
macro_rules! eprint { ($($t:tt)*) => {{ let _ = format_args!($($t)*); }}; }
macro_rules! print { ($($t:tt)*) => {{ let _ = format_args!($($t)*); }}; }

// f64 math isn't in core; route the few calls gba-core makes to libm (PS5 libc has it).
pub mod float {
    extern "C" {
        fn floor(x: f64) -> f64; fn sqrt(x: f64) -> f64; fn sin(x: f64) -> f64; fn cos(x: f64) -> f64;
        fn round(x: f64) -> f64; fn exp2(x: f64) -> f64; fn atan(x: f64) -> f64; fn atan2(y: f64, x: f64) -> f64;
    }
    pub trait F64Ext: Sized {
        fn floor(self) -> f64; fn sqrt(self) -> f64; fn sin_cos(self) -> (f64, f64); fn round(self) -> f64;
        fn exp2(self) -> f64; fn atan(self) -> f64; fn atan2(self, x: f64) -> f64;
    }
    impl F64Ext for f64 {
        fn floor(self) -> f64 { unsafe { floor(self) } }
        fn sqrt(self) -> f64 { unsafe { sqrt(self) } }
        fn sin_cos(self) -> (f64, f64) { unsafe { (sin(self), cos(self)) } }
        fn round(self) -> f64 { unsafe { round(self) } }
        fn exp2(self) -> f64 { unsafe { exp2(self) } }
        fn atan(self) -> f64 { unsafe { atan(self) } }
        fn atan2(self, x: f64) -> f64 { unsafe { atan2(self, x) } }
    }
}
