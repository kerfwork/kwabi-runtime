// Copyright (C) 2026 Simon Janes
// SPDX-License-Identifier: AGPL-3.0-or-later

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
    /// The first byte is not a pgoutput message type.
    UnknownTag(u8),
    /// A field that must hold a particular tag byte held another.
    BadTag { msg: u8, found: u8 },
    /// A tuple column kind other than null, unchanged, text, or binary.
    BadColumnKind { msg: u8, found: u8 },
    /// A name or GID is not valid UTF-8.
    BadText { msg: u8 },
    /// Bytes remain after the message's last field.
    Trailing { msg: u8, extra: usize },
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

/// One column of a tuple, as pgoutput sends it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Column {
    Null,
    /// The column is a TOASTed value that did not change; the bytes are not sent.
    Unchanged,
    Text(Vec<u8>),
    Binary(Vec<u8>),
}

/// Which old tuple an update or delete carries: the key columns (`K`) or the
/// whole old row (`O`, when the table's replica identity is FULL).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OldKind {
    Key,
    Full,
}

/// One column of a relation message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelationColumn {
    pub flags: u8,
    pub name: String,
    pub type_oid: u32,
    pub type_mod: i32,
}

/// One decoded pgoutput message. Field names follow the protocol documentation.
/// `xid` is present on a change only when it arrived inside a stream.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Begin {
        final_lsn: u64,
        commit_time: i64,
        xid: u32,
    },
    Commit {
        flags: u8,
        commit_lsn: u64,
        end_lsn: u64,
        commit_time: i64,
    },
    Relation {
        xid: Option<u32>,
        relid: u32,
        namespace: String,
        name: String,
        replica_identity: u8,
        columns: Vec<RelationColumn>,
    },
    Insert {
        xid: Option<u32>,
        relid: u32,
        new: Vec<Column>,
    },
    Update {
        xid: Option<u32>,
        relid: u32,
        old: Option<(OldKind, Vec<Column>)>,
        new: Vec<Column>,
    },
    Delete {
        xid: Option<u32>,
        relid: u32,
        old: (OldKind, Vec<Column>),
    },
    Truncate {
        xid: Option<u32>,
        options: u8,
        relids: Vec<u32>,
    },
    StreamStart {
        xid: u32,
        first_segment: bool,
    },
    StreamStop,
    StreamCommit {
        xid: u32,
        flags: u8,
        commit_lsn: u64,
        end_lsn: u64,
        commit_time: i64,
    },
    /// Carries the LSN and time fields only from proto_version 4 on.
    StreamAbort {
        xid: u32,
        subxid: u32,
        abort_lsn: Option<u64>,
        abort_time: Option<i64>,
    },
    BeginPrepare {
        prepare_lsn: u64,
        end_lsn: u64,
        prepare_time: i64,
        xid: u32,
        gid: String,
    },
    Prepare {
        flags: u8,
        prepare_lsn: u64,
        end_lsn: u64,
        prepare_time: i64,
        xid: u32,
        gid: String,
    },
    CommitPrepared {
        flags: u8,
        commit_lsn: u64,
        end_lsn: u64,
        commit_time: i64,
        xid: u32,
        gid: String,
    },
    RollbackPrepared {
        flags: u8,
        prepare_end_lsn: u64,
        rollback_end_lsn: u64,
        prepare_time: i64,
        rollback_time: i64,
        xid: u32,
        gid: String,
    },
    StreamPrepare {
        flags: u8,
        prepare_lsn: u64,
        end_lsn: u64,
        prepare_time: i64,
        xid: u32,
        gid: String,
    },
}

