#include "hle/p3p_hle_io.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace p3p {
namespace {

// ---- Error codes (PPSSPP Core/HLE/ErrorCodes.h; errno mapping 0x80010000+e).
constexpr std::uint32_t kErrFileNotFound = 0x80010002u;  // ENOENT
constexpr std::uint32_t kErrFileExist = 0x80010011u;     // EEXIST
constexpr std::uint32_t kErrInvalidArg = 0x80010016u;    // EINVAL
constexpr std::uint32_t kErrTooManyOpen = 0x80010018u;   // EMFILE
constexpr std::uint32_t kErrReadOnly = 0x8001001Eu;      // EROFS
constexpr std::uint32_t kErrBadF = 0x80020323u;          // SCE_KERNEL_ERROR_BADF
constexpr std::uint32_t kErrNoAsync = 0x8002032Au;       // SCE_KERNEL_ERROR_NOASYNC

// ---- PSP open flags (sceIoOpen).
constexpr std::uint32_t kAccessMask = 0x0003u;
constexpr std::uint32_t kOReadOnly = 0x0001u;
constexpr std::uint32_t kOWriteOnly = 0x0002u;
constexpr std::uint32_t kOAppend = 0x0100u;
constexpr std::uint32_t kOCreat = 0x0200u;
constexpr std::uint32_t kOTrunc = 0x0400u;
constexpr std::uint32_t kOExcl = 0x0800u;

struct OpenFile {
    std::fstream stream;
    fs::path host_path;
    bool read_only_device = true;
    bool readable = false;
    bool writable = false;
    bool has_async_result = false;
    std::int64_t async_result = 0;
};

struct OpenDir {
    fs::path host_path;
    bool read_only_device = true;
    std::vector<fs::directory_entry> entries;
    std::size_t next = 0;  // 0 -> ".", 1 -> "..", >=2 -> entries[next-2]
};

struct IoState {
    std::map<std::int32_t, OpenFile> files;
    std::map<std::int32_t, OpenDir> dirs;
    std::map<std::int32_t, std::int64_t> async_tombstones;  // closed fds
    std::int32_t next_id = 3;                               // 0,1,2 = stdio
};

IoState g_io;

bool io_trace_enabled() {
    static const bool enabled = std::getenv("P3P_IO_TRACE") != nullptr;
    return enabled;
}

void io_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[p3p-io] %s\n", buf);
}

// ---- Guest memory helpers -------------------------------------------------

std::string read_guest_string(psprecomp::GuestMemory &memory, std::uint32_t address,
                              std::size_t max_len = 1024) {
    std::string out;
    for (std::size_t i = 0; i < max_len; ++i) {
        const char c = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0') break;
        out.push_back(c);
    }
    return out;
}

void write_guest_bytes(psprecomp::GuestMemory &memory, std::uint32_t address,
                       const std::uint8_t *data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
        memory.store8(address + static_cast<std::uint32_t>(i), data[i]);
}

void read_guest_bytes(psprecomp::GuestMemory &memory, std::uint32_t address,
                      std::uint8_t *data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
        data[i] = memory.load8(address + static_cast<std::uint32_t>(i));
}

