// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;
constexpr size_t DynamicCallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
constexpr size_t DynamicRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46200;
constexpr size_t DynamicStateOffset = sizeof(Loader::NSOHeader) + 0x4E9FE00;
constexpr size_t DynamicRoutineSize = 120;
constexpr u32 DynamicCallInstruction = 0x94D57D4F;
constexpr u32 DynamicOriginalInstruction = 0x1E201828;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word, size_t offset = PatchOffset) {
    const bool wide = offset > 0xffffff;
    std::vector<u8> ips = wide ? std::vector<u8>{'I', 'P', 'S', '3', '2'}
                               : std::vector<u8>{'P', 'A', 'T', 'C', 'H'};
    if (wide) {
        ips.push_back(static_cast<u8>(offset >> 24));
    }
    ips.push_back(static_cast<u8>(offset >> 16));
    ips.push_back(static_cast<u8>(offset >> 8));
    ips.push_back(static_cast<u8>(offset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    if (wide) {
        ips.insert(ips.end(), {'E', 'E', 'O', 'F'});
    } else {
        ips.insert(ips.end(), {'E', 'O', 'F'});
    }
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt,
                                     size_t user_mod_offset = PatchOffset) {
        Settings::values.oot_30fps.SetValue(false);
        Settings::values.oot_frame_mode.SetValue(-1);
        Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Dynarmic);
        Settings::values.cpu_clock.SetValue(Settings::CpuClock::Normal);
        Settings::values.use_multi_core.SetValue(true);
        Settings::values.sync_core_speed.SetValue(false);
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word, user_mod_offset);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        Settings::values.oot_30fps.SetValue(previous_oot_30fps);
        Settings::values.oot_frame_mode.SetValue(previous_frame_mode);
        Settings::values.cpu_backend.SetValue(previous_backend);
        Settings::values.cpu_clock.SetValue(previous_clock);
        Settings::values.use_multi_core.SetValue(previous_multicore);
        Settings::values.sync_core_speed.SetValue(previous_sync);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

private:
    const bool previous_oot_30fps = Settings::values.oot_30fps.GetValue();
    const s32 previous_frame_mode = Settings::values.oot_frame_mode.GetValue();
    const Settings::CpuBackend previous_backend = Settings::values.cpu_backend.GetValue();
    const Settings::CpuClock previous_clock = Settings::values.cpu_clock.GetValue();
    const bool previous_multicore = Settings::values.use_multi_core.GetValue();
    const bool previous_sync = Settings::values.sync_core_speed.GetValue();
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset = PatchOffset) {
    REQUIRE(nso.size() >= offset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + offset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

constexpr std::string_view Fps30 = "30 FPS (built-in)";
struct Oot30Build {
    FileSys::PatchManager::BuildID id;
    std::array<size_t, 3> offsets;
};
constexpr std::array Oot30Builds{
    Oot30Build{OcarinaBuildId, {0x3b4f004, 0x3b4f370, 0x3b4f37c}},
    Oot30Build{{0x33, 0xFA, 0x6C, 0x2C, 0x23, 0x84, 0x82, 0x82, 0xA7, 0x7E,
               0x8B, 0x88, 0xA0, 0xA9, 0x3F, 0x48, 0x87, 0xC5, 0x59, 0x20},
              {0x3af1994, 0x3af1d00, 0x3af1d0c}},
};
constexpr std::array<u32, 3> Oot30Originals{0x1a89b10a, 0x52800788, 0x1ac10d08};
constexpr std::array<u32, 3> Oot30Replacements{0x5280004a, 0x52800041, 0x528003c8};

std::vector<u8> MakeOot30Main(const Oot30Build& build) {
    auto nso = MakeMain(build.id, TbzInstruction);
    nso.resize(sizeof(Loader::NSOHeader) + build.offsets.back() + sizeof(u32));
    if (build.id == OcarinaBuildId) {
        std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction, sizeof(u32));
    }
    for (size_t i = 0; i < build.offsets.size(); ++i) {
        std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + build.offsets[i],
                    &Oot30Originals[i], sizeof(u32));
    }
    return nso;
}