/// Parse one pgoutput message. `in_stream` is the stream state before it, and
/// `proto_version` is the one the slot was opened with. Every byte must be used:
/// trailing bytes are an error, as is any truncation.
pub fn parse(msg: &[u8], in_stream: bool, proto_version: u8) -> Result<Message, Error> {
    let mut r = Reader::new(msg)?;
    let parsed = match r.tag {
        b'B' => Message::Begin {
            final_lsn: r.u64()?,
            commit_time: r.i64()?,
            xid: r.u32()?,
        },
        b'C' => Message::Commit {
            flags: r.u8()?,
            commit_lsn: r.u64()?,
            end_lsn: r.u64()?,
            commit_time: r.i64()?,
        },
        b'R' => {
            let xid = r.maybe_xid(in_stream)?;
            let relid = r.u32()?;
            let namespace = r.cstr()?;
            let name = r.cstr()?;
            let replica_identity = r.u8()?;
            let natts = r.u16()?;
            let mut columns = Vec::with_capacity(natts as usize);
            for _ in 0..natts {
                columns.push(RelationColumn {
                    flags: r.u8()?,
                    name: r.cstr()?,
                    type_oid: r.u32()?,
                    type_mod: r.i32()?,
                });
            }
            Message::Relation {
                xid,
                relid,
                namespace,
                name,
                replica_identity,
                columns,
            }
        }
        b'I' => {
            let xid = r.maybe_xid(in_stream)?;
            let relid = r.u32()?;
            r.expect_tag(b'N')?;
            Message::Insert {
                xid,
                relid,
                new: r.tuple()?,
            }
        }
        b'U' => {
            let xid = r.maybe_xid(in_stream)?;
            let relid = r.u32()?;
            let mut old = None;
            let mut next = r.u8()?;
            if next == b'K' || next == b'O' {
                let kind = if next == b'K' {
                    OldKind::Key
                } else {
                    OldKind::Full
                };
                old = Some((kind, r.tuple()?));
                next = r.u8()?;
            }
            if next != b'N' {
                return Err(Error::BadTag {
                    msg: b'U',
                    found: next,
                });
            }
            Message::Update {
                xid,
                relid,
                old,
                new: r.tuple()?,
            }
        }
        b'D' => {
            let xid = r.maybe_xid(in_stream)?;
            let relid = r.u32()?;
            let kind = match r.u8()? {
                b'K' => OldKind::Key,
                b'O' => OldKind::Full,
                found => return Err(Error::BadTag { msg: b'D', found }),
            };
            Message::Delete {
                xid,
                relid,
                old: (kind, r.tuple()?),
            }
        }
        b'T' => {
            let xid = r.maybe_xid(in_stream)?;
            let nrels = r.u32()?;
            let options = r.u8()?;
            let mut relids = Vec::with_capacity(nrels as usize);
            for _ in 0..nrels {
                relids.push(r.u32()?);
            }
            Message::Truncate {
                xid,
                options,
                relids,
            }
        }
        b'S' => Message::StreamStart {
            xid: r.u32()?,
            first_segment: r.u8()? != 0,
        },
        b'E' => Message::StreamStop,
        b'c' => Message::StreamCommit {
            xid: r.u32()?,
            flags: r.u8()?,
            commit_lsn: r.u64()?,
            end_lsn: r.u64()?,
            commit_time: r.i64()?,
        },
        b'A' => {
            let xid = r.u32()?;
            let subxid = r.u32()?;
            let (abort_lsn, abort_time) = if proto_version >= 4 {
                (Some(r.u64()?), Some(r.i64()?))
            } else {
                (None, None)
            };
            Message::StreamAbort {
                xid,
                subxid,
                abort_lsn,
                abort_time,
            }
        }
        b'b' => Message::BeginPrepare {
            prepare_lsn: r.u64()?,
            end_lsn: r.u64()?,
            prepare_time: r.i64()?,
            xid: r.u32()?,
            gid: r.cstr()?,
        },
        b'P' | b'p' => {
            let flags = r.u8()?;
            let prepare_lsn = r.u64()?;
            let end_lsn = r.u64()?;
            let prepare_time = r.i64()?;
            let xid = r.u32()?;
            let gid = r.cstr()?;
            if r.tag == b'P' {
                Message::Prepare {
                    flags,
                    prepare_lsn,
                    end_lsn,
                    prepare_time,
                    xid,
                    gid,
                }
            } else {
                Message::StreamPrepare {
                    flags,
                    prepare_lsn,
                    end_lsn,
                    prepare_time,
                    xid,
                    gid,
                }
            }
        }
        b'K' => Message::CommitPrepared {
            flags: r.u8()?,
            commit_lsn: r.u64()?,
            end_lsn: r.u64()?,
            commit_time: r.i64()?,
            xid: r.u32()?,
            gid: r.cstr()?,
        },
        b'r' => Message::RollbackPrepared {
            flags: r.u8()?,
            prepare_end_lsn: r.u64()?,
            rollback_end_lsn: r.u64()?,
            prepare_time: r.i64()?,
            rollback_time: r.i64()?,
            xid: r.u32()?,
            gid: r.cstr()?,
        },
        found => return Err(Error::UnknownTag(found)),
    };
    r.finish()?;
    Ok(parsed)
}