void store64(psprecomp::GuestMemory &memory, std::uint32_t address, std::int64_t value) {
    const std::uint64_t v = static_cast<std::uint64_t>(value);
    memory.store32(address, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    memory.store32(address + 4u, static_cast<std::uint32_t>(v >> 32u));
}

// ---- Path handling ---------------------------------------------------------

bool is_read_only_device(const std::string &psp_path) {
    const auto colon = psp_path.find(':');
    if (colon == std::string::npos) return true;  // no device: treat as disc
    const std::string device = psp_path.substr(0, colon);
    return !(device == "ms0" || device == "host0");
}

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// translate_path + case-insensitive fallback walk (UMD names vs literals).
fs::path resolve_host_path(psprecomp::Runtime &runtime, const std::string &psp_path) {
    const fs::path translated = runtime.translate_path(psp_path);  // may throw
    std::error_code ec;
    if (fs::exists(translated, ec)) return translated;
    const fs::path root = runtime.game_root();
    if (root.empty()) return translated;
    const fs::path rel = translated.lexically_relative(root);
    if (rel.empty()) return translated;
    fs::path current = root;
    for (const auto &part : rel) {
        if (!fs::is_directory(current, ec)) return translated;
        const std::string want = ascii_lower(part.string());
        bool found = false;
        for (const auto &entry : fs::directory_iterator(current, ec)) {
            if (ascii_lower(entry.path().filename().string()) == want) {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found) return translated;
    }
    return current;
}

// ---- SceIoStat / SceIoDirEnt filling --------------------------------------
// SceIoStat (88 bytes): mode u32 @0, attr u32 @4, size i64 @8, then three
// ScePspDateTime (16 bytes each: six u16 fields + u32 microsecond), then
// st_private[6] zeroed. SceIoDirEnt adds d_name[256] @88 and d_private @344.

void put_u16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8u) & 0xFFu);
}
void put_u32(std::uint8_t *p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu);
}
void put_u64(std::uint8_t *p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((v >> (8u * i)) & 0xFFu);
}

void fill_datetime(std::uint8_t *p, const fs::path &path) {
    std::error_code ec;
    const auto tp = fs::last_write_time(path, ec);
    if (ec) return;
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        tp - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t t = system_clock::to_time_t(sctp);
    std::tm tmv{};
    if (const std::tm *lt = std::localtime(&t)) tmv = *lt;
    put_u16(p + 0, static_cast<std::uint16_t>(tmv.tm_year + 1900));
    put_u16(p + 2, static_cast<std::uint16_t>(tmv.tm_mon + 1));
    put_u16(p + 4, static_cast<std::uint16_t>(tmv.tm_mday));
    put_u16(p + 6, static_cast<std::uint16_t>(tmv.tm_hour));
    put_u16(p + 8, static_cast<std::uint16_t>(tmv.tm_min));
    put_u16(p + 10, static_cast<std::uint16_t>(tmv.tm_sec));
    put_u32(p + 12, 0u);  // microsecond
}

// Fills an 88-byte SceIoStat image for host_path. Returns false on error.
bool build_stat(const fs::path &host_path, bool read_only_device, std::uint8_t out[88]) {
    std::error_code ec;
    const bool is_dir = fs::is_directory(host_path, ec);
    if (ec) return false;
    std::memset(out, 0, 88);
    const std::uint32_t type = is_dir ? 0x1000u : 0x2000u;   // FIO_S_IFDIR / IFREG
    const std::uint32_t attr = is_dir ? 0x10u : 0x20u;       // TYPE_DIR / TYPE_FILE
    put_u32(out + 0, type | (read_only_device ? 0555u : 0777u));
    put_u32(out + 4, attr | (read_only_device ? 0x01u : 0x00u));
    const std::uint64_t size = is_dir ? 0u : fs::file_size(host_path, ec);
    if (ec) return false;
    put_u64(out + 8, size);
    fill_datetime(out + 16, host_path);
    fill_datetime(out + 32, host_path);
    fill_datetime(out + 48, host_path);
    return true;
}

// ---- Core operations --------------------------------------------------------
// Each returns >= 0 on success (fd / byte count / position) or an error code
// value with `is_error` semantics handled by callers: errors are the PSP
// 0x8001xxxx/0x8002xxxx codes, which are negative when read as s32. To keep
// the two apart, ops return std::int64_t and set `error` (0 = success).

struct OpResult {
    std::int64_t value = 0;
    std::uint32_t error = 0;
};

