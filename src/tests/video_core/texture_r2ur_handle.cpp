// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;

class HandleEnvironment final : public Environment {
public:
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 0);
        REQUIRE(offset == 0x10); // never the stale word at 0x120
        return 0x1234;
    }
    TextureType ReadTextureType(u32) override { return TextureType::ColorArray2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};
} // Anonymous namespace

// The lowered R2UR keeps the uniform register's old value unless VoteAny(predicate) is true; with
// PT that select must fold to the broadcast, or the texture pass takes the old value (Ocarina of
// Time's handle table address in c0[0x120]) as the bindless handle.
TEST_CASE("A texture handle written by an unpredicated R2UR ignores the register's old value",
          "[video_core][texture]") {
    HandleEnvironment env;
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    IR::IREmitter ir{block};
    const IR::U32 stale{ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x120U))};
    const IR::U32 broadcast{ir.GetCbuf(ir.Imm32(0U), ir.Imm32(0x10U))};
    const IR::U32 handle{ir.Select(ir.VoteAny(ir.Imm1(true)), broadcast, stale)};
    IR::TextureInstInfo info{};
    info.type.Assign(TextureType::ColorArray2D);
    const IR::Value coords{ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f), ir.Imm32(0.0f))};
    (void)ir.ImageSampleImplicitLod(handle, coords, {}, {}, {}, info);

    Optimization::ConstantPropagationPass(env, program);
    HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Optimization::TexturePass(env, program, host_info);
    const auto& descriptors = program.info.texture_descriptors;
    REQUIRE(descriptors.size() == 1);
    REQUIRE(descriptors[0].cbuf_index == 0);
    REQUIRE(descriptors[0].cbuf_offset == 0x10);
}