struct Reader<'a> {
    tag: u8,
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(msg: &'a [u8]) -> Result<Self, Error> {
        match msg.first() {
            Some(&tag) => Ok(Reader {
                tag,
                buf: msg,
                pos: 1,
            }),
            None => Err(Error::Truncated { msg: 0, len: 0 }),
        }
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], Error> {
        match self.buf.get(self.pos..self.pos + n) {
            Some(bytes) => {
                self.pos += n;
                Ok(bytes)
            }
            None => Err(Error::Truncated {
                msg: self.tag,
                len: self.buf.len(),
            }),
        }
    }

    fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }

    fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_be_bytes(
            self.take(2)?.try_into().expect("2 bytes"),
        ))
    }

    fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_be_bytes(
            self.take(4)?.try_into().expect("4 bytes"),
        ))
    }

    fn i32(&mut self) -> Result<i32, Error> {
        Ok(self.u32()? as i32)
    }

    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_be_bytes(
            self.take(8)?.try_into().expect("8 bytes"),
        ))
    }

    fn i64(&mut self) -> Result<i64, Error> {
        Ok(self.u64()? as i64)
    }

    fn maybe_xid(&mut self, in_stream: bool) -> Result<Option<u32>, Error> {
        if in_stream {
            Ok(Some(self.u32()?))
        } else {
            Ok(None)
        }
    }

    fn expect_tag(&mut self, want: u8) -> Result<(), Error> {
        let found = self.u8()?;
        if found == want {
            Ok(())
        } else {
            Err(Error::BadTag {
                msg: self.tag,
                found,
            })
        }
    }

    fn cstr(&mut self) -> Result<String, Error> {
        let rest = &self.buf[self.pos..];
        let Some(nul) = rest.iter().position(|&b| b == 0) else {
            return Err(Error::Truncated {
                msg: self.tag,
                len: self.buf.len(),
            });
        };
        let text =
            std::str::from_utf8(&rest[..nul]).map_err(|_| Error::BadText { msg: self.tag })?;
        let owned = text.to_owned();
        self.pos += nul + 1;
        Ok(owned)
    }

    fn tuple(&mut self) -> Result<Vec<Column>, Error> {
        let natts = self.u16()?;
        let mut cols = Vec::with_capacity(natts as usize);
        for _ in 0..natts {
            let kind = self.u8()?;
            cols.push(match kind {
                b'n' => Column::Null,
                b'u' => Column::Unchanged,
                b't' => {
                    let len = self.u32()? as usize;
                    Column::Text(self.take(len)?.to_vec())
                }
                b'b' => {
                    let len = self.u32()? as usize;
                    Column::Binary(self.take(len)?.to_vec())
                }
                found => {
                    return Err(Error::BadColumnKind {
                        msg: self.tag,
                        found,
                    })
                }
            });
        }
        Ok(cols)
    }

    fn finish(&self) -> Result<(), Error> {
        if self.pos == self.buf.len() {
            Ok(())
        } else {
            Err(Error::Trailing {
                msg: self.tag,
                extra: self.buf.len() - self.pos,
            })
        }
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

#[cfg(test)]
mod parse_tests {
    use super::*;
    use std::collections::BTreeSet;

    const FIXTURES: &str = include_str!("../tests/fixtures/pgoutput-pg18.txt");

    /// One captured message: its section, its tag, and its bytes.
    struct Sample {
        section: &'static str,
        tag: u8,
        bytes: Vec<u8>,
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex"))
            .collect()
    }

    fn samples() -> Vec<Sample> {
        let mut out = Vec::new();
        let mut section: &'static str = "";
        for line in FIXTURES.lines() {
            if line.starts_with('#') || line.is_empty() {
                continue;
            }
            match line {
                "[plain]" => section = "plain",
                "[stream]" => section = "stream",
                _ => {
                    let (tag, bytes) = line.split_once(' ').expect("tag and hex");
                    out.push(Sample {
                        section,
                        tag: tag.as_bytes()[0],
                        bytes: hex(bytes),
                    });
                }
            }
        }
        out
    }

    /// The protocol version each section was captured with.
    fn proto_of(section: &str) -> u8 {
        if section == "stream" {
            4
        } else {
            3
        }
    }

    /// Whether a sample was inside a stream when captured. The samples are one per
    /// tag, not a sequence, so this comes from the section and the tag: only the
    /// stream section holds streamed changes, and only data messages carry an xid
    /// there. Stream state across a sequence is tested in the unit tests above.
    fn in_stream_of(section: &str, tag: u8) -> bool {
        section == "stream" && matches!(tag, b'I' | b'U' | b'D' | b'R' | b'T')
    }

    fn parse_section(section: &str) -> Vec<Message> {
        samples()
            .into_iter()
            .filter(|s| s.section == section)
            .map(|s| {
                parse(&s.bytes, in_stream_of(section, s.tag), proto_of(section))
                    .expect("captured message parses")
            })
            .collect()
    }

    fn parse_one(tag: u8, section: &str) -> Message {
        let s = samples()
            .into_iter()
            .find(|s| s.tag == tag && s.section == section)
            .expect("sample present");
        parse(&s.bytes, in_stream_of(section, tag), proto_of(section)).expect("parses")
    }

    fn txt(s: &str) -> Column {
        Column::Text(s.as_bytes().to_vec())
    }

    #[test]
    fn every_message_type_is_captured() {
        let tags: BTreeSet<u8> = samples().iter().map(|s| s.tag).collect();
        let want: BTreeSet<u8> = b"BCRIUDTSEcAbPKrp".iter().copied().collect();
        assert_eq!(tags, want);
    }

    #[test]
    fn every_captured_message_parses_and_uses_every_byte() {
        for section in ["plain", "stream"] {
            let expected = samples().iter().filter(|s| s.section == section).count();
            assert_eq!(parse_section(section).len(), expected);
        }
    }

    #[test]
    fn plain_changes_carry_their_columns() {
        assert_eq!(
            parse_one(b'I', "plain"),
            Message::Insert {
                xid: None,
                relid: 16384,
                new: vec![txt("1"), txt("a")],
            }
        );
        assert_eq!(
            parse_one(b'U', "plain"),
            Message::Update {
                xid: None,
                relid: 16384,
                old: Some((OldKind::Full, vec![txt("1"), txt("a")])),
                new: vec![txt("1"), txt("b")],
            }
        );
        assert_eq!(
            parse_one(b'D', "plain"),
            Message::Delete {
                xid: None,
                relid: 16384,
                old: (OldKind::Full, vec![txt("1"), txt("b")]),
            }
        );
        assert_eq!(
            parse_one(b'T', "plain"),
            Message::Truncate {
                xid: None,
                options: 0,
                relids: vec![16384],
            }
        );
    }

    #[test]
    fn relation_names_and_columns() {
        let Message::Relation {
            namespace,
            name,
            columns,
            ..
        } = parse_one(b'R', "plain")
        else {
            panic!("expected a relation message");
        };
        assert_eq!(namespace, "public");
        assert_eq!(name, "t");
        let names: Vec<&str> = columns.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, vec!["id", "s"]);
    }

    #[test]
    fn transaction_boundaries_and_prepared_transactions() {
        assert!(matches!(
            parse_one(b'B', "plain"),
            Message::Begin { xid: 755, .. }
        ));
        assert!(matches!(
            parse_one(b'C', "plain"),
            Message::Commit { flags: 0, .. }
        ));
        let Message::BeginPrepare { xid, gid, .. } = parse_one(b'b', "plain") else {
            panic!("expected begin prepare");
        };
        assert_eq!((xid, gid.as_str()), (756, "gid_commit"));
        let Message::Prepare { xid, gid, .. } = parse_one(b'P', "plain") else {
            panic!("expected prepare");
        };
        assert_eq!((xid, gid.as_str()), (756, "gid_commit"));
        let Message::CommitPrepared { gid, .. } = parse_one(b'K', "plain") else {
            panic!("expected commit prepared");
        };
        assert_eq!(gid, "gid_commit");
        let Message::RollbackPrepared { gid, .. } = parse_one(b'r', "plain") else {
            panic!("expected rollback prepared");
        };
        assert_eq!(gid, "gid_rollback");
    }

    #[test]
    fn streamed_messages_carry_xids_and_abort_lsns() {
        assert_eq!(
            parse_one(b'S', "stream"),
            Message::StreamStart {
                xid: 0x2f6,
                first_segment: true,
            }
        );
        assert_eq!(parse_one(b'E', "stream"), Message::StreamStop);
        let Message::StreamAbort {
            xid,
            subxid,
            abort_lsn,
            abort_time,
        } = parse_one(b'A', "stream")
        else {
            panic!("expected stream abort");
        };
        assert_eq!((xid, subxid), (0x2f6, 0x2f7));
        assert!(abort_lsn.is_some() && abort_time.is_some());
        let Message::StreamCommit { xid, .. } = parse_one(b'c', "stream") else {
            panic!("expected stream commit");
        };
        assert_eq!(xid, 0x2f6);
        let Message::StreamPrepare { xid, gid, .. } = parse_one(b'p', "stream") else {
            panic!("expected stream prepare");
        };
        assert_eq!((xid, gid.as_str()), (0x2f8, "stream_prep"));
        let Message::Insert { xid, new, .. } = parse_one(b'I', "stream") else {
            panic!("expected insert");
        };
        assert_eq!(xid, Some(0x2f6));
        assert_eq!(new, vec![txt("1"), txt("bulk")]);
        assert!(matches!(
            parse_one(b'U', "stream"),
            Message::Update {
                xid: Some(0x2f9),
                ..
            }
        ));
        assert!(matches!(
            parse_one(b'D', "stream"),
            Message::Delete {
                xid: Some(0x2f9),
                ..
            }
        ));
    }

    #[test]
    fn every_truncated_prefix_is_an_error() {
        for s in samples() {
            let proto = proto_of(s.section);
            for cut in 0..s.bytes.len() {
                assert!(
                    parse(&s.bytes[..cut], in_stream_of(s.section, s.tag), proto).is_err(),
                    "tag {} cut at {} of {} parsed",
                    s.tag as char,
                    cut,
                    s.bytes.len()
                );
            }
        }
    }

    #[test]
    fn trailing_bytes_are_an_error() {
        for s in samples() {
            let mut long = s.bytes.clone();
            long.push(0);
            assert!(matches!(
                parse(&long, in_stream_of(s.section, s.tag), proto_of(s.section)),
                Err(Error::Trailing { .. })
            ));
        }
    }

    #[test]
    fn unknown_tags_are_an_error() {
        let mut msg = hex("45");
        msg[0] = b'Z';
        assert_eq!(parse(&msg, false, 4), Err(Error::UnknownTag(b'Z')));
    }

    #[test]
    fn corrupting_any_byte_never_panics() {
        for s in samples() {
            for i in 0..s.bytes.len() {
                let mut bad = s.bytes.clone();
                bad[i] ^= 0xff;
                let _ = parse(&bad, in_stream_of(s.section, s.tag), proto_of(s.section));
            }
        }
    }
}
