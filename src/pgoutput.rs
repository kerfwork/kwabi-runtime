//! Transaction ids in pgoutput messages, for changes read through the ABI.
//!
//! pgoutput (proto_version 4, streaming = parallel) writes the xid of the
//! writing (sub)transaction into a change message only while that change is
//! inside a stream. The layout below was captured from a live PostgreSQL 18
//! stream, including a savepoint rolled back mid-stream:
//!
//! - `I`, `U`, `D`, `R` carry the (sub)xid at byte 1, but only inside a stream.
//!   Outside a stream, byte 1 is the relation oid, so the caller must know the
//!   stream state before reading it.
//! - `S` (stream start) carries the top-level xid at byte 1. `E` (stream stop)
//!   carries none.
//! - `A` (stream abort) carries the top-level xid at byte 1 and the aborted
//!   subtransaction's xid at byte 5.
//!
//! Whether a change is inside a stream depends on the `S` and `E` messages
//! before it, so [`StreamState`] tracks that across a batch.

use std::os::raw::c_int;

pub const MSG_INSERT: u8 = b'I';
pub const MSG_UPDATE: u8 = b'U';
pub const MSG_DELETE: u8 = b'D';
pub const MSG_RELATION: u8 = b'R';
pub const MSG_STREAM_START: u8 = b'S';
pub const MSG_STREAM_STOP: u8 = b'E';
pub const MSG_STREAM_ABORT: u8 = b'A';

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The message is shorter than the layout for its type requires.
    Truncated { msg: u8, len: usize },
}

/// Whether the next change is inside a stream, tracked from `S` and `E`.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct StreamState {
    in_stream: bool,
}

impl StreamState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Advance the state past one message. Only `S` and `E` change it.
    pub fn observe(&mut self, msg: &[u8]) {
        match msg.first() {
            Some(&MSG_STREAM_START) => self.in_stream = true,
            Some(&MSG_STREAM_STOP) => self.in_stream = false,
            _ => {}
        }
    }

    pub fn in_stream(&self) -> bool {
        self.in_stream
    }
}

/// The (sub)xid that wrote a change, or `None` when the message carries none.
///
/// `in_stream` is the stream state before this message. Outside a stream the
/// message has no xid, even for `I`, `U`, `D` and `R`.
pub fn writing_xid(msg: &[u8], in_stream: bool) -> Result<Option<u32>, Error> {
    let Some(&kind) = msg.first() else {
        return Err(Error::Truncated { msg: 0, len: 0 });
    };
    match kind {
        MSG_INSERT | MSG_UPDATE | MSG_DELETE | MSG_RELATION if in_stream => {
            read_u32(msg, 1).map(Some)
        }
        _ => Ok(None),
    }
}

/// The `(top_level, subxid)` pair of a stream abort, or `None` for other messages.
///
/// A savepoint rollback inside a stream sends `A` with the subxid it aborted.
/// Every change whose writing xid equals that subxid is discarded.
pub fn abort_xids(msg: &[u8]) -> Result<Option<(u32, u32)>, Error> {
    match msg.first() {
        Some(&MSG_STREAM_ABORT) => {
            let top = read_u32(msg, 1)?;
            let sub = read_u32(msg, 5)?;
            Ok(Some((top, sub)))
        }
        Some(_) => Ok(None),
        None => Err(Error::Truncated { msg: 0, len: 0 }),
    }
}

fn read_u32(msg: &[u8], at: usize) -> Result<u32, Error> {
    match msg.get(at..at + 4) {
        Some(bytes) => Ok(u32::from_be_bytes(bytes.try_into().expect("4 bytes"))),
        None => Err(Error::Truncated {
            msg: msg.first().copied().unwrap_or(0),
            len: msg.len(),
        }),
    }
}

/// C entry point for [`StreamState::observe`]. `in_stream` is read and written.
///
/// # Safety
///
/// `in_stream` must be valid for reads and writes, and `msg` must point to `len`
/// readable bytes, or be null with `len` 0.
#[no_mangle]
pub unsafe extern "C" fn kwabi_pgoutput_observe(in_stream: *mut bool, msg: *const u8, len: usize) {
    if in_stream.is_null() {
        return;
    }
    let mut st = StreamState {
        in_stream: *in_stream,
    };
    st.observe(msg_slice(msg, len));
    *in_stream = st.in_stream;
}

/// C entry point for [`writing_xid`]. Returns 1 and writes `*out` when the
/// message carries an xid, 0 when it carries none, and -1 when it is truncated.
///
/// # Safety
///
/// `msg` must point to `len` readable bytes, and `out` must be valid for a write.
#[no_mangle]
pub unsafe extern "C" fn kwabi_pgoutput_writing_xid(
    msg: *const u8,
    len: usize,
    in_stream: bool,
    out: *mut u32,
) -> c_int {
    if out.is_null() {
        return -1;
    }
    match writing_xid(msg_slice(msg, len), in_stream) {
        Ok(Some(xid)) => {
            *out = xid;
            1
        }
        Ok(None) => 0,
        Err(_) => -1,
    }
}