void CheckOot30Words(const std::vector<u8>& nso, const Oot30Build& build,
                     const std::array<u32, 3>& expected) {
    for (size_t i = 0; i < build.offsets.size(); ++i) {
        CAPTURE(i);
        u32 word;
        std::memcpy(&word, nso.data() + sizeof(Loader::NSOHeader) + build.offsets[i], sizeof(word));
        REQUIRE(word == expected[i]);
    }
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

TEST_CASE("Ocarina 30 FPS patches both known builds only when requested", "[patch_manager][oot_30fps]") {
    PatchManagerTestContext context;
    REQUIRE_FALSE(Settings::values.oot_30fps.GetDefault());
    for (const auto& build : Oot30Builds) {
        const auto nso = MakeOot30Main(build);
        Settings::values.oot_30fps.SetValue(false);
        CheckOot30Words(context.Manager().PatchNSO(nso, "main"), build, Oot30Originals);
        Settings::values.oot_30fps.SetValue(true);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        CheckOot30Words(patched, build, Oot30Replacements);
        CheckOot30Words(nso, build, Oot30Originals);
        if (build.id == OcarinaBuildId) {
            REQUIRE(WordAt(patched) == BranchInstruction); // Max Resolution still applies.
        }
        CheckOot30Words(context.Manager().PatchNSO(nso, "sdk"), build, Oot30Originals);
        CheckOot30Words(context.Manager(OtherTitleId).PatchNSO(nso, "main"), build, Oot30Originals);
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{Fps30}};
        CheckOot30Words(context.Manager().PatchNSO(nso, "main"), build, Oot30Originals);
        Settings::values.disabled_addons.erase(OcarinaTitleId);
    }
}

TEST_CASE("Ocarina 30 FPS checks the entire build ID and instruction group", "[patch_manager][oot_30fps]") {
    PatchManagerTestContext context;
    Settings::values.oot_30fps.SetValue(true);
    for (const auto& build : Oot30Builds) {
        for (size_t bad_word = 0; bad_word < 3; ++bad_word) {
            auto nso = MakeOot30Main(build);
            auto expected = Oot30Originals;
            expected[bad_word] = NopInstruction;
            std::memcpy(nso.data() + sizeof(Loader::NSOHeader) + build.offsets[bad_word],
                        &NopInstruction, sizeof(u32));
            CheckOot30Words(context.Manager().PatchNSO(nso, "main"), build, expected);
        }
        for (const size_t changed_byte : {size_t{0}, size_t{19}, size_t{31}}) {
            auto other = build;
            other.id[changed_byte] ^= 1;
            CheckOot30Words(context.Manager().PatchNSO(MakeOot30Main(other), "main"),
                            other, Oot30Originals);
        }
        auto truncated = MakeOot30Main(build);
        truncated.resize(truncated.size() - sizeof(u32));
        const auto patched = context.Manager().PatchNSO(truncated, "main");
        for (size_t i = 0; i < 2; ++i) {
            u32 word;
            std::memcpy(&word, patched.data() + sizeof(Loader::NSOHeader) + build.offsets[i], sizeof(word));
            REQUIRE(word == Oot30Originals[i]);
        }
    }
}

TEST_CASE("Ocarina 30 FPS does not partially combine with an external patch", "[patch_manager][oot_30fps]") {
    const auto& build = Oot30Builds.front();
    for (const u32 mod_word : {NopInstruction, Oot30Replacements.back()}) {
        PatchManagerTestContext context{mod_word, sizeof(Loader::NSOHeader) + build.offsets.back()};
        Settings::values.oot_30fps.SetValue(true);
        auto expected = Oot30Originals;
        expected.back() = mod_word;
        CheckOot30Words(context.Manager().PatchNSO(MakeOot30Main(build), "main"), build, expected);
    }
}

TEST_CASE("Ocarina 30 FPS is listed once with its effective enable state", "[patch_manager][oot_30fps]") {
    PatchManagerTestContext context;
    const auto enabled = [&] {
        const auto patches = context.Manager().GetPatches();
        REQUIRE(std::ranges::count(patches, Fps30, &FileSys::Patch::name) == 1);
        return std::ranges::find(patches, Fps30, &FileSys::Patch::name)->enabled;
    };
    REQUIRE_FALSE(enabled());
    Settings::values.oot_30fps.SetValue(true);
    REQUIRE(enabled());
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{Fps30}};
    REQUIRE_FALSE(enabled());
    const auto other_patches = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::count(other_patches, Fps30, &FileSys::Patch::name) == 0);
}

namespace {

std::vector<u8> MakeDynamicMain() {
    auto nso = MakeOot30Main(Oot30Builds[0]);
    nso.resize(DynamicStateOffset + 16);
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction,
                sizeof(DynamicOriginalInstruction));
    return nso;
}

} // Anonymous namespace

