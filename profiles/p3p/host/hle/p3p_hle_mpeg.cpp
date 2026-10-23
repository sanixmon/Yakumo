
// sceMpeg HLE: the surface the game's movie player exercises while
// bringing up the opening movie (sceMpegInit -> QueryMemSize ->
// RingbufferConstruct -> Create -> QueryStreamOffset/Size ->
// RegistStream -> the AU loop). Semantics mirror PPSSPP's sceMpeg
// (structure sizes, the "LIBMPEG" handle layout, PSMF header
// analysis, stream ids, error codes). Deliberate limit: no demuxer
// or decoder exists behind this yet, so sceMpegGetAvcAu /
// sceMpegGetAtracAu report SCE_MPEG_ERROR_NO_DATA and the decode
// entry points fail with their fatal codes rather than fabricating
// frames. The demuxer below (Gate 6 follow-up) does parse the real
// MPEG-PS the ringbuffer callback delivers: video AUs split on
// PTS-bearing PES packets, audio AUs are single ATRAC3+ frames cut
// from the private-stream flow; AvcDecode fills a valid black frame
// and AtracDecode valid silence (right sizes/formats/timestamps,
// synthetic content -- there is still no H.264/ATRAC decoder).

#include "hle/p3p_hle_mpeg.hpp"

#include "kernel/p3p_kernel.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace p3p {

namespace {

inline constexpr std::uint32_t kNidMpegInit = 0x682A619Bu;
inline constexpr std::uint32_t kNidMpegFinish = 0x874624D6u;
inline constexpr std::uint32_t kNidMpegQueryMemSize = 0xC132E22Fu;
inline constexpr std::uint32_t kNidMpegCreate = 0xD8C5F121u;
inline constexpr std::uint32_t kNidMpegDelete = 0x606A4649u;
inline constexpr std::uint32_t kNidMpegRingbufferQueryMemSize = 0xD7A29F46u;
inline constexpr std::uint32_t kNidMpegRingbufferConstruct = 0x37295ED8u;
inline constexpr std::uint32_t kNidMpegRingbufferDestruct = 0x13407F13u;
inline constexpr std::uint32_t kNidMpegRingbufferPut = 0xB240A59Eu;
inline constexpr std::uint32_t kNidMpegRingbufferAvailableSize = 0xB5F6DC87u;
inline constexpr std::uint32_t kNidMpegQueryStreamOffset = 0x21FF80E4u;
inline constexpr std::uint32_t kNidMpegQueryStreamSize = 0x611E9E11u;
inline constexpr std::uint32_t kNidMpegRegistStream = 0x42560F23u;
inline constexpr std::uint32_t kNidMpegUnRegistStream = 0x591A4AA2u;
inline constexpr std::uint32_t kNidMpegFlushAllStream = 0x707B7629u;
inline constexpr std::uint32_t kNidMpegInitAu = 0x167AFD9Eu;
inline constexpr std::uint32_t kNidMpegGetAvcAu = 0xFE246728u;
inline constexpr std::uint32_t kNidMpegGetAtracAu = 0xE1CE83A7u;
inline constexpr std::uint32_t kNidMpegQueryAtracEsSize = 0xF8DCB679u;
inline constexpr std::uint32_t kNidMpegMallocAvcEsBuf = 0xA780CF7Eu;
inline constexpr std::uint32_t kNidMpegFreeAvcEsBuf = 0xCEB870B1u;
inline constexpr std::uint32_t kNidMpegAtracDecode = 0x800C44DFu;
inline constexpr std::uint32_t kNidMpegAvcDecode = 0x0E3C2E9Du;
inline constexpr std::uint32_t kNidMpegAvcDecodeStop = 0x740FCCD1u;
inline constexpr std::uint32_t kNidMpegAvcDecodeFlush = 0x4571CC64u;

inline constexpr std::uint32_t kMpegErrorNoData = 0x80618001u;        // SCE_MPEG_ERROR_NO_DATA
inline constexpr std::uint32_t kMpegErrorBadVersion = 0x80610002u;    // SCE_MPEG_ERROR_BAD_VERSION
inline constexpr std::uint32_t kMpegErrorNotYetInit = 0x80618009u;    // SCE_MPEG_ERROR_NOT_YET_INIT
inline constexpr std::uint32_t kMpegErrorNoMemory = 0x80610022u;      // SCE_MPEG_ERROR_NO_MEMORY
inline constexpr std::uint32_t kMpegErrorInvalidValue = 0x806101FEu;  // SCE_MPEG_ERROR_INVALID_VALUE
inline constexpr std::uint32_t kMpegErrorAvcDecodeFatal = 0x80628002u;  // SCE_MPEG_ERROR_AVC_DECODE_FATAL

// Handle memory the library requires (lib version >= 0x0105): 64 KiB.
inline constexpr std::uint32_t kMpegMemSize = 0x10000u;
// One ringbuffer packet slot: 104 bytes of header + 2048 of payload.
inline constexpr std::uint32_t kRingbufferPacketMem = 104u + 2048u;
inline constexpr std::uint32_t kPacketSize = 2048u;
inline constexpr std::uint32_t kPsmfMagic = 0x464D5350u;  // "PSMF" little-endian
inline constexpr std::uint32_t kAtracEsSize = 2112u;
inline constexpr std::uint32_t kAtracEsOutputSize = 8192u;
inline constexpr std::uint32_t kAvcEsSize = 2048u;
inline constexpr int kEsBuffers = 2;

// SceMpegRingBuffer field offsets (PPSSPP sceMpeg.h).
enum RbField : std::uint32_t {
    kRbPackets = 0,
    kRbPacketsRead = 4,
    kRbPacketsWritePos = 8,
    kRbPacketsAvail = 12,
    kRbPacketSize = 16,
    kRbData = 20,
    kRbCallback = 24,
    kRbCallbackArg = 28,
    kRbDataUpperBound = 32,
    kRbSemaId = 36,
    kRbMpeg = 40,
    kRbGp = 44,
};

struct DemuxAu {
    std::int64_t pts = -1;
    std::int64_t dts = -1;
    std::uint32_t end_offset = 0;  // stream offset just past this AU's last byte
    std::vector<std::uint8_t> data;
};

struct MpegRecord {
    std::uint32_t mpeg_addr = 0;  // the SceMpeg pointer the game holds
    std::uint32_t ringbuffer = 0;
    std::uint32_t frame_width = 0;
    std::uint32_t stream_offset = 0;
    std::uint32_t stream_size = 0;
    bool analyzed = false;
    bool es_buffers[kEsBuffers]{};
    std::map<std::int32_t, std::pair<std::uint32_t, std::uint32_t>> streams;  // sid -> (type, num)

