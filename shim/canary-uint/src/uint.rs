// Vendored from kerfwork/kwabi-types, src/uint.rs at 3808ea4. Keep in sync by hand.

//! Unsigned 32- and 64-bit integers with checked arithmetic and strict text I/O.
//!
//! The text form is decimal digits only: no sign, no whitespace, no underscores.
//! Leading zeros are accepted and dropped, so "007" reads as 7. Arithmetic never wraps:
//! an overflow or a division by zero is an error, not a silent result.

use std::fmt;
use std::str::FromStr;

/// Why a value could not be read or computed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UIntError {
    /// The input was empty, or held a character that is not an ASCII digit.
    Syntax,
    /// The value does not fit in the target width.
    OutOfRange,
    /// Division or remainder by zero.
    DivideByZero,
}

impl fmt::Display for UIntError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            UIntError::Syntax => write!(f, "invalid unsigned integer syntax"),
            UIntError::OutOfRange => write!(f, "unsigned integer out of range"),
            UIntError::DivideByZero => write!(f, "division by zero"),
        }
    }
}

impl std::error::Error for UIntError {}

macro_rules! uint_type {
    ($name:ident, $prim:ty, $bits:expr) => {
        #[doc = concat!("An unsigned ", stringify!($bits), "-bit integer.")]
        #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
        pub struct $name($prim);

        impl $name {
            pub const MIN: $name = $name(0);
            pub const MAX: $name = $name(<$prim>::MAX);

            pub const fn new(value: $prim) -> Self {
                $name(value)
            }

            pub const fn get(self) -> $prim {
                self.0
            }

            pub fn checked_add(self, rhs: Self) -> Result<Self, UIntError> {
                self.0
                    .checked_add(rhs.0)
                    .map($name)
                    .ok_or(UIntError::OutOfRange)
            }

            pub fn checked_sub(self, rhs: Self) -> Result<Self, UIntError> {
                self.0
                    .checked_sub(rhs.0)
                    .map($name)
                    .ok_or(UIntError::OutOfRange)
            }

            pub fn checked_mul(self, rhs: Self) -> Result<Self, UIntError> {
                self.0
                    .checked_mul(rhs.0)
                    .map($name)
                    .ok_or(UIntError::OutOfRange)
            }

            pub fn checked_div(self, rhs: Self) -> Result<Self, UIntError> {
                self.0
                    .checked_div(rhs.0)
                    .map($name)
                    .ok_or(UIntError::DivideByZero)
            }

            pub fn checked_rem(self, rhs: Self) -> Result<Self, UIntError> {
                self.0
                    .checked_rem(rhs.0)
                    .map($name)
                    .ok_or(UIntError::DivideByZero)
            }
        }

        impl FromStr for $name {
            type Err = UIntError;

            fn from_str(s: &str) -> Result<Self, UIntError> {
                if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(UIntError::Syntax);
                }
                // Digits only, so the only failure left is magnitude.
                s.parse::<$prim>()
                    .map($name)
                    .map_err(|_| UIntError::OutOfRange)
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.0)
            }
        }
    };
}

uint_type!(UInt32, u32, 32);
uint_type!(UInt64, u64, 64);

impl From<UInt32> for UInt64 {
    fn from(v: UInt32) -> Self {
        UInt64::new(u64::from(v.get()))
    }
}

impl TryFrom<UInt64> for UInt32 {
    type Error = UIntError;

    fn try_from(v: UInt64) -> Result<Self, UIntError> {
        u32::try_from(v.get())
            .map(UInt32::new)
            .map_err(|_| UIntError::OutOfRange)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_decimal_and_drops_leading_zeros() {
        assert_eq!("0".parse::<UInt64>(), Ok(UInt64::new(0)));
        assert_eq!("007".parse::<UInt32>(), Ok(UInt32::new(7)));
        assert_eq!("18446744073709551615".parse::<UInt64>(), Ok(UInt64::MAX));
        assert_eq!("4294967295".parse::<UInt32>(), Ok(UInt32::MAX));
    }

    #[test]
    fn rejects_everything_that_is_not_plain_digits() {
        for bad in ["", "-1", "+1", " 1", "1 ", "1_000", "0x10", "1.0", "١"] {
            assert_eq!(
                bad.parse::<UInt64>(),
                Err(UIntError::Syntax),
                "input {bad:?}"
            );
        }
    }

    #[test]
    fn rejects_out_of_range_text() {
        assert_eq!("4294967296".parse::<UInt32>(), Err(UIntError::OutOfRange));
        assert_eq!(
            "18446744073709551616".parse::<UInt64>(),
            Err(UIntError::OutOfRange)
        );
    }

    #[test]
    fn display_round_trips() {
        for v in [0u64, 1, 42, u64::MAX] {
            let s = UInt64::new(v).to_string();
            assert_eq!(s.parse::<UInt64>(), Ok(UInt64::new(v)));
        }
    }

    #[test]
    fn arithmetic_at_the_edges() {
        assert_eq!(
            UInt32::MAX.checked_add(UInt32::new(1)),
            Err(UIntError::OutOfRange)
        );
        assert_eq!(
            UInt32::MIN.checked_sub(UInt32::new(1)),
            Err(UIntError::OutOfRange)
        );
        assert_eq!(
            UInt64::new(1 << 32).checked_mul(UInt64::new(1 << 32)),
            Err(UIntError::OutOfRange)
        );
        assert_eq!(
            UInt64::new(7).checked_div(UInt64::MIN),
            Err(UIntError::DivideByZero)
        );
        assert_eq!(
            UInt64::new(7).checked_rem(UInt64::MIN),
            Err(UIntError::DivideByZero)
        );
        assert_eq!(
            UInt64::new(7).checked_div(UInt64::new(2)),
            Ok(UInt64::new(3))
        );
        assert_eq!(
            UInt64::new(7).checked_rem(UInt64::new(2)),
            Ok(UInt64::new(1))
        );
        assert_eq!(
            UInt32::new(2).checked_mul(UInt32::new(3)),
            Ok(UInt32::new(6))
        );
    }

    #[test]
    fn widening_is_lossless_and_narrowing_is_checked() {
        assert_eq!(UInt64::from(UInt32::MAX), UInt64::new(4_294_967_295));
        assert_eq!(
            UInt32::try_from(UInt64::new(4_294_967_295)),
            Ok(UInt32::MAX)
        );
        assert_eq!(
            UInt32::try_from(UInt64::new(4_294_967_296)),
            Err(UIntError::OutOfRange)
        );
    }
}