OpResult op_open(psprecomp::Runtime &runtime, const std::string &psp_path,
                 std::uint32_t flags, bool make_async) {
    const bool read_only_dev = is_read_only_device(psp_path);
    const std::uint32_t access = flags & kAccessMask;
    const bool want_write = access != kOReadOnly || (flags & (kOCreat | kOTrunc)) != 0;
    if (want_write && read_only_dev) return {0, kErrReadOnly};

    fs::path host = resolve_host_path(runtime, psp_path);
    std::error_code ec;
    const bool exists = fs::exists(host, ec);
    if (exists && fs::is_directory(host, ec)) return {0, kErrInvalidArg};
    if (!exists) {
        if ((flags & kOCreat) == 0 || read_only_dev) return {0, kErrFileNotFound};
        if (host.has_parent_path()) fs::create_directories(host.parent_path(), ec);
        std::ofstream create(host, std::ios::binary);
        if (!create) return {0, kErrFileNotFound};
        create.close();
    } else if ((flags & (kOCreat | kOExcl)) == (kOCreat | kOExcl)) {
        return {0, kErrFileExist};
    }
    if (static_cast<std::size_t>(g_io.next_id) > 0x10000u) return {0, kErrTooManyOpen};

    std::ios::openmode mode = std::ios::binary;
    const bool readable = access == kOReadOnly || access == 0x0003u;
    const bool writable = access != kOReadOnly;
    if (readable) mode |= std::ios::in;
    if (writable) mode |= std::ios::out;
    if (flags & kOTrunc) mode |= std::ios::trunc;
    if (flags & kOAppend) mode |= std::ios::app;

    OpenFile file;
    file.host_path = host;
    file.read_only_device = read_only_dev;
    file.readable = readable;
    file.writable = writable;
    file.stream.open(host, mode);
    if (!file.stream) return {0, kErrFileNotFound};

    const std::int32_t fd = g_io.next_id++;
    if (make_async) {
        file.has_async_result = true;
        file.async_result = fd;  // PSP: async open result is the fd itself
    }
    g_io.files.emplace(fd, std::move(file));
    io_log("open '%s' flags=%s -> fd %d", psp_path.c_str(),
           psprecomp::hex32(flags).c_str(), fd);
    return {fd, 0};
}

OpenFile *find_file(std::int32_t fd) {
    const auto it = g_io.files.find(fd);
    return it == g_io.files.end() ? nullptr : &it->second;
}

OpResult op_read(psprecomp::Runtime &runtime, std::int32_t fd, std::uint32_t buf_addr,
                 std::uint32_t size) {
    OpenFile *file = find_file(fd);
    if (file == nullptr || !file->readable) return {0, kErrBadF};
    if (size == 0) return {0, 0};
    std::vector<std::uint8_t> buffer(size);
    file->stream.clear();
    file->stream.read(reinterpret_cast<char *>(buffer.data()),
                      static_cast<std::streamsize>(size));
    const std::int64_t got = static_cast<std::int64_t>(file->stream.gcount());
    if (got > 0)
        write_guest_bytes(runtime.memory(), buf_addr, buffer.data(),
                          static_cast<std::size_t>(got));
    return {got, 0};
}

OpResult op_write(psprecomp::Runtime &runtime, std::int32_t fd, std::uint32_t buf_addr,
                  std::uint32_t size) {
    OpenFile *file = find_file(fd);
    if (file == nullptr || !file->writable) return {0, kErrBadF};
    std::vector<std::uint8_t> buffer(size);
    read_guest_bytes(runtime.memory(), buf_addr, buffer.data(), size);
    file->stream.clear();
    file->stream.write(reinterpret_cast<const char *>(buffer.data()),
                       static_cast<std::streamsize>(size));
    file->stream.flush();
    if (!file->stream) return {0, kErrInvalidArg};
    return {static_cast<std::int64_t>(size), 0};
}