TEST_CASE("Ocarina Dynamic FPS is opt-in and can be disabled in Add-Ons",
          "[patch_manager][oot_dynamic_fps]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    const auto enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, DynamicFps, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE_FALSE(enabled());
    auto nso = MakeDynamicMain();
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicCallOffset) == DynamicOriginalInstruction);
    Settings::values.oot_frame_mode.SetValue(2);
    REQUIRE(enabled());
    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0x1E201828);
    REQUIRE(WordAt(patched, DynamicRoutineOffset + 116) == 0x3F83D70A);
    REQUIRE(
        Common::HexToString(std::span{patched}.subspan(DynamicRoutineOffset, DynamicRoutineSize)) ==
        "2818201ECA9200B04A0138912BE03BD54C0140F94B0100F9CC0200B46B010CCB0DE03BD5"
        "8E0280D2AE09CE9A7F010EEBCB818B9A4C0540F94C0000B5EC030BAA6E010CCB8C0D8E8B"
        "4C0500F98201239EA301239E4218231E4208211EC300001C0309231E4020231E4D000054"
        "4840201EC0035FD60AD7833F");
    REQUIRE(WordAt(patched) == BranchInstruction);
    CheckOot30Words(patched, Oot30Builds[0], Oot30Originals);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
    REQUIRE_FALSE(enabled());
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicCallOffset) == DynamicOriginalInstruction);
    REQUIRE(WordAt(manager.PatchNSO(nso, "main"), DynamicRoutineOffset) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE(enabled());
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == TbzInstruction);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, DynamicFps, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("Built-in Dynamic FPS rejects mismatched or truncated modules transactionally",
          "[patch_manager][oot_dynamic_fps]") {
    PatchManagerTestContext context;
    Settings::values.oot_frame_mode.SetValue(2);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    std::string module = "main";
    u64 title = OcarinaTitleId;
    SECTION("Wrong build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Nonzero build ID suffix") {
        nso[offsetof(Loader::NSOHeader, build_id) + 31] = 1;
    }
    SECTION("Base build is unsupported") {
        std::memcpy(nso.data() + offsetof(Loader::NSOHeader, build_id), Oot30Builds[1].id.data(),
                    Oot30Builds[1].id.size());
    }
    SECTION("Fixed-30 controller already patched") {
        nso[sizeof(Loader::NSOHeader) + Oot30Builds[0].offsets[0]] ^= 1;
    }
    SECTION("Wrong module") {
        module = "sdk";
    }
    SECTION("Wrong title") {
        title = OtherTitleId;
    }
    SECTION("Incomplete header") {
        nso.resize(sizeof(Loader::NSOHeader) - 1);
    }
    SECTION("Incomplete call") {
        nso.resize(DynamicCallOffset + 3);
    }
    SECTION("Incomplete routine") {
        nso.resize(DynamicRoutineOffset + DynamicRoutineSize - 1);
    }
    SECTION("Incomplete timer state") {
        nso.resize(DynamicStateOffset + 15);
    }
    SECTION("Timer state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 15;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Wrong original hook") {
        nso[DynamicCallOffset] ^= 1;
    }
    SECTION("Nonzero code cave") {
        nso[DynamicRoutineOffset + DynamicRoutineSize - 1] = 1;
    }
    SECTION("Nonzero timer state") {
        nso[DynamicStateOffset + 15] = 1;
    }
    SECTION("Code cave outside executable padding") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0x3C46000;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Code cave overlaps read-only segment") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[1].location = 0x3C46200;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(context.Manager(title).PatchNSO(nso, module) == nso);
}

TEST_CASE("Built-in Dynamic FPS leaves both records alone when a user mod changes either",
          "[patch_manager][oot_dynamic_fps]") {
    size_t offset = DynamicCallOffset;
    u32 mod_word = 0x11223344;
    SECTION("User call takes precedence") {}
    SECTION("Identical call still takes precedence for the whole patch") {
        mod_word = DynamicCallInstruction;
    }
    SECTION("User routine takes precedence") {
        offset = DynamicRoutineOffset;
    }
    SECTION("Last routine byte takes precedence") {
        offset = DynamicRoutineOffset + DynamicRoutineSize - sizeof(u32);
    }
    SECTION("External fixed-30 controller takes precedence") {
        offset = sizeof(Loader::NSOHeader) + Oot30Builds[0].offsets[0];
        mod_word = Oot30Replacements[0];
    }
    SECTION("External fixed-30 ForceInterval takes precedence") {
        offset = sizeof(Loader::NSOHeader) + Oot30Builds[0].offsets[1];
        mod_word = Oot30Replacements[1];
    }
    SECTION("External fixed-30 step takes precedence") {
        offset = sizeof(Loader::NSOHeader) + Oot30Builds[0].offsets[2];
        mod_word = Oot30Replacements[2];
    }
    SECTION("User timer state takes precedence") {
        offset = DynamicStateOffset;
    }
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.oot_frame_mode.SetValue(2);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    REQUIRE(context.Manager().PatchNSO(nso, "main") == expected);
}

