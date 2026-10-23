
// sceMpeg HLE: the surface the game's movie player exercises while
// bringing up the opening movie (sceMpegInit -> QueryMemSize ->
// RingbufferConstruct -> Create -> QueryStreamOffset/Size ->
// RegistStream -> the AU loop). Semantics mirror PPSSPP's sceMpeg
// (structure sizes, the "LIBMPEG" handle layout, PSMF header
// analysis, stream ids, error codes). Deliberate limit: no demuxer
// or decoder exists behind this yet, so sceMpegGetAvcAu /
// sceMpegGetAtracAu report SCE_MPEG_ERROR_NO_DATA and the decode
// entry points fail with their fatal codes rather than fabricating
// frames.

#include "hle/p3p_hle_mpeg.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <cstring>
#include <map>

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

inline constexpr std::uint32_t kErrorNoData = 0x80618001u;        // SCE_MPEG_ERROR_NO_DATA
inline constexpr std::uint32_t kErrorBadVersion = 0x80610002u;    // SCE_MPEG_ERROR_BAD_VERSION
inline constexpr std::uint32_t kErrorNotYetInit = 0x80618009u;    // SCE_MPEG_ERROR_NOT_YET_INIT
inline constexpr std::uint32_t kErrorNoMemory = 0x80610022u;      // SCE_MPEG_ERROR_NO_MEMORY
inline constexpr std::uint32_t kErrorInvalidValue = 0x806101FEu;  // SCE_MPEG_ERROR_INVALID_VALUE
inline constexpr std::uint32_t kErrorAvcDecodeFatal = 0x80628002u;  // SCE_MPEG_ERROR_AVC_DECODE_FATAL

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

struct MpegRecord {
    std::uint32_t mpeg_addr = 0;  // the SceMpeg pointer the game holds
    std::uint32_t ringbuffer = 0;
    std::uint32_t frame_width = 0;
    std::uint32_t stream_offset = 0;
    std::uint32_t stream_size = 0;
    bool analyzed = false;
    bool es_buffers[kEsBuffers]{};
    std::map<std::int32_t, std::pair<std::uint32_t, std::uint32_t>> streams;  // sid -> (type, num)
};

// Host-side contexts, keyed by the handle value Create writes at
// *mpeg_addr (data buffer + 0x30), which is how every other call
// finds its context again (the reference does the same).
std::map<std::uint32_t, MpegRecord> g_mpegs;
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

bool hle_mpeg_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                       std::uint32_t nid) {
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
                set_v0(ctx, kErrorNoMemory);
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
            std::fprintf(stderr, "[p3p-mpeg] ringbuffer construct rb %s packets %u data %s\n",
                         psprecomp::hex32(a0).c_str(), a1, psprecomp::hex32(a2).c_str());
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
                set_v0(ctx, kErrorNoMemory);
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
                set_v0(ctx, kErrorInvalidValue);
                return true;
            }
            if (mpeg_version_of(h.raw_version) < 0) {
                memory.store32(a2, 0u);
                set_v0(ctx, kErrorBadVersion);
                return true;
            }
            if ((h.stream_offset & 2047u) != 0u || h.stream_offset == 0u) {
                memory.store32(a2, 0u);
                set_v0(ctx, kErrorInvalidValue);
                return true;
            }
            rec->stream_offset = h.stream_offset;
            rec->stream_size = h.stream_size;
            rec->analyzed = true;
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
                set_v0(ctx, kErrorInvalidValue);
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
        case kNidMpegFlushAllStream:
            set_v0(ctx, find_by_guest_ptr(memory, a0) != nullptr
                            ? 0u
                            : static_cast<std::uint32_t>(-1));
            return true;
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
            set_v0(ctx, kErrorNoMemory);
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
        case kNidMpegGetAtracAu:
            // No demuxer: there is never a complete AU to hand out.
            set_v0(ctx, find_by_guest_ptr(memory, a0) != nullptr
                            ? kErrorNoData
                            : static_cast<std::uint32_t>(-1));
            return true;
        case kNidMpegAtracDecode:
            set_v0(ctx, kErrorNoData);
            return true;
        case kNidMpegAvcDecode:
        case kNidMpegAvcDecodeStop:
            set_v0(ctx, kErrorAvcDecodeFatal);
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

}  // namespace p3p