OpResult op_lseek(std::int32_t fd, std::int64_t offset, std::uint32_t whence) {
    OpenFile *file = find_file(fd);
    if (file == nullptr) return {0, kErrBadF};
    std::error_code ec;
    const std::int64_t size = static_cast<std::int64_t>(fs::file_size(file->host_path, ec));
    if (ec) return {0, kErrInvalidArg};
    file->stream.clear();
    const std::int64_t cur = static_cast<std::int64_t>(file->stream.tellg());
    std::int64_t target = 0;
    switch (whence) {
        case 0: target = offset; break;                    // SEEK_SET
        case 1: target = (cur < 0 ? 0 : cur) + offset; break;  // SEEK_CUR
        case 2: target = size + offset; break;             // SEEK_END
        default: return {0, kErrInvalidArg};
    }
    if (target < 0) return {0, kErrInvalidArg};
    file->stream.seekg(static_cast<std::streamoff>(target), std::ios::beg);
    file->stream.seekp(static_cast<std::streamoff>(target), std::ios::beg);
    if (!file->stream) return {0, kErrInvalidArg};
    return {target, 0};
}

OpResult op_close(std::int32_t fd) {
    const auto it = g_io.files.find(fd);
    if (it != g_io.files.end()) {
        g_io.files.erase(it);
        return {0, 0};
    }
    const auto dit = g_io.dirs.find(fd);
    if (dit != g_io.dirs.end()) {
        g_io.dirs.erase(dit);
        return {0, 0};
    }
    return {0, kErrBadF};
}

// Completes an async wait/poll: consumes the stored result when present.
OpResult op_async_result(psprecomp::Runtime &runtime, std::int32_t fd,
                         std::uint32_t result_addr) {
    std::int64_t result = 0;
    if (OpenFile *file = find_file(fd)) {
        if (!file->has_async_result) return {0, kErrNoAsync};
        result = file->async_result;
        file->has_async_result = false;
    } else if (const auto it = g_io.async_tombstones.find(fd);
               it != g_io.async_tombstones.end()) {
        result = it->second;
        g_io.async_tombstones.erase(it);
    } else {
        return {0, kErrBadF};
    }
    if (result_addr != 0) store64(runtime.memory(), result_addr, result);
    return {0, 0};
}

void set_result(psprecomp::AllegrexContext &ctx, const OpResult &r) {
    if (r.error != 0) {
        ctx.set_gpr(2, r.error);
    } else {
        ctx.set_gpr(2, static_cast<std::uint32_t>(r.value & 0xFFFFFFFFu));
    }
}

void set_result64(psprecomp::AllegrexContext &ctx, const OpResult &r) {
    if (r.error != 0) {
        ctx.set_gpr(2, r.error);
        ctx.set_gpr(3, 0xFFFFFFFFu);  // sign-extended error
    } else {
        ctx.set_gpr(2, static_cast<std::uint32_t>(r.value & 0xFFFFFFFFu));
        ctx.set_gpr(3, static_cast<std::uint32_t>((r.value >> 32) & 0xFFFFFFFFu));
    }
}

std::int64_t arg_i64(const psprecomp::AllegrexContext &ctx) {
    // o32: after a0 (fd), the 64-bit argument occupies the aligned a2/a3 pair.
    const std::uint64_t v = static_cast<std::uint64_t>(ctx.gpr[6]) |
                            (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u);
    return static_cast<std::int64_t>(v);
}

// EABI: the argument after the a0-a3 block travels in $t0 (for Lseek this
// is `whence`, following the 64-bit offset in a2/a3).
std::uint32_t t0_arg(const psprecomp::AllegrexContext &ctx) { return ctx.gpr[8]; }

}  // namespace

void io_reset_state() { g_io = IoState{}; }