    // Demuxer state. The ringbuffer carries the stream from its
    // first byte (the PSMF header included); positions below are
    // stream offsets into that byte flow.
    std::uint64_t total_put_bytes = 0;
    bool put_eof = false;
    bool demux_started = false;
    std::uint32_t parse_pos = 0;
    std::uint32_t consumed_pos = 0;  // end offset of the last AU handed out
    bool at_end = false;
    std::uint32_t avc_width = 480;
    std::uint32_t avc_height = 272;
    std::deque<DemuxAu> video_queue;
    std::deque<DemuxAu> audio_queue;
    DemuxAu video_open;          // AU under construction (data empty = none)
    bool video_open_valid = false;
    std::int64_t video_next_pts = -1;  // timestamps for the next picture to START
    std::int64_t video_next_dts = -1;
    std::size_t video_scanned = 0;     // AU data bytes already scanned for AUDs
    DemuxAu audio_partial;       // frame under construction
    std::int64_t audio_partial_pts = -1;
    bool audio_pts_pending = false;
    std::int64_t audio_pts_for_next = 0;
    std::int64_t audio_last_pts = -1;
    DemuxAu current_video;       // handed out by GetAvcAu, consumed by AvcDecode
    bool current_video_valid = false;
    DemuxAu current_audio;
    bool current_audio_valid = false;
};

// Host-side contexts, keyed by the handle value Create writes at
// *mpeg_addr (data buffer + 0x30), which is how every other call
// finds its context again (the reference does the same).
std::map<std::uint32_t, MpegRecord> g_mpegs;
std::uint64_t g_stat_packets_put = 0;
std::uint64_t g_stat_au_video = 0;
std::uint64_t g_stat_au_audio = 0;
std::uint32_t g_put_log_budget = 10;

// RingbufferPut detour state. sceMpegRingbufferPut with a callback
// is synchronous on hardware: the library CALLS the guest callback
// (which reads the stream file into the data area) and returns how
// many packets it produced. The host models that by pointing the
// calling thread at the callback with $ra set to kTrampoline; the
// missing-function hook routes the return here, where the round's
// bookkeeping lands and the next round (or the Put result) is set
// up. One pending Put at a time: the player's reader is a single
// thread, and any concurrent Put falls back to plain bookkeeping.
inline constexpr std::uint32_t kTrampoline = 0x0BADC0DEu;
struct PendingPut {
    bool active = false;
    std::int32_t thread_uid = 0;
    std::uint32_t rb = 0;
    std::int32_t total_want = 0;
    std::int32_t accumulated = 0;
    std::int32_t round_want = 0;
    std::uint32_t caller_ra = 0;
};
PendingPut g_pending_put;

// Sets up one callback round in the live context. Returns false when
// no round can run (bad ringbuffer state).
bool begin_put_round(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t rb = g_pending_put.rb;
    if (!memory.contains(rb, 48u))
        return false;
    const std::uint32_t packets = memory.load32(rb + kRbPackets);
    const std::uint32_t write_pos = memory.load32(rb + kRbPacketsWritePos);
    const std::uint32_t data = memory.load32(rb + kRbData);
    const std::uint32_t callback = memory.load32(rb + kRbCallback);
    const std::uint32_t cb_arg = memory.load32(rb + kRbCallbackArg);
    if (packets == 0u || callback == 0u)
        return false;
    const std::uint32_t offset = write_pos % packets;
    std::int32_t round = g_pending_put.total_want - g_pending_put.accumulated;
    const std::int32_t room = static_cast<std::int32_t>(packets - offset);
    if (round > room)
        round = room;
    if (round <= 0)
        return false;
    g_pending_put.round_want = round;
    ctx.gpr[4] = data + offset * kPacketSize;
    ctx.gpr[5] = static_cast<std::uint32_t>(round);
    ctx.gpr[6] = cb_arg;
    ctx.gpr[31] = kTrampoline;
    ctx.pc = callback;
    return true;
}
std::int32_t g_next_stream_id = 1;  // reset by sceMpegInit, per the reference
bool g_mpeg_initialized = false;

void set_v0(psprecomp::AllegrexContext &ctx, std::uint32_t value) { ctx.set_gpr(2, value); }

MpegRecord *find_by_guest_ptr(psprecomp::GuestMemory &memory, std::uint32_t mpeg_addr) {
    if (mpeg_addr == 0u || !memory.contains(mpeg_addr, 4u))
        return nullptr;
    const std::uint32_t handle = memory.load32(mpeg_addr);
    const auto it = g_mpegs.find(handle);
    return it == g_mpegs.end() ? nullptr : &it->second;
}

std::uint32_t bswap32(std::uint32_t v) {
    return ((v & 0x000000FFu) << 24u) | ((v & 0x0000FF00u) << 8u) |
           ((v & 0x00FF0000u) >> 8u) | ((v & 0xFF000000u) >> 24u);
}

// The PSMF header fields the Query* calls analyze (PPSSPP
// AnalyzeMpeg): magic at +0, version at +4, and big-endian stream
// offset/size at +8/+12.
struct PsmfHeader {
    bool magic_ok = false;
    std::uint32_t raw_version = 0;
    std::uint32_t stream_offset = 0;
    std::uint32_t stream_size = 0;
};

PsmfHeader read_psmf_header(psprecomp::GuestMemory &memory, std::uint32_t buffer) {
    PsmfHeader h;
    if (buffer == 0u || !memory.contains(buffer, 16u))
        return h;
    h.magic_ok = memory.load32(buffer) == kPsmfMagic;
    h.raw_version = memory.load32(buffer + 4u);
    h.stream_offset = bswap32(memory.load32(buffer + 8u));
    h.stream_size = bswap32(memory.load32(buffer + 12u));
    return h;
}

// PPSSPP's getMpegVersion: the raw version word packs the version as
// decimal digits in its bytes (e.g. 0x00000105-ish layouts are handled
// through the same nibble walk); negative result means "bad version".
std::int32_t mpeg_version_of(std::uint32_t raw) {
    // The reference extracts the version from the ASCII-ish digits in
    // the header's version field; firmwares this game targets report
    // 0x0105 or later. Treat any plausible nonzero version as valid.
    if (raw == 0u)
        return -1;
    return static_cast<std::int32_t>(raw);
}

}  // namespace


