// Unit tests for the P3P IoFileMgrForUser HLE (Gate 5).
//
// Drives hle_io_dispatch directly against a fixture game-root tree built in
// a temporary directory: path translation, open/read/seek/getstat, the
// case-insensitive fallback, directory enumeration, the async family
// (synchronous completion), ms0 writes + rename, and read-only enforcement
// on disc0 paths.

#include "hle/p3p_hle_io.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

constexpr std::uint32_t kStrAddr = 0x09000100u;
constexpr std::uint32_t kStr2Addr = 0x09000200u;
constexpr std::uint32_t kBufAddr = 0x09100000u;
constexpr std::uint32_t kStatAddr = 0x09200000u;
constexpr std::uint32_t kDirentAddr = 0x09300000u;
constexpr std::uint32_t kResultAddr = 0x09400000u;

psprecomp::Runtime *g_runtime = nullptr;

void put_str(std::uint32_t addr, const std::string &s) {
    for (std::size_t i = 0; i <= s.size(); ++i)
        g_runtime->memory().store8(addr + static_cast<std::uint32_t>(i),
                                   static_cast<std::uint8_t>(s.c_str()[i]));
}

std::uint32_t call_io(std::uint32_t nid) {
    auto &ctx = g_runtime->cpu();
    p3p::hle_io_dispatch(*g_runtime, ctx, nid);
    return ctx.gpr[2];
}

std::uint32_t call_io1(std::uint32_t nid, std::uint32_t a0) {
    auto &ctx = g_runtime->cpu();
    ctx.gpr[4] = a0;
    return call_io(nid);
}

std::int32_t open_path(const std::string &path, std::uint32_t flags, bool async = false) {
    auto &ctx = g_runtime->cpu();
    put_str(kStrAddr, path);
    ctx.gpr[4] = kStrAddr;
    ctx.gpr[5] = flags;
    ctx.gpr[6] = 0777;
    return static_cast<std::int32_t>(
        call_io(async ? p3p::kNidIoOpenAsync : p3p::kNidIoOpen));
}

std::uint32_t guest_u32(std::uint32_t addr) { return g_runtime->memory().load32(addr); }
std::uint64_t guest_u64(std::uint32_t addr) {
    return static_cast<std::uint64_t>(guest_u32(addr)) |
           (static_cast<std::uint64_t>(guest_u32(addr + 4u)) << 32u);
}

bool guest_bytes_eq(std::uint32_t addr, const std::string &expected) {
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (g_runtime->memory().load8(addr + static_cast<std::uint32_t>(i)) !=
            static_cast<std::uint8_t>(expected[i]))
            return false;
    return true;
}