bool hle_io_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                     std::uint32_t nid) {
    auto &memory = runtime.memory();
    try {
        switch (nid) {
            case kNidIoOpen:
            case kNidIoOpenAsync: {
                const std::string path = read_guest_string(memory, ctx.gpr[4]);
                OpResult r = op_open(runtime, path, ctx.gpr[5], nid == kNidIoOpenAsync);
                if (r.error != 0)
                    io_log("open '%s' -> error %s", path.c_str(),
                           psprecomp::hex32(r.error).c_str());
                set_result(ctx, r);
                return true;
            }
            case kNidIoRead: {
                OpResult r = op_read(runtime, static_cast<std::int32_t>(ctx.gpr[4]),
                                     ctx.gpr[5], ctx.gpr[6]);
                if (r.error != 0)
                    io_log("read fd %u -> error %s", ctx.gpr[4],
                           psprecomp::hex32(r.error).c_str());
                set_result(ctx, r);
                return true;
            }
            case kNidIoReadAsync: {
                const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
                OpResult r = op_read(runtime, fd, ctx.gpr[5], ctx.gpr[6]);
                if (r.error == 0) {
                    if (OpenFile *file = find_file(fd)) {
                        file->has_async_result = true;
                        file->async_result = r.value;
                    }
                    set_result(ctx, {0, 0});
                } else {
                    set_result(ctx, r);
                }
                return true;
            }
            case kNidIoWrite: {
                OpResult r = op_write(runtime, static_cast<std::int32_t>(ctx.gpr[4]),
                                      ctx.gpr[5], ctx.gpr[6]);
                set_result(ctx, r);
                return true;
            }
            case kNidIoLseek: {
                OpResult r = op_lseek(static_cast<std::int32_t>(ctx.gpr[4]), arg_i64(ctx),
                                      t0_arg(ctx));
                set_result64(ctx, r);
                return true;
            }
            case kNidIoLseekAsync: {
                const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
                OpResult r = op_lseek(fd, arg_i64(ctx), t0_arg(ctx));
                if (r.error == 0) {
                    if (OpenFile *file = find_file(fd)) {
                        file->has_async_result = true;
                        file->async_result = r.value;
                    }
                    set_result(ctx, {0, 0});
                } else {
                    set_result(ctx, r);
                }
                return true;
            }
            case kNidIoClose:
                set_result(ctx, op_close(static_cast<std::int32_t>(ctx.gpr[4])));
                return true;
            case kNidIoCloseAsync: {
                const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
                if (find_file(fd) == nullptr) {
                    set_result(ctx, {0, kErrBadF});
                    return true;
                }
                OpResult r = op_close(fd);
                if (r.error == 0) g_io.async_tombstones[fd] = 0;
                set_result(ctx, r);
                return true;
            }
            case kNidIoGetstat: {
                const std::string path = read_guest_string(memory, ctx.gpr[4]);
                const fs::path host = resolve_host_path(runtime, path);
                std::error_code ec;
                if (!fs::exists(host, ec)) {
                    io_log("getstat '%s' -> not found", path.c_str());
                    set_result(ctx, {0, kErrFileNotFound});
                    return true;
                }
                std::uint8_t image[88];
                if (!build_stat(host, is_read_only_device(path), image)) {
                    set_result(ctx, {0, kErrInvalidArg});
                    return true;
                }
                write_guest_bytes(memory, ctx.gpr[5], image, sizeof(image));
                set_result(ctx, {0, 0});
                return true;
            }
            case kNidIoDopen: {
                const std::string path = read_guest_string(memory, ctx.gpr[4]);
                const fs::path host = resolve_host_path(runtime, path);
                std::error_code ec;
                if (!fs::exists(host, ec)) {
                    io_log("dopen '%s' -> not found", path.c_str());
                    set_result(ctx, {0, kErrFileNotFound});
                    return true;
                }
                if (!fs::is_directory(host, ec)) {
                    set_result(ctx, {0, kErrInvalidArg});
                    return true;
                }
                OpenDir dir;
                dir.host_path = host;
                dir.read_only_device = is_read_only_device(path);
                for (const auto &entry : fs::directory_iterator(host, ec))
                    dir.entries.push_back(entry);
                const std::int32_t fd = g_io.next_id++;
                g_io.dirs.emplace(fd, std::move(dir));
                io_log("dopen '%s' -> dh %d", path.c_str(), fd);
                set_result(ctx, {fd, 0});
                return true;
            }
            case kNidIoDread: {
                const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
                const auto it = g_io.dirs.find(fd);
                if (it == g_io.dirs.end()) {
                    set_result(ctx, {0, kErrBadF});
                    return true;
                }
                OpenDir &dir = it->second;
                fs::path entry_path;
                std::string entry_name;
                if (dir.next == 0) {
                    entry_path = dir.host_path;
                    entry_name = ".";
                } else if (dir.next == 1) {
                    entry_path = dir.host_path.parent_path();
                    entry_name = "..";
                } else if (dir.next - 2 < dir.entries.size()) {
                    entry_path = dir.entries[dir.next - 2].path();
                    entry_name = entry_path.filename().string();
                } else {
                    set_result(ctx, {0, 0});  // end of directory
                    return true;
                }
                ++dir.next;
                std::array<std::uint8_t, 348> image{};
                if (!build_stat(entry_path, dir.read_only_device, image.data())) {
                    set_result(ctx, {0, kErrInvalidArg});
                    return true;
                }
                const std::size_t name_len = std::min(entry_name.size(), std::size_t{255});
                std::memcpy(image.data() + 88, entry_name.data(), name_len);
                write_guest_bytes(memory, ctx.gpr[5], image.data(), image.size());
                set_result(ctx, {1, 0});
                return true;
            }
            case kNidIoDclose:
                set_result(ctx, op_close(static_cast<std::int32_t>(ctx.gpr[4])));
                return true;
            case kNidIoRename: {
                const std::string from = read_guest_string(memory, ctx.gpr[4]);
                const std::string to = read_guest_string(memory, ctx.gpr[5]);
                if (is_read_only_device(from) || is_read_only_device(to)) {
                    set_result(ctx, {0, kErrReadOnly});
                    return true;
                }
                const fs::path host_from = resolve_host_path(runtime, from);
                const fs::path host_to = runtime.translate_path(to);
                std::error_code ec;
                if (!fs::exists(host_from, ec)) {
                    set_result(ctx, {0, kErrFileNotFound});
                    return true;
                }
                if (host_to.has_parent_path())
                    fs::create_directories(host_to.parent_path(), ec);
                fs::rename(host_from, host_to, ec);
                set_result(ctx, ec ? OpResult{0, kErrInvalidArg} : OpResult{0, 0});
                return true;
            }
            case kNidIoDevctl: {
                const std::string dev = read_guest_string(memory, ctx.gpr[4]);
                static std::set<std::uint32_t> seen_cmds;
                if (seen_cmds.emplace(ctx.gpr[5]).second)
                    io_log("devctl '%s' cmd %s (minimal: returns 0)", dev.c_str(),
                           psprecomp::hex32(ctx.gpr[5]).c_str());
                set_result(ctx, {0, 0});
                return true;
            }
            case kNidIoPollAsync:
            case kNidIoWaitAsync:
            case kNidIoWaitAsyncCB:
                set_result(ctx, op_async_result(runtime,
                                                static_cast<std::int32_t>(ctx.gpr[4]),
                                                ctx.gpr[5]));
                return true;
            case kNidIoSync:
            case kNidIoChangeAsyncPriority:
                set_result(ctx, {0, 0});
                return true;
            default:
                return false;
        }
    } catch (const std::exception &e) {
        // translate_path throws on traversal; filesystem calls may throw too.
        io_log("io dispatch nid %s exception: %s", psprecomp::hex32(nid).c_str(), e.what());
        ctx.set_gpr(2, kErrInvalidArg);
        return true;
    }
}

}  // namespace p3p