// ---------------------------------------------------------------------------
// Minimal MPEG-PS demuxer over the ringbuffer byte flow.
//
// Layout measured on this title's opening movie (and standard for
// PSMF): the stream is a sequence of 2048-byte packets; after the
// 2048-byte PSMF header come MPEG-PS packs (00 00 01 BA) and PES
// packets, each PES exactly filling its transport packet(s). Video
// lives in stream 0xE0; a PES carrying a PTS starts a new picture.
// Pictures inside the video flow are delimited by AUD NAL units
// (type 9): one AU per picture, with timestamps anchored at each
// PTS-bearing PES (sparse -- one per GOP here) and stepped by the
// frame duration for the pictures in between. Audio (ATRAC3+)
// lives in private stream 1 (0xBD): each PES payload
// opens with a 4-byte big-endian count of the bytes that complete
// the frame split across the previous PES, and the frames themselves
// are a continuous 752-byte flow across PES boundaries (160 kbps
// ATRAC3+, 2048 samples per frame = 4180 units at 90 kHz).

inline constexpr std::uint32_t kAtracFrameSize = 752u;
inline constexpr std::int64_t kAudioPtsStep = 4180;
inline constexpr std::int64_t kVideoPtsStep = 3003;

MpegRecord *find_by_ringbuffer(std::uint32_t rb) {
    for (auto &[handle, rec] : g_mpegs)
        if (rec.ringbuffer == rb)
            return &rec;
    return nullptr;
}

// Byte at a stream offset, mapped through the circular ringbuffer.
// Callers guarantee pos < total_put_bytes.
std::uint8_t demux_byte(psprecomp::GuestMemory &memory, const MpegRecord &rec,
                        std::uint32_t rb, std::uint32_t pos) {
    const std::uint32_t packets = memory.load32(rb + kRbPackets);
    const std::uint32_t data = memory.load32(rb + kRbData);
    const std::uint32_t slot = (pos / kPacketSize) % packets;
    return memory.load8(data + slot * kPacketSize + (pos % kPacketSize));
}

void demux_read(psprecomp::GuestMemory &memory, const MpegRecord &rec, std::uint32_t rb,
                std::uint32_t pos, std::uint8_t *out, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        out[i] = demux_byte(memory, rec, rb, pos + static_cast<std::uint32_t>(i));
}

std::int64_t demux_read_pts(const std::uint8_t *b) {
    return (static_cast<std::int64_t>((b[0] >> 1) & 0x07) << 30) |
           (static_cast<std::int64_t>(b[1]) << 22) |
           (static_cast<std::int64_t>((b[2] >> 1) & 0x7F) << 15) |
           (static_cast<std::int64_t>(b[3]) << 7) |
           (static_cast<std::int64_t>((b[4] >> 1) & 0x7F));
}

// Feeds audio bytes (the continuous frame flow) into the frame
// assembler, emitting complete 752-byte frames.
void demux_feed_audio(MpegRecord &rec, const std::uint8_t *bytes, std::size_t count,
                      std::uint32_t end_offset) {
    std::size_t i = 0;
    while (i < count) {
        if (rec.audio_partial.data.empty()) {
            // A new frame starts here: its timestamp is the pending
            // PES pts if one was queued, else the running clock.
            if (rec.audio_pts_pending) {
                rec.audio_partial_pts = rec.audio_pts_for_next;
                rec.audio_pts_pending = false;
            } else if (rec.audio_last_pts >= 0) {
                rec.audio_partial_pts = rec.audio_last_pts + kAudioPtsStep;
            } else {
                rec.audio_partial_pts = -1;
            }
        }
        const std::size_t room = kAtracFrameSize - rec.audio_partial.data.size();
        const std::size_t take = room < (count - i) ? room : (count - i);
        rec.audio_partial.data.insert(rec.audio_partial.data.end(), bytes + i, bytes + i + take);
        i += take;
        if (rec.audio_partial.data.size() == kAtracFrameSize) {
            DemuxAu au;
            au.pts = rec.audio_partial_pts;
            au.dts = rec.audio_partial_pts;
            au.end_offset = end_offset - static_cast<std::uint32_t>(count - i);
            au.data = std::move(rec.audio_partial.data);
            rec.audio_partial.data.clear();
            if (au.pts >= 0)
                rec.audio_last_pts = au.pts;
            rec.audio_queue.push_back(std::move(au));
        }
    }
}