TEST_CASE("Ocarina frame mode overrides the legacy bool without combining timing patches",
          "[patch_manager][oot_dynamic_fps]") {
    PatchManagerTestContext context;
    REQUIRE(Settings::values.oot_frame_mode.GetDefault() == -1);
    const auto nso = MakeDynamicMain();
    for (const bool legacy_30 : {false, true}) {
        Settings::values.oot_30fps.SetValue(legacy_30);
        for (const s32 mode : {-1, 0, 1, 2}) {
            CAPTURE(legacy_30, mode);
            // Exercise the same string conversion used by the generic INI reader.
            Settings::values.oot_frame_mode.LoadString(std::to_string(mode));
            REQUIRE(Settings::values.oot_frame_mode.ToString() == std::to_string(mode));
            const bool fixed_30 = mode == 1 || (mode == -1 && legacy_30);
            const auto patched = context.Manager().PatchNSO(nso, "main");
            CheckOot30Words(patched, Oot30Builds[0], fixed_30 ? Oot30Replacements : Oot30Originals);
            REQUIRE(WordAt(patched, DynamicCallOffset) ==
                    (mode == 2 ? DynamicCallInstruction : DynamicOriginalInstruction));
            REQUIRE(WordAt(patched, DynamicRoutineOffset) == (mode == 2 ? 0x1E201828U : 0U));
            const auto patches = context.Manager().GetPatches();
            REQUIRE(std::ranges::find(patches, Fps30, &FileSys::Patch::name)->enabled == fixed_30);
            REQUIRE(std::ranges::find(patches, DynamicFps, &FileSys::Patch::name)->enabled ==
                    (mode == 2));
        }
    }
}

TEST_CASE("Ocarina fixed 30 defers to an external dynamic time-step hook",
          "[patch_manager][oot_30fps][oot_dynamic_fps]") {
    PatchManagerTestContext context{DynamicCallInstruction, DynamicCallOffset};
    SECTION("Explicit fixed 30") {
        Settings::values.oot_frame_mode.SetValue(1);
    }
    SECTION("Legacy fixed 30") {
        Settings::values.oot_30fps.SetValue(true);
    }
    const auto patched = context.Manager().PatchNSO(MakeDynamicMain(), "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0);
    CheckOot30Words(patched, Oot30Builds[0], Oot30Originals);
}

TEST_CASE("Ocarina Dynamic FPS rejects scaled or simulated clocks without changing settings",
          "[patch_manager][oot_dynamic_fps]") {
    PatchManagerTestContext context;
    Settings::values.oot_frame_mode.SetValue(2);
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    SECTION("CPU Boost") {
        Settings::values.cpu_clock.SetValue(Settings::CpuClock::Boost);
    }
    SECTION("CPU Overclock") {
        Settings::values.cpu_clock.SetValue(Settings::CpuClock::Overclock);
    }
    SECTION("Core speed synchronization") {
        Settings::values.sync_core_speed.SetValue(true);
    }
    SECTION("Single core timing") {
        Settings::values.use_multi_core.SetValue(false);
    }
#ifdef HAS_NCE
    SECTION("NCE backend") {
        Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Nce);
    }
#endif
    const auto clock = Settings::values.cpu_clock.GetValue();
    const auto sync = Settings::values.sync_core_speed.GetValue();
    const auto multicore = Settings::values.use_multi_core.GetValue();
    const auto backend = Settings::values.cpu_backend.GetValue();
    const auto nso = MakeDynamicMain();
    REQUIRE(context.Manager().PatchNSO(nso, "main") == nso);
    REQUIRE(Settings::values.cpu_clock.GetValue() == clock);
    REQUIRE(Settings::values.sync_core_speed.GetValue() == sync);
    REQUIRE(Settings::values.use_multi_core.GetValue() == multicore);
    REQUIRE(Settings::values.cpu_backend.GetValue() == backend);
}