void write_file(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "p3p_io_test_root";
    fs::remove_all(root);
    write_file(root / "PSP_GAME/USRDIR/DATA/test.bin", "HELLO-PSP-WORLD");
    write_file(root / "PSP_GAME/USRDIR/sound/bgm.bin", "AUDIO!");
    write_file(root / "PSP_GAME/USRDIR/umd0.cpk", std::string(1024, 'C'));

    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    g_runtime = &runtime;
    runtime.set_game_root(root);
    p3p::io_reset_state();
    auto &ctx = runtime.cpu();
    ctx.gpr[29] = 0x09FFF000u;  // $sp: stack args (lseek whence) live at sp+16

    // --- Open + read + seek + close --------------------------------------
    const std::int32_t fd = open_path("disc0:/PSP_GAME/USRDIR/DATA/test.bin", 0x0001);
    check(fd >= 3, "open existing disc0 file returns fd");
    ctx.gpr[4] = static_cast<std::uint32_t>(fd);
    ctx.gpr[5] = kBufAddr;
    ctx.gpr[6] = 5;
    check(call_io(p3p::kNidIoRead) == 5u, "read returns byte count");
    check(guest_bytes_eq(kBufAddr, "HELLO"), "read bytes match file head");
    // Lseek SET to 6 -> "PSP-WORLD" remains.
    ctx.gpr[4] = static_cast<std::uint32_t>(fd);
    ctx.gpr[6] = 6;
    ctx.gpr[7] = 0;
    ctx.gpr[8] = 0u;  // whence SEEK_SET (EABI $t0)
    check(call_io(p3p::kNidIoLseek) == 6u && ctx.gpr[3] == 0u, "lseek SET returns pos");
    ctx.gpr[4] = static_cast<std::uint32_t>(fd);
    ctx.gpr[5] = kBufAddr;
    ctx.gpr[6] = 9;
    check(call_io(p3p::kNidIoRead) == 9u, "read after seek count");
    check(guest_bytes_eq(kBufAddr, "PSP-WORLD"), "read after seek bytes");
    // Lseek END -4.
    ctx.gpr[4] = static_cast<std::uint32_t>(fd);
    ctx.gpr[6] = 0xFFFFFFFCu;  // -4 low
    ctx.gpr[7] = 0xFFFFFFFFu;  // -4 high
    ctx.gpr[8] = 2u;  // whence SEEK_END (EABI $t0)
    check(call_io(p3p::kNidIoLseek) == 11u, "lseek END-4 returns 15-4=11");
    check(call_io1(p3p::kNidIoClose, static_cast<std::uint32_t>(fd)) == 0u, "close ok");
    ctx.gpr[4] = static_cast<std::uint32_t>(fd);
    ctx.gpr[5] = kBufAddr;
    ctx.gpr[6] = 1;
    check(call_io(p3p::kNidIoRead) == 0x80020323u, "read after close is BADF");

    // --- Missing file / traversal ----------------------------------------
    check(open_path("disc0:/PSP_GAME/USRDIR/nope.bin", 0x0001) ==
              static_cast<std::int32_t>(0x80010002u),
          "open missing file is FILE_NOT_FOUND");
    check(open_path("disc0:/PSP_GAME/../secret", 0x0001) ==
              static_cast<std::int32_t>(0x80010016u),
          "path traversal rejected as EINVAL");

    // --- Case-insensitive fallback ---------------------------------------
    const std::int32_t fd2 = open_path("disc0:/PSP_GAME/USRDIR/SOUND/BGM.BIN", 0x0001);
    check(fd2 >= 3, "case-insensitive open succeeds");
    if (fd2 >= 3) {
        ctx.gpr[4] = static_cast<std::uint32_t>(fd2);
        ctx.gpr[5] = kBufAddr;
        ctx.gpr[6] = 6;
        check(call_io(p3p::kNidIoRead) == 6u && guest_bytes_eq(kBufAddr, "AUDIO!"),
              "case-insensitive read bytes");
        call_io1(p3p::kNidIoClose, static_cast<std::uint32_t>(fd2));
    }

    // --- Getstat ----------------------------------------------------------
    put_str(kStrAddr, "disc0:/PSP_GAME/USRDIR/umd0.cpk");
    ctx.gpr[4] = kStrAddr;
    ctx.gpr[5] = kStatAddr;
    check(call_io(p3p::kNidIoGetstat) == 0u, "getstat ok");
    check(guest_u64(kStatAddr + 8u) == 1024u, "getstat size field");
    check((guest_u32(kStatAddr) & 0xF000u) == 0x2000u, "getstat mode is IFREG");
    put_str(kStrAddr, "disc0:/PSP_GAME/USRDIR");
    ctx.gpr[4] = kStrAddr;
    ctx.gpr[5] = kStatAddr;
    check(call_io(p3p::kNidIoGetstat) == 0u &&
              (guest_u32(kStatAddr) & 0xF000u) == 0x1000u,
          "getstat dir mode is IFDIR");

    // --- Directory listing -------------------------------------------------
    put_str(kStrAddr, "disc0:/PSP_GAME/USRDIR");
    ctx.gpr[4] = kStrAddr;
    const std::int32_t dir_handle = static_cast<std::int32_t>(call_io(p3p::kNidIoDopen));
    check(dir_handle >= 3, "dopen returns handle");
    std::string names;
    for (int i = 0; i < 10; ++i) {
        ctx.gpr[4] = static_cast<std::uint32_t>(dir_handle);
        ctx.gpr[5] = kDirentAddr;
        const std::uint32_t r = call_io(p3p::kNidIoDread);
        if (r == 0u) break;
        check(r == 1u, "dread returns 1 while entries remain");
        std::string name;
        for (int j = 0; j < 256; ++j) {
            const char c =
                static_cast<char>(g_runtime->memory().load8(kDirentAddr + 88u + j));
            if (c == '\0') break;
            name.push_back(c);
        }
        names += name + ";";
    }
    check(names.find("umd0.cpk;") != std::string::npos, "dread lists umd0.cpk");
    check(names.find("DATA;") != std::string::npos, "dread lists DATA dir");
    check(names.rfind(".;", 0) == 0, "dread first entry is .");
    check(call_io1(p3p::kNidIoDclose, static_cast<std::uint32_t>(dir_handle)) == 0u,
          "dclose ok");

    // --- Async family (synchronous completion) -----------------------------
    const std::int32_t afd = open_path("disc0:/PSP_GAME/USRDIR/DATA/test.bin", 0x0001,
                                       /*async=*/true);
    check(afd >= 3, "open async returns fd");
    ctx.gpr[4] = static_cast<std::uint32_t>(afd);
    ctx.gpr[5] = kResultAddr;
    check(call_io(p3p::kNidIoPollAsync) == 0u, "poll async completes");
    check(guest_u64(kResultAddr) == static_cast<std::uint64_t>(afd),
          "open async result is fd");
    ctx.gpr[4] = static_cast<std::uint32_t>(afd);
    ctx.gpr[5] = kBufAddr;
    ctx.gpr[6] = 5;
    check(call_io(p3p::kNidIoReadAsync) == 0u, "read async queues ok");
    ctx.gpr[4] = static_cast<std::uint32_t>(afd);
    ctx.gpr[5] = kResultAddr;
    check(call_io(p3p::kNidIoWaitAsync) == 0u, "wait async completes");
    check(guest_u64(kResultAddr) == 5u, "read async result is byte count");
    check(guest_bytes_eq(kBufAddr, "HELLO"), "read async bytes landed");
    ctx.gpr[4] = static_cast<std::uint32_t>(afd);
    ctx.gpr[5] = kResultAddr;
    check(call_io(p3p::kNidIoPollAsync) == 0x8002032Au,
          "poll with no pending async is NOASYNC");
    check(call_io1(p3p::kNidIoCloseAsync, static_cast<std::uint32_t>(afd)) == 0u,
          "close async ok");
    ctx.gpr[4] = static_cast<std::uint32_t>(afd);
    ctx.gpr[5] = kResultAddr;
    check(call_io(p3p::kNidIoWaitAsyncCB) == 0u && guest_u64(kResultAddr) == 0u,
          "wait after close async returns 0");

    // --- ms0 write + rename + read-only enforcement ------------------------
    const std::int32_t wfd =
        open_path("ms0:/PSP/SAVEDATA/ULES01523-INST00/save.bin", 0x0002 | 0x0200);
    check(wfd >= 3, "ms0 write open with CREAT creates dirs+file");
    if (wfd >= 3) {
        put_str(kBufAddr, "SAVE-DATA-42");
        ctx.gpr[4] = static_cast<std::uint32_t>(wfd);
        ctx.gpr[5] = kBufAddr;
        ctx.gpr[6] = 12;
        check(call_io(p3p::kNidIoWrite) == 12u, "write returns count");
        call_io1(p3p::kNidIoClose, static_cast<std::uint32_t>(wfd));
        check(fs::exists(root / "PSP/SAVEDATA/ULES01523-INST00/save.bin"),
              "ms0 file exists on host");
        // Rename it, then read back through the new name.
        put_str(kStrAddr, "ms0:/PSP/SAVEDATA/ULES01523-INST00/save.bin");
        put_str(kStr2Addr, "ms0:/PSP/SAVEDATA/ULES01523-INST00/save2.bin");
        ctx.gpr[4] = kStrAddr;
        ctx.gpr[5] = kStr2Addr;
        check(call_io(p3p::kNidIoRename) == 0u, "rename ok");
        const std::int32_t rfd =
            open_path("ms0:/PSP/SAVEDATA/ULES01523-INST00/save2.bin", 0x0001);
        check(rfd >= 3, "reopen renamed file");
        if (rfd >= 3) {
            ctx.gpr[4] = static_cast<std::uint32_t>(rfd);
            ctx.gpr[5] = kBufAddr;
            ctx.gpr[6] = 12;
            check(call_io(p3p::kNidIoRead) == 12u &&
                      guest_bytes_eq(kBufAddr, "SAVE-DATA-42"),
                  "renamed file content round-trips");
            call_io1(p3p::kNidIoClose, static_cast<std::uint32_t>(rfd));
        }
    }
    check(open_path("disc0:/PSP_GAME/USRDIR/evil.bin", 0x0002 | 0x0200) ==
              static_cast<std::int32_t>(0x8001001Eu),
          "write open on disc0 is READ_ONLY");

    fs::remove_all(root);
    std::fprintf(stderr, "test_p3p_io: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