// Advances the parse cursor, filling the AU queues, until the
// buffered data runs out or both queues hold an AU. Bounded per
// call so a Get*Au never stalls on a full buffer of the other kind.
void demux_pump(psprecomp::GuestMemory &memory, MpegRecord &rec) {
    const std::uint32_t rb = rec.ringbuffer;
    if (rb == 0u || !memory.contains(rb, 48u))
        return;
    if (!rec.demux_started) {
        rec.demux_started = true;
        rec.parse_pos = 0;
        // The flow opens with the PSMF header when present; the PS
        // data starts at the analyzed stream offset.
        if (rec.total_put_bytes >= 4u) {
            std::uint8_t magic[4];
            demux_read(memory, rec, rb, 0, magic, 4);
            if (magic[0] == 'P' && magic[1] == 'S' && magic[2] == 'M' && magic[3] == 'F')
                rec.parse_pos = rec.stream_offset != 0u ? rec.stream_offset : 2048u;
        }
    }
    const std::uint32_t budget_end = rec.parse_pos + 512u * 1024u;
    while (rec.parse_pos < rec.total_put_bytes && rec.parse_pos < budget_end) {
        if (!rec.video_queue.empty() && !rec.audio_queue.empty())
            return;
        const std::uint32_t pos = rec.parse_pos;
        // Need at least a start code + id.
        if (pos + 4u > rec.total_put_bytes)
            return;
        std::uint8_t sc[4];
        demux_read(memory, rec, rb, pos, sc, 4);
        if (sc[0] != 0u || sc[1] != 0u || sc[2] != 1u) {
            // Resync: scan for the next start code.
            std::uint32_t next = pos + 1u;
            bool found = false;
            while (next + 3u <= rec.total_put_bytes && next < budget_end) {
                if (demux_byte(memory, rec, rb, next) == 0u &&
                    demux_byte(memory, rec, rb, next + 1u) == 0u &&
                    demux_byte(memory, rec, rb, next + 2u) == 1u) {
                    found = true;
                    break;
                }
                ++next;
            }
            if (!found)
                return;
            rec.parse_pos = next;
            continue;
        }
        const std::uint8_t id = sc[3];
        if (id == 0xBAu) {  // pack header: 14 bytes + stuffing
            if (pos + 14u > rec.total_put_bytes)
                return;
            const std::uint8_t stuffing = demux_byte(memory, rec, rb, pos + 13u) & 0x07u;
            rec.parse_pos = pos + 14u + stuffing;
            continue;
        }
        if (pos + 6u > rec.total_put_bytes)
            return;
        const std::uint32_t pes_len =
            (static_cast<std::uint32_t>(demux_byte(memory, rec, rb, pos + 4u)) << 8u) |
            demux_byte(memory, rec, rb, pos + 5u);
        if (id == 0xBBu) {  // system header
            rec.parse_pos = pos + 6u + pes_len;
            continue;
        }
        const bool is_pes = (id >= 0xC0u && id <= 0xEFu) || id == 0xBDu;
        if (!is_pes) {
            if (pes_len == 0u)
                return;  // cannot size it; wait for nothing (shouldn't happen)
            rec.parse_pos = pos + 6u + pes_len;
            continue;
        }
        // PES with the standard optional header.
        if (pos + 9u > rec.total_put_bytes)
            return;
        const std::uint8_t flags = demux_byte(memory, rec, rb, pos + 7u);
        const std::uint8_t hdr_len = demux_byte(memory, rec, rb, pos + 8u);
        const std::uint32_t header_end = pos + 9u + hdr_len;
        if (pes_len == 0u) {
            // Unbounded PES (not used by this title): unsupported in
            // the minimal demuxer; stop rather than misparse.
            std::fprintf(stderr, "[p3p-mpeg] demux: unbounded PES at %u, stopping\n", pos);
            rec.parse_pos = static_cast<std::uint32_t>(rec.total_put_bytes);
            return;
        }
        const std::uint32_t pes_end = pos + 6u + pes_len;
        if (pes_end > rec.total_put_bytes)
            return;  // the rest of this PES hasn't been delivered yet
        const std::uint32_t payload_len = pes_end - header_end;
        std::int64_t pts = -1, dts = -1;
        const std::uint8_t pts_dts = (flags >> 6) & 0x03u;
        if (pts_dts == 2u || pts_dts == 3u) {
            std::uint8_t b[5];
            demux_read(memory, rec, rb, pos + 9u, b, 5);
            pts = demux_read_pts(b);
            if (pts_dts == 3u) {
                demux_read(memory, rec, rb, pos + 14u, b, 5);
                dts = demux_read_pts(b);
            } else {
                dts = pts;
            }
        }
        if (id == 0xBDu) {
            // Audio: 4-byte sub-header, then the continuous frame flow.
            if (pts >= 0) {
                rec.audio_pts_for_next = pts;
                rec.audio_pts_pending = true;
            }
            if (payload_len > 4u) {
                std::vector<std::uint8_t> payload(payload_len - 4u);
                demux_read(memory, rec, rb, header_end + 4u, payload.data(), payload.size());
                demux_feed_audio(rec, payload.data(), payload.size(), pes_end);
            }
        } else if (id >= 0xE0u && id <= 0xEFu) {
            // Video: anchor the running timestamps at this PES, then
            // append the payload and split pictures at AUD NALs.
            if (pts >= 0) {
                rec.video_next_pts = pts;
                rec.video_next_dts = dts;
            }
            if (!rec.video_open_valid) {
                rec.video_open = DemuxAu{};
                rec.video_open.pts = rec.video_next_pts;
                rec.video_open.dts = rec.video_next_dts;
                rec.video_next_pts += kVideoPtsStep;
                rec.video_next_dts += kVideoPtsStep;
                rec.video_open_valid = true;
                rec.video_scanned = 0;
            }
            if (payload_len > 0u) {
                const std::size_t at = rec.video_open.data.size();
                rec.video_open.data.resize(at + payload_len);
                demux_read(memory, rec, rb, header_end, rec.video_open.data.data() + at,
                           payload_len);
            }
            rec.video_open.end_offset = pes_end;
            // Scan the not-yet-scanned region (plus a 3-byte lookback
            // for start codes split across PES payloads) for AUDs.
            std::vector<std::uint8_t> &buf = rec.video_open.data;
            std::size_t scan = rec.video_scanned >= 3u ? rec.video_scanned - 3u : 0u;
            while (scan + 3u < buf.size()) {
                std::size_t found = buf.size();
                for (std::size_t i = scan; i + 3u < buf.size(); ++i) {
                    if (buf[i] == 0u && buf[i + 1] == 0u && buf[i + 2] == 1u &&
                        (buf[i + 3] & 0x1Fu) == 9u) {
                        found = i;
                        break;
                    }
                }
                if (found == buf.size())
                    break;
                const std::size_t boundary =
                    (found > 0u && buf[found - 1] == 0u) ? found - 1u : found;
                if (boundary == 0u) {
                    scan = found + 3u;  // this AUD opens the current AU
                    continue;
                }
                DemuxAu done;
                done.pts = rec.video_open.pts;
                done.dts = rec.video_open.dts;
                done.end_offset = pes_end;
                done.data.assign(buf.begin(), buf.begin() + boundary);
                rec.video_queue.push_back(std::move(done));
                buf.erase(buf.begin(), buf.begin() + boundary);
                rec.video_open.pts = rec.video_next_pts;
                rec.video_open.dts = rec.video_next_dts;
                rec.video_next_pts += kVideoPtsStep;
                rec.video_next_dts += kVideoPtsStep;
                scan = 0;
            }
            rec.video_scanned = buf.size();
        }
        // Other PES ids (MPEG audio etc.): not present in PSMF; skip.
        rec.parse_pos = pes_end;
    }
    if (rec.put_eof && rec.parse_pos >= rec.total_put_bytes) {
        if (rec.video_open_valid && !rec.video_open.data.empty()) {
            rec.video_queue.push_back(std::move(rec.video_open));
            rec.video_open = DemuxAu{};
            rec.video_open_valid = false;
        }
        rec.at_end = true;
    }
}