/// C entry point for [`abort_xids`]. Returns 1 and writes both xids when the
/// message is a stream abort, 0 for other messages, and -1 when it is truncated.
///
/// # Safety
///
/// `msg` must point to `len` readable bytes, and `top` and `sub` must be valid
/// for writes.
#[no_mangle]
pub unsafe extern "C" fn kwabi_pgoutput_abort_xids(
    msg: *const u8,
    len: usize,
    top: *mut u32,
    sub: *mut u32,
) -> c_int {
    if top.is_null() || sub.is_null() {
        return -1;
    }
    match abort_xids(msg_slice(msg, len)) {
        Ok(Some((t, s))) => {
            *top = t;
            *sub = s;
            1
        }
        Ok(None) => 0,
        Err(_) => -1,
    }
}

unsafe fn msg_slice<'a>(msg: *const u8, len: usize) -> &'a [u8] {
    if msg.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(msg, len)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Prefixes of live messages from PostgreSQL 18, captured with
    /// proto_version 4, streaming = parallel. Only the bytes the helpers read
    /// are kept; the rest of each message is cut.
    const STREAM_START_761: &[u8] = &[0x53, 0x00, 0x00, 0x02, 0xf9, 0x01];
    const STREAM_STOP: &[u8] = &[0x45];
    const STREAM_ABORT_761_762: &[u8] = &[
        0x41, 0x00, 0x00, 0x02, 0xf9, 0x00, 0x00, 0x02, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x01, 0xb6,
        0x63, 0x38,
    ];
    const INSERT_STREAMED_762: &[u8] = &[
        0x49, 0x00, 0x00, 0x02, 0xfa, 0x00, 0x00, 0x40, 0x05, 0x4e, 0x00, 0x01,
    ];
    const UPDATE_STREAMED_766: &[u8] = &[
        0x55, 0x00, 0x00, 0x02, 0xfe, 0x00, 0x00, 0x40, 0x05, 0x4f, 0x00, 0x01,
    ];
    const DELETE_STREAMED_767: &[u8] = &[
        0x44, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x40, 0x05, 0x4f, 0x00, 0x01,
    ];
    /// Outside a stream: byte 1 is the relation oid (16389), not an xid.
    const INSERT_UNSTREAMED: &[u8] = &[
        0x49, 0x00, 0x00, 0x40, 0x05, 0x4e, 0x00, 0x01, 0x74, 0x00, 0x00, 0x00, 0x01, 0x37,
    ];

    #[test]
    fn stream_state_follows_s_and_e() {
        let mut st = StreamState::new();
        assert!(!st.in_stream());
        st.observe(STREAM_START_761);
        assert!(st.in_stream());
        st.observe(INSERT_STREAMED_762);
        assert!(st.in_stream());
        st.observe(STREAM_STOP);
        assert!(!st.in_stream());
    }

    #[test]
    fn streamed_changes_carry_their_writing_xid() {
        assert_eq!(writing_xid(INSERT_STREAMED_762, true), Ok(Some(762)));
        assert_eq!(writing_xid(UPDATE_STREAMED_766, true), Ok(Some(766)));
        assert_eq!(writing_xid(DELETE_STREAMED_767, true), Ok(Some(767)));
    }

    #[test]
    fn unstreamed_insert_has_no_xid_even_though_byte_1_is_read() {
        assert_eq!(writing_xid(INSERT_UNSTREAMED, false), Ok(None));
    }

    #[test]
    fn stream_control_messages_carry_no_writing_xid() {
        assert_eq!(writing_xid(STREAM_START_761, true), Ok(None));
        assert_eq!(writing_xid(STREAM_STOP, true), Ok(None));
    }

    #[test]
    fn abort_gives_top_level_and_aborted_subxid() {
        assert_eq!(abort_xids(STREAM_ABORT_761_762), Ok(Some((761, 762))));
        assert_eq!(abort_xids(INSERT_STREAMED_762), Ok(None));
    }

    #[test]
    fn truncated_messages_are_errors_not_panics() {
        assert_eq!(
            writing_xid(&[MSG_INSERT, 0, 0], true),
            Err(Error::Truncated {
                msg: MSG_INSERT,
                len: 3
            })
        );
        assert_eq!(
            abort_xids(&[MSG_STREAM_ABORT, 0, 0, 0, 1, 0, 0]),
            Err(Error::Truncated {
                msg: MSG_STREAM_ABORT,
                len: 7
            })
        );
        assert_eq!(
            writing_xid(&[], true),
            Err(Error::Truncated { msg: 0, len: 0 })
        );
    }

    #[test]
    fn subxid_sweep_matches_a_rolled_back_savepoint() {
        let stream = [
            STREAM_START_761,
            INSERT_STREAMED_762,
            UPDATE_STREAMED_766,
            DELETE_STREAMED_767,
            STREAM_ABORT_761_762,
            STREAM_STOP,
        ];
        let mut st = StreamState::new();
        let mut aborted = None;
        let mut writers = Vec::new();
        for msg in stream {
            if let Some((_, sub)) = abort_xids(msg).unwrap() {
                aborted = Some(sub);
            }
            if let Some(xid) = writing_xid(msg, st.in_stream()).unwrap() {
                writers.push(xid);
            }
            st.observe(msg);
        }
        assert_eq!(aborted, Some(762));
        assert_eq!(writers, vec![762, 766, 767]);
    }
}