// Frees the ringbuffer packets an AU's byte span covered and keeps
// the guest-visible avail count honest.
void demux_consume(psprecomp::GuestMemory &memory, MpegRecord &rec, std::uint32_t end_offset) {
    const std::uint32_t rb = rec.ringbuffer;
    if (rb == 0u || !memory.contains(rb, 48u))
        return;
    const std::uint32_t before_packets = rec.consumed_pos / kPacketSize;
    if (end_offset > rec.consumed_pos)
        rec.consumed_pos = end_offset;
    const std::uint32_t freed = rec.consumed_pos / kPacketSize - before_packets;
    if (freed > 0u) {
        const std::uint32_t avail = memory.load32(rb + kRbPacketsAvail);
        memory.store32(rb + kRbPacketsAvail, avail >= freed ? avail - freed : 0u);
    }
}

// Writes a SceMpegAu {pts s64, dts s64, esBuffer, esSize}.
void write_au(psprecomp::GuestMemory &memory, std::uint32_t au_addr, std::int64_t pts,
              std::int64_t dts, std::uint32_t es_buffer, std::uint32_t es_size) {
    memory.store32(au_addr + 0u, static_cast<std::uint32_t>(pts & 0xFFFFFFFFu));
    memory.store32(au_addr + 4u, static_cast<std::uint32_t>((pts >> 32) & 0xFFFFFFFFu));
    memory.store32(au_addr + 8u, static_cast<std::uint32_t>(dts & 0xFFFFFFFFu));
    memory.store32(au_addr + 12u, static_cast<std::uint32_t>((dts >> 32) & 0xFFFFFFFFu));
    memory.store32(au_addr + 16u, es_buffer);
    memory.store32(au_addr + 20u, es_size);
}

bool hle_mpeg_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                       P3PKernel *kernel, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const std::uint32_t a0 = ctx.gpr[4];
    const std::uint32_t a1 = ctx.gpr[5];
    const std::uint32_t a2 = ctx.gpr[6];
    const std::uint32_t a3 = ctx.gpr[7];
    const std::uint32_t t0 = ctx.gpr[8];
    const std::uint32_t t1 = ctx.gpr[9];

    switch (nid) {
        case kNidMpegInit:
            if (g_mpeg_initialized) {
                set_v0(ctx, 0x80618005u);  // SCE_MPEG_ERROR_ALREADY_INIT
                return true;
            }
            g_mpegs.clear();
            g_next_stream_id = 1;
            g_mpeg_initialized = true;
            std::fprintf(stderr, "[p3p-mpeg] init\n");
            set_v0(ctx, 0);
            return true;
        case kNidMpegFinish:
            g_mpegs.clear();
            g_mpeg_initialized = false;
            set_v0(ctx, 0);
            return true;
        case kNidMpegQueryMemSize:
            set_v0(ctx, kMpegMemSize);
            return true;
        case kNidMpegRingbufferQueryMemSize:
            set_v0(ctx, a0 * kRingbufferPacketMem);
            return true;
        case kNidMpegRingbufferConstruct: {
            // SceMpegRingbuffer *rb, packets, data, size, callback, callbackArg
            if (a0 == 0u || !memory.contains(a0, 48u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            if (static_cast<std::int32_t>(a3) < 0 ||
                (a1 < 0x100000u && a1 * kRingbufferPacketMem > a3)) {
                set_v0(ctx, kMpegErrorNoMemory);
                return true;
            }
            memory.store32(a0 + kRbPackets, a1);
            memory.store32(a0 + kRbPacketsRead, 0u);
            memory.store32(a0 + kRbPacketsWritePos, 0u);
            memory.store32(a0 + kRbPacketsAvail, 0u);
            memory.store32(a0 + kRbPacketSize, kPacketSize);
            memory.store32(a0 + kRbData, a2);
            memory.store32(a0 + kRbCallback, t0);
            memory.store32(a0 + kRbCallbackArg, t1);
            memory.store32(a0 + kRbDataUpperBound, a2 + a1 * kPacketSize);
            // semaID (+36) is not written by this call on hardware.
            memory.store32(a0 + kRbMpeg, 0u);
            memory.store32(a0 + kRbGp, ctx.gpr[28]);
            std::fprintf(stderr,
                         "[p3p-mpeg] ringbuffer construct rb %s packets %u data %s callback %s cbarg %s\n",
                         psprecomp::hex32(a0).c_str(), a1, psprecomp::hex32(a2).c_str(),
                         psprecomp::hex32(t0).c_str(), psprecomp::hex32(t1).c_str());
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegRingbufferDestruct:
            if (a0 != 0u && memory.contains(a0, 48u))
                memory.store32(a0 + kRbMpeg, 0u);
            set_v0(ctx, 0);
            return true;
        case kNidMpegCreate: {
            // SceMpeg *mpeg, data, size, ringbuffer, frameWidth, mode, ddrTop
            if (a0 == 0u || !memory.contains(a0, 4u) || a1 == 0u) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            if (a2 < kMpegMemSize) {
                set_v0(ctx, kMpegErrorNoMemory);
                return true;
            }
            const std::uint32_t handle = a1 + 0x30u;
            if (!memory.contains(handle, 24u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            memory.store32(a0, handle);
            // The handle struct the library stamps into the data
            // buffer: "LIBMPEG\0" "001\0" -1, ringbuffer, data upper.
            static constexpr char kMagic[8] = {'L', 'I', 'B', 'M', 'P', 'E', 'G', '\0'};
            for (std::uint32_t i = 0; i < 8u; ++i)
                memory.store8(handle + i, static_cast<std::uint8_t>(kMagic[i]));
            memory.store32(handle + 8u, 0x00313030u);  // "001\0" as stored by the reference
            memory.store32(handle + 12u, 0xFFFFFFFFu);
            std::uint32_t rb_upper = 0u;
            if (a3 != 0u && memory.contains(a3, 48u)) {
                rb_upper = memory.load32(a3 + kRbDataUpperBound);
                memory.store32(a3 + kRbMpeg, a0);
            }
            memory.store32(handle + 16u, a3);
            memory.store32(handle + 20u, rb_upper);
            MpegRecord rec;
            rec.mpeg_addr = a0;
            rec.ringbuffer = a3;
            rec.frame_width = t0;
            g_mpegs[handle] = rec;
            std::fprintf(stderr, "[p3p-mpeg] create mpeg %s handle %s rb %s framewidth %u\n",
                         psprecomp::hex32(a0).c_str(), psprecomp::hex32(handle).c_str(),
                         psprecomp::hex32(a3).c_str(), t0);
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegDelete: {
            if (a0 == 0u || !memory.contains(a0, 4u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const std::uint32_t handle = memory.load32(a0);
            if (g_mpegs.erase(handle) == 0u) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegQueryStreamOffset: {
            // (mpeg, buffer, offsetAddr): analyzes the PSMF header in
            // the stream buffer against this context.
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr || a2 == 0u || !memory.contains(a2, 4u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const PsmfHeader h = read_psmf_header(memory, a1);
            if (!h.magic_ok) {
                memory.store32(a2, 0u);
                set_v0(ctx, kMpegErrorInvalidValue);
                return true;
            }
            if (mpeg_version_of(h.raw_version) < 0) {
                memory.store32(a2, 0u);
                set_v0(ctx, kMpegErrorBadVersion);
                return true;
            }
            if ((h.stream_offset & 2047u) != 0u || h.stream_offset == 0u) {
                memory.store32(a2, 0u);
                set_v0(ctx, kMpegErrorInvalidValue);
                return true;
            }
            rec->stream_offset = h.stream_offset;
            rec->stream_size = h.stream_size;
            rec->analyzed = true;
            if (memory.contains(a1, 144u)) {
                rec->avc_width = static_cast<std::uint32_t>(memory.load8(a1 + 142u)) * 16u;
                rec->avc_height = static_cast<std::uint32_t>(memory.load8(a1 + 143u)) * 16u;
            }
            memory.store32(a2, h.stream_offset);
            std::fprintf(stderr, "[p3p-mpeg] stream offset %u size %u\n",
                         h.stream_offset, h.stream_size);
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegQueryStreamSize: {
            // (buffer, sizeAddr): no mpeg handle involved.
            if (a1 == 0u || !memory.contains(a1, 4u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const PsmfHeader h = read_psmf_header(memory, a0);
            if (!h.magic_ok || (h.stream_offset & 2047u) != 0u) {
                memory.store32(a1, 0u);
                set_v0(ctx, kMpegErrorInvalidValue);
                return true;
            }
            memory.store32(a1, h.stream_size);
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegRegistStream: {
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const std::int32_t sid = g_next_stream_id++;
            rec->streams[sid] = {a1, a2};
            std::fprintf(stderr, "[p3p-mpeg] regist stream type %u num %u -> sid %d\n",
                         a1, a2, sid);
            set_v0(ctx, static_cast<std::uint32_t>(sid));
            return true;
        }
        case kNidMpegUnRegistStream: {
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            rec->streams.erase(static_cast<std::int32_t>(a1));
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegFlushAllStream: {
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            rec->video_queue.clear();
            rec->audio_queue.clear();
            rec->video_open = DemuxAu{};
            rec->video_open_valid = false;
            rec->video_next_pts = -1;
            rec->video_next_dts = -1;
            rec->video_scanned = 0;
            rec->audio_partial = DemuxAu{};
            rec->current_video_valid = false;
            rec->current_audio_valid = false;
            rec->demux_started = false;
            rec->parse_pos = 0;
            rec->consumed_pos = 0;
            rec->at_end = false;
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegRingbufferAvailableSize: {
            // (ringbuffer) -> free packet slots, returned as the value.
            if (a0 == 0u || !memory.contains(a0, 48u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const std::uint32_t packets = memory.load32(a0 + kRbPackets);
            const std::uint32_t avail = memory.load32(a0 + kRbPacketsAvail);
            set_v0(ctx, packets >= avail ? packets - avail : 0u);
            return true;
        }
        case kNidMpegRingbufferPut: {
            // (ringbuffer, numPackets, available): the caller has
            // filled the data area (or the registered callback would);
            // bookkeeping only -- counts clamp to free space, and the
            // number actually enqueued is returned. Guest-callback
            // invocation is not modelled yet (no callback is expected
            // from this title's wrapper; the boot log will say if one
            // is registered).
            if (a0 == 0u || !memory.contains(a0, 48u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const std::uint32_t packets = memory.load32(a0 + kRbPackets);
            const std::uint32_t avail = memory.load32(a0 + kRbPacketsAvail);
            std::int32_t want = static_cast<std::int32_t>(a1);
            if (want > static_cast<std::int32_t>(a2))
                want = static_cast<std::int32_t>(a2);
            const std::int32_t free_slots = static_cast<std::int32_t>(packets - avail);
            if (want > free_slots)
                want = free_slots;
            if (want <= 0) {
                set_v0(ctx, 0);
                return true;
            }
            // With a registered callback the library pulls the data
            // itself: detour the caller into the callback (see
            // PendingPut). The Put completes at the trampoline.
            if (memory.load32(a0 + kRbCallback) != 0u && kernel != nullptr &&
                !g_pending_put.active) {
                g_pending_put.active = true;
                g_pending_put.thread_uid = kernel->current_uid();
                g_pending_put.rb = a0;
                g_pending_put.total_want = want;
                g_pending_put.accumulated = 0;
                g_pending_put.round_want = 0;
                g_pending_put.caller_ra = ctx.gpr[31];
                if (begin_put_round(memory, ctx)) {
                    std::fprintf(stderr,
                                 "[p3p-mpeg] ringbuffer put detours into callback %s "
                                 "(want %d packets)\n",
                                 psprecomp::hex32(memory.load32(a0 + kRbCallback)).c_str(),
                                 want);
                    return true;
                }
                g_pending_put.active = false;
            }
            g_stat_packets_put += static_cast<std::uint32_t>(want);
            if (MpegRecord *rec = find_by_ringbuffer(a0))
                rec->total_put_bytes += static_cast<std::uint64_t>(want) * kPacketSize;
            if (g_put_log_budget > 0u || (g_stat_packets_put % 480u) < static_cast<std::uint64_t>(want)) {
                if (g_put_log_budget > 0u)
                    --g_put_log_budget;
                std::fprintf(stderr,
                             "[p3p-mpeg] ringbuffer put +%d packets (avail %u, total put %llu)\n",
                             want, avail + static_cast<std::uint32_t>(want),
                             static_cast<unsigned long long>(g_stat_packets_put));
            }
            const std::uint32_t write_pos = memory.load32(a0 + kRbPacketsWritePos);
            memory.store32(a0 + kRbPacketsAvail, avail + static_cast<std::uint32_t>(want));
            memory.store32(a0 + kRbPacketsWritePos,
                           packets == 0u ? 0u
                                         : (write_pos + static_cast<std::uint32_t>(want)) % packets);
            set_v0(ctx, static_cast<std::uint32_t>(want));
            return true;
        }
        case kNidMpegInitAu: {
            // (mpeg, esBufferIdOrIgnored, auAddr): stamps a fresh
            // SceMpegAu {pts, dts, esBuffer, esSize} at the pointer.
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr || a2 == 0u || !memory.contains(a2, 24u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const bool avc = a1 >= 1u && a1 <= static_cast<std::uint32_t>(kEsBuffers) &&
                             rec->es_buffers[a1 - 1u];
            memory.store32(a2 + 0u, 0u);   // pts low
            memory.store32(a2 + 4u, 0u);   // pts high
            if (avc) {
                memory.store32(a2 + 8u, 0u);            // dts = 0
                memory.store32(a2 + 12u, 0u);
                memory.store32(a2 + 20u, kAvcEsSize);
            } else {
                memory.store32(a2 + 8u, 0xFFFFFFFFu);    // dts = UNKNOWN (-1)
                memory.store32(a2 + 12u, 0xFFFFFFFFu);
                memory.store32(a2 + 20u, kAtracEsSize);
            }
            memory.store32(a2 + 16u, 0u);  // esBuffer (reference stores the stream id slot as 0)
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegQueryAtracEsSize:
            if (find_by_guest_ptr(memory, a0) == nullptr || a1 == 0u || a2 == 0u ||
                !memory.contains(a1, 4u) || !memory.contains(a2, 4u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            memory.store32(a1, kAtracEsSize);
            memory.store32(a2, kAtracEsOutputSize);
            set_v0(ctx, 0);
            return true;
        case kNidMpegMallocAvcEsBuf: {
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            for (int i = 0; i < kEsBuffers; ++i) {
                if (!rec->es_buffers[i]) {
                    rec->es_buffers[i] = true;
                    set_v0(ctx, static_cast<std::uint32_t>(i + 1));
                    return true;
                }
            }
            set_v0(ctx, kMpegErrorNoMemory);
            return true;
        }
        case kNidMpegFreeAvcEsBuf: {
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            if (a1 >= 1u && a1 <= static_cast<std::uint32_t>(kEsBuffers))
                rec->es_buffers[a1 - 1u] = false;
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegGetAvcAu:
        case kNidMpegGetAtracAu: {
            const bool audio = nid == kNidMpegGetAtracAu;
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr || a2 == 0u || !memory.contains(a2, 24u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            demux_pump(memory, *rec);
            std::deque<DemuxAu> &queue = audio ? rec->audio_queue : rec->video_queue;
            if (!queue.empty()) {
                DemuxAu au = std::move(queue.front());
                queue.pop_front();
                const auto sit = rec->streams.find(static_cast<std::int32_t>(a1));
                const std::uint32_t stream_num = sit != rec->streams.end() ? sit->second.second : 0u;
                write_au(memory, a2, au.pts, audio ? au.pts : au.dts, stream_num,
                         static_cast<std::uint32_t>(au.data.size()));
                if (a3 != 0u && memory.contains(a3, 4u))
                    memory.store32(a3, audio ? 0u : 1u);
                demux_consume(memory, *rec, au.end_offset);
                if (audio) {
                    rec->current_audio = std::move(au);
                    rec->current_audio_valid = true;
                    ++g_stat_au_audio;
                } else {
                    rec->current_video = std::move(au);
                    rec->current_video_valid = true;
                    ++g_stat_au_video;
                }
                if (g_stat_au_video + g_stat_au_audio <= 12u ||
                    ((g_stat_au_video + g_stat_au_audio) % 200u) == 0u)
                    std::fprintf(stderr,
                                 "[p3p-mpeg] get%sau ok pts %lld size %u (totals v %llu a %llu)\n",
                                 audio ? "atrac" : "avc",
                                 static_cast<long long>(audio ? rec->current_audio.pts
                                                              : rec->current_video.pts),
                                 static_cast<unsigned>(audio ? rec->current_audio.data.size()
                                                             : rec->current_video.data.size()),
                                 (unsigned long long)g_stat_au_video,
                                 (unsigned long long)g_stat_au_audio);
                set_v0(ctx, 0);
                return true;
            }
            if (rec->at_end) {
                // End of stream: report the last timestamps with
                // dts = -1, the reference's end signal.
                const std::int64_t last = audio ? rec->audio_last_pts : -1;
                write_au(memory, a2, last >= 0 ? last : 0, -1, 0u, 0u);
                set_v0(ctx, kMpegErrorNoData);
                return true;
            }
            if (rec->ringbuffer != 0u && memory.contains(rec->ringbuffer, 48u) &&
                memory.load32(rec->ringbuffer + kRbPacketsAvail) == 0u)
                write_au(memory, a2, 0, 0, 0u, 0u);
            set_v0(ctx, kMpegErrorNoData);
            return true;
        }
        case kNidMpegAtracDecode: {
            // (mpeg, auAddr, bufferAddr, init): the reference memsets
            // the output to silence before writing samples; with no
            // ATRAC decoder the silence IS the output -- 8192 bytes
            // of valid 44.1 kHz stereo PCM per frame.
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr || a2 == 0u) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            for (std::uint32_t off = 0; off < kAtracEsOutputSize; off += 4u) {
                if (!memory.contains(a2 + off, 4u))
                    break;
                memory.store32(a2 + off, 0u);
            }
            if (a1 != 0u && memory.contains(a1, 24u)) {
                std::int64_t next = -1;
                if (!rec->audio_queue.empty())
                    next = rec->audio_queue.front().pts;
                else if (rec->current_audio_valid)
                    next = rec->current_audio.pts + kAudioPtsStep;
                if (next >= 0) {
                    memory.store32(a1 + 0u, static_cast<std::uint32_t>(next & 0xFFFFFFFFu));
                    memory.store32(a1 + 4u, static_cast<std::uint32_t>((next >> 32) & 0xFFFFFFFFu));
                }
            }
            rec->current_audio_valid = false;
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegAvcDecode: {
            // (mpeg, auAddr, frameWidth, bufferAddr, initAddr):
            // fills a valid ABGR8888 black frame at *bufferAddr,
            // reports frame status 1 at *initAddr, and advances the
            // AU's pts to the next picture like the reference. There
            // is no H.264 decoder behind this: the pixels are
            // synthetic, the contract (sizes, format, timestamps,
            // return codes) is real.
            MpegRecord *rec = find_by_guest_ptr(memory, a0);
            if (rec == nullptr || !rec->current_video_valid) {
                set_v0(ctx, kMpegErrorAvcDecodeFatal);
                return true;
            }
            if (a3 == 0u || t0 == 0u || !memory.contains(a3, 4u) || !memory.contains(t0, 4u)) {
                set_v0(ctx, static_cast<std::uint32_t>(-1));
                return true;
            }
            const std::uint32_t buffer = memory.load32(a3);
            if (buffer == 0u) {
                set_v0(ctx, kMpegErrorAvcDecodeFatal);
                return true;
            }
            const std::uint32_t stride = a2 != 0u ? a2 : rec->frame_width;
            const std::uint32_t height = rec->avc_height != 0u ? rec->avc_height : 272u;
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint32_t row = buffer + y * stride * 4u;
                if (!memory.contains(row, stride * 4u))
                    break;
                for (std::uint32_t x = 0; x < stride; ++x)
                    memory.store32(row + x * 4u, 0xFF000000u);
            }
            memory.store32(t0, 1u);
            if (a1 != 0u && memory.contains(a1, 24u)) {
                const std::int64_t next =
                    !rec->video_queue.empty()
                        ? rec->video_queue.front().pts
                        : rec->current_video.pts + kVideoPtsStep;
                memory.store32(a1 + 0u, static_cast<std::uint32_t>(next & 0xFFFFFFFFu));
                memory.store32(a1 + 4u, static_cast<std::uint32_t>((next >> 32) & 0xFFFFFFFFu));
            }
            rec->current_video_valid = false;
            static std::uint32_t s_decode_logs = 0;
            if (s_decode_logs < 6u) {
                ++s_decode_logs;
                std::fprintf(stderr, "[p3p-mpeg] avcdecode ok buffer %s stride %u\n",
                             psprecomp::hex32(buffer).c_str(), stride);
            }
            set_v0(ctx, 0);
            return true;
        }
        case kNidMpegAvcDecodeStop:
            set_v0(ctx, find_by_guest_ptr(memory, a0) != nullptr
                            ? 0u
                            : static_cast<std::uint32_t>(-1));
            return true;
        case kNidMpegAvcDecodeFlush:
            set_v0(ctx, find_by_guest_ptr(memory, a0) != nullptr
                            ? 0u
                            : static_cast<std::uint32_t>(-1));
            return true;
        default:
            return false;
    }
}

bool hle_mpeg_trampoline(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                         std::uint32_t pc) {
    if (pc != kTrampoline || !g_pending_put.active)
        return false;
    psprecomp::GuestMemory &memory = runtime.memory();
    const std::uint32_t rb = g_pending_put.rb;
    // The callback's return value is the packets it wrote this round.
    std::int32_t got = static_cast<std::int32_t>(ctx.gpr[2]);
    if (got < 0)
        got = 0;
    if (got > g_pending_put.round_want)
        got = g_pending_put.round_want;
    if (memory.contains(rb, 48u) && got > 0) {
        const std::uint32_t avail = memory.load32(rb + kRbPacketsAvail);
        const std::uint32_t write_pos = memory.load32(rb + kRbPacketsWritePos);
        const std::uint32_t read = memory.load32(rb + kRbPacketsRead);
        memory.store32(rb + kRbPacketsAvail, avail + static_cast<std::uint32_t>(got));
        memory.store32(rb + kRbPacketsWritePos, write_pos + static_cast<std::uint32_t>(got));
        memory.store32(rb + kRbPacketsRead, read + static_cast<std::uint32_t>(got));
        g_stat_packets_put += static_cast<std::uint32_t>(got);
    }
    if (MpegRecord *rec = find_by_ringbuffer(rb)) {
        rec->total_put_bytes += static_cast<std::uint64_t>(got) * kPacketSize;
        if (got < g_pending_put.round_want)
            rec->put_eof = true;  // a short callback round means end of file
    }
    g_pending_put.accumulated += got;
    // Another round when the callback filled its whole allotment and
    // the caller asked for more than that.
    if (got == g_pending_put.round_want &&
        g_pending_put.accumulated < g_pending_put.total_want &&
        begin_put_round(memory, ctx))
        return true;
    const std::int32_t result = g_pending_put.accumulated;
    const std::uint32_t caller_ra = g_pending_put.caller_ra;
    g_pending_put.active = false;
    std::fprintf(stderr, "[p3p-mpeg] ringbuffer put completed via callback: %d packets\n",
                 result);
    ctx.gpr[2] = static_cast<std::uint32_t>(result);
    ctx.pc = caller_ra;
    return true;
}

MpegProgress hle_mpeg_progress() {
    MpegProgress p;
    p.packets_put = g_stat_packets_put;
    p.au_video = g_stat_au_video;
    p.au_audio = g_stat_au_audio;
    return p;
}

}  // namespace p3p
