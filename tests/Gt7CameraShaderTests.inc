// Included inside shaderCfgTests.cpp's test namespace; exercises actual native IR metadata.
namespace Gt7CameraShaderTest {
namespace IR = ShaderRecompiler::IR;

struct Fixture {
    IR::Program program;
    IR::Block* block = nullptr;
    IR::Value address;
    std::array<IR::Value, 16> matrix {};
    std::array<IR::Inst*, 16> loads {};
    std::array<IR::Value, 4> source {};
    std::array<IR::Value, 4> output {};
    IR::Inst* exported = nullptr;

    IR::Value Emit(IR::ValueOpcode opcode, std::initializer_list<IR::Value> args = {}) {
        return IR::Value(&block->AppendNewInst(opcode, args));
    }

    explicit Fixture(unsigned mode = 0, uint32_t camera_reg = 10) {
        using namespace IR;
        program.stage = ShaderType::Vertex;
        program.user_data_count = camera_reg + 2;
        program.block_storage.push_back(std::make_unique<Block>());
        block = program.block_storage.back().get();
        program.blocks.push_back(block);
        program.block_info.push_back({.id = 0, .terminator = {.kind = ShaderRecompiler::CFG::TerminatorKind::Return}});
        const auto low = Emit(ValueOpcode::GetUserData, {Value(ScalarReg(camera_reg))});
        const auto high = Emit(ValueOpcode::GetUserData, {Value(ScalarReg(camera_reg + 1))});
        address = Emit(ValueOpcode::GetAddressResource, {low, high});
        for (uint32_t i = 0; i != 16; ++i) {
            program.memory_info.push_back({.kind = ResourceKind::ScalarAddress,
                                           .offset = (i % 4) * 4});
            const auto load = Emit(ValueOpcode::LoadAddressU32,
                {address, Value(80u + (i / 4) * 16), Value(0u), Value(true)});
            loads[i] = load.TryInstruction();
            loads[i]->SetFlags(MemoryFlags{.index = i});
            matrix[i] = Emit(ValueOpcode::BitCastF32U32, {load});
        }
        for (uint32_t i = 0; i != 4; ++i) {
            const auto attr = Emit(ValueOpcode::GetAttribute, {Value(0u), Value(i)});
            source[i] = Emit(ValueOpcode::BitCastF32U32, {attr});
        }
        if (mode == 3) source[3] = Value::F32(1.0f);
        for (uint32_t column = 0; column != 4; ++column) {
            auto term = [&](uint32_t row) {
                return Emit(ValueOpcode::FPMul32, {source[row], matrix[4 * row + column]});
            };
            Value dot;
            if (mode == 1) {
                dot = Emit(ValueOpcode::FPAdd32,
                    {Emit(ValueOpcode::FPAdd32, {term(2), term(0)}),
                     Emit(ValueOpcode::FPAdd32, {term(3), term(1)})});
            } else {
                dot = mode == 3 ? matrix[12 + column] : term(3);
                for (uint32_t row = 0; row != 3; ++row)
                    dot = Emit(mode == 2 ? ValueOpcode::FPMad32 : ValueOpcode::FPFma32,
                        {matrix[4 * row + column], source[row], dot});
            }
            output[column] = dot;
        }
        if (mode == 4) {
            std::array<Value, 16> model;
            for (uint32_t i = 0; i != 16; ++i)
                model[i] = Emit(ValueOpcode::BitCastF32U32,
                    {Emit(ValueOpcode::GetAttribute, {Value(1u), Value(i)})});
            for (uint32_t column = 0; column != 4; ++column) {
                auto dot = Value::F32(0.0f);
                for (uint32_t input = 0; input != 4; ++input) {
                    auto combined = Value::F32(0.0f);
                    for (uint32_t row = 0; row != 4; ++row)
                        combined = Emit(ValueOpcode::FPMad32,
                            {model[input * 4 + row], matrix[4 * row + column], combined});
                    dot = Emit(ValueOpcode::FPFma32, {source[input], combined, dot});
                }
                output[column] = dot;
            }
        }
        Export();
    }

    void Export() {
        using namespace IR;
        std::array<Value, 4> bits;
        for (uint32_t column = 0; column != 4; ++column)
            bits[column] = Emit(ValueOpcode::BitCastU32F32, {output[column]});
        const auto vector = Emit(ValueOpcode::CompositeConstructU32x4,
            {bits[0], bits[1], bits[2], bits[3]});
        program.export_info.push_back({.kind = ExportTargetKind::Position, .en = 0xf});
        exported = Emit(ValueOpcode::SetAttribute, {vector, Value(true)}).TryInstruction();
        exported->SetFlags(ExportFlags{.index = uint32_t(program.export_info.size() - 1)});
    }

    bool Matches() const { return IR::HasGt7CameraPositionContract(program); }
};

} // namespace Gt7CameraShaderTest

// Local diagnostic only: guest binaries stay outside the source tree and test fixtures.
// Uses the production translation pass (no GPU or guest address-space reads).
int ProbeGt7CameraShader(int argc, char** argv) {
    const auto number = [](const char* text, uint32_t limit, uint32_t& out) {
        char* end = nullptr;
        const auto value = std::strtoul(text, &end, 10);
        if (*text == '\0' || *end != '\0' || value > limit) return false;
        out = static_cast<uint32_t>(value);
        return true;
    };
    uint32_t base = 0, count = 0, wave = 32;
    if (!number(argv[3], 127, base) || !number(argv[4], 128 - base, count) ||
        (argc == 6 && !number(argv[5], 64, wave)) || (wave != 32 && wave != 64)) return 2;
    std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
    if (!file) return 2;
    const auto size = file.tellg();
    if (size <= 0 || size > (16 << 20) || (size % 4) != 0) return 2;
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) return 2;
    std::vector<uint32_t> user_data(count);
    auto options = MakeCompileOptions(ShaderType::Vertex);
    options.non_fatal = true;
    options.bindless_images = true;
    options.user_data_base = base;
    options.user_data = user_data;
    options.wave_size = wave;
    options.dump_ir = std::getenv("KYTY_TEST_SHADER_LOG") != nullptr;
    options.early_dump = options.dump_ir;
    auto result = ShaderRecompiler::TranslateProgram(code, options);
    const auto& contract = result.program.position_matrix;
    std::printf("GT7_CAMERA_PROBE proof=%u unsupported=%u blocks=%zu typed_kind=%u typed_sgpr=%u typed_offset=%u\n",
                result.program.gt7_camera_position ? 1u : 0u,
                result.unsupported ? 1u : 0u, result.program.blocks.size(),
                static_cast<unsigned>(contract.kind), contract.user_sgpr, contract.byte_offset);
    return result.unsupported ? 3 : 0;
}

void TestPositionMatrixContract() {
    using namespace ShaderRecompiler::IR;
    using Gt7CameraShaderTest::Fixture;
    using Kind = ShaderRecompiler::CFG::TerminatorKind;
    const auto matches = [](const Fixture& f, uint32_t reg = 12) {
        const auto proof = ProvePositionMatrixContract(f.program);
        return proof.kind == PositionMatrixKind::UserDataRowMajor4x4V1 &&
            proof.user_sgpr == reg && proof.byte_offset == 80;
    };
    const auto missing = [](const Fixture& f) {
        return ProvePositionMatrixContract(f.program).kind == PositionMatrixKind::None;
    };
    const auto before_output = [](Fixture& f, ValueOpcode op, std::initializer_list<Value> args) {
        const auto* vector = f.exported->Arg(0).TryInstruction();
        const auto at = std::find_if(f.block->begin(), f.block->end(), [&](const Inst& i) { return &i == vector; });
        return Value(&*f.block->PrependNewInst(at, op, args));
    };
    const auto multiply_all = [&](Fixture& f, Value factor) {
        for (unsigned column = 0; column < 4; ++column) {
            const auto value = before_output(f, ValueOpcode::FPMul32, {f.output[column], factor});
            const auto bits = before_output(f, ValueOpcode::BitCastU32F32, {value});
            f.exported->Arg(0).TryInstruction()->SetArg(column, bits);
        }
    };
    const auto diamond = [](Fixture& f) {
        std::array<Block*, 3> prefix;
        for (auto& block : prefix) {
            f.program.block_storage.push_back(std::make_unique<Block>());
            block = f.program.block_storage.back().get();
        }
        prefix[0]->AddBranch(prefix[1]); prefix[0]->AddBranch(prefix[2]);
        prefix[1]->AddBranch(f.block); prefix[2]->AddBranch(f.block);
        f.program.blocks.insert(f.program.blocks.begin(), prefix.begin(), prefix.end());
        f.program.block_info = {
            {.id = 0, .terminator = {.kind = Kind::ConditionalBranch, .true_block = 1, .false_block = 2}, .condition = Value(true)},
            {.id = 1, .terminator = {.kind = Kind::Branch, .true_block = 3}},
            {.id = 2, .terminator = {.kind = Kind::Branch, .true_block = 3}},
            {.id = 3, .terminator = {.kind = Kind::Return}}};
        return prefix;
    };
    for (uint32_t reg : {10u, 12u, 20u}) for (unsigned mode = 0; mode < 5; ++mode) {
        Fixture f(mode, reg);
        Check(matches(f, reg), "position metadata: structural user origin/arithmetic rejected");
        if (reg != 10) Check(!f.Matches(), "position metadata broadened the s10 camera observer");
    }
    {
        Fixture f(0, 12); const auto prefix = diamond(f);
        const auto left_raw = Value(&prefix[1]->AppendNewInst(ValueOpcode::GetAttribute, {Value(0u), Value(0u)}));
        const auto right_raw = Value(&prefix[2]->AppendNewInst(ValueOpcode::GetAttribute, {Value(1u), Value(0u)}));
        const auto left = Value(&prefix[1]->AppendNewInst(ValueOpcode::BitCastF32U32, {left_raw}));
        const auto right = Value(&prefix[2]->AppendNewInst(ValueOpcode::BitCastF32U32, {right_raw}));
        auto& phi = *f.block->PrependNewInst(f.block->begin(), ValueOpcode::Phi);
        phi.SetFlags(Type::F32);
        phi.AddPhiOperand(prefix[1], left); phi.AddPhiOperand(prefix[2], right);
        f.source[0].TryInstruction()->ReplaceUsesWith(Value(&phi));
        Check(matches(f), "position metadata: independent prefix source Phi rejected");
        phi.SetArg(1, f.matrix[0]);
        Check(missing(f), "position metadata: non-dominating camera Phi accepted");
    }
    {
        Fixture f(0, 12); const auto prefix = diamond(f);
        auto& low = prefix[0]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(12))});
        auto& high = prefix[0]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(13))});
        auto& address = prefix[0]->AppendNewInst(ValueOpcode::GetAddressResource, {Value(&low), Value(&high)});
        auto& load = prefix[0]->AppendNewInst(ValueOpcode::LoadAddressU32, {Value(&address), Value(80u), Value(0u), Value(true)});
        load.SetFlags(MemoryFlags{.index = 0});
        const auto coefficient = Value(&prefix[0]->AppendNewInst(ValueOpcode::BitCastF32U32, {Value(&load)}));
        const auto raw = Value(&prefix[2]->AppendNewInst(ValueOpcode::GetAttribute, {Value(0u), Value(0u)}));
        const auto opaque = Value(&prefix[2]->AppendNewInst(ValueOpcode::BitCastF32U32, {raw}));
        auto& phi = *f.block->PrependNewInst(f.block->begin(), ValueOpcode::Phi);
        phi.SetFlags(Type::F32);
        phi.AddPhiOperand(prefix[1], coefficient); phi.AddPhiOperand(prefix[2], opaque);
        f.source[0].TryInstruction()->ReplaceUsesWith(Value(&phi));
        Gt7CameraPositionDetail::PositionDag dag;
        Check(dag.Build(f.program, f.exported), "position metadata: Phi taint negative must have valid dominance");
        Check(missing(f), "position metadata: dominating camera coefficient hidden in source Phi accepted");
    }
    for (const auto op : {ValueOpcode::FPAbs32, ValueOpcode::FPNeg32, ValueOpcode::FPSaturate32, ValueOpcode::FPSqrt}) {
        Fixture f(0, 12);
        const auto factor = before_output(f, op, {f.matrix[0]});
        multiply_all(f, factor);
        Check(missing(f), "position metadata: hidden camera modifier accepted as opaque");
    }
    {
        Fixture f;
        const auto factor = before_output(f, ValueOpcode::FPAbs32, {f.matrix[0]});
        multiply_all(f, factor);
        Check(!f.Matches(), "position metadata: old s10 proof still hides camera dependency");
    }
    {
        Fixture f(0, 12);
        const auto factor = before_output(f, ValueOpcode::SelectF32, {Value(true), f.matrix[0], f.source[0]});
        multiply_all(f, factor);
        Check(missing(f), "position metadata: hidden camera select accepted");
    }
    {
        Fixture f(0, 12);
        const auto offset = before_output(f, ValueOpcode::BitCastU32F32, {f.matrix[0]});
        const auto factor = before_output(f, ValueOpcode::LoadAddressU32, {f.address, offset, Value(0u), Value(true)});
        factor.TryInstruction()->SetFlags(MemoryFlags{.index = 0});
        multiply_all(f, factor);
        Check(missing(f), "position metadata: camera-dependent load address accepted");
    }
    {
        Fixture f(0, 12);
        const auto factor = before_output(f, ValueOpcode::LoadAddressU32, {f.address, Value(81u), Value(0u), Value(true)});
        factor.TryInstruction()->SetFlags(MemoryFlags{.index = 0});
        multiply_all(f, factor);
        Check(missing(f), "position metadata: ambiguous scalar alignment accepted");
    }
    {
        Fixture f(0, 12); diamond(f);
        f.program.block_info[0].condition = f.matrix[0];
        Check(missing(f), "position metadata: camera-dependent/invalid prefix condition accepted");
    }
    {
        Fixture f(0, 12); const auto prefix = diamond(f);
        // A well-formed U1 camera-derived condition whose load dominates the branch.
        auto& low = prefix[0]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(12))});
        auto& high = prefix[0]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(13))});
        auto& address = prefix[0]->AppendNewInst(ValueOpcode::GetAddressResource, {Value(&low), Value(&high)});
        auto& load = prefix[0]->AppendNewInst(ValueOpcode::LoadAddressU32, {Value(&address), Value(80u), Value(0u), Value(true)});
        load.SetFlags(MemoryFlags{.index = 0});
        auto& condition = prefix[0]->AppendNewInst(ValueOpcode::IEqual32, {Value(&load), Value(0u)});
        f.program.block_info[0].condition = Value(&condition);
        Check(missing(f), "position metadata: structurally valid camera-dependent branch accepted");
    }
    {
        Fixture f(0, 12); const auto prefix = diamond(f);
        auto& phi = *f.block->PrependNewInst(f.block->begin(), ValueOpcode::Phi);
        phi.SetFlags(Type::U32);
        auto& low1 = prefix[1]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(12))});
        auto& low2 = prefix[2]->AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(10))});
        phi.AddPhiOperand(prefix[1], Value(&low1)); phi.AddPhiOperand(prefix[2], Value(&low2));
        f.address.TryInstruction()->SetArg(0, Value(&phi));
        Check(missing(f), "position metadata: branch changes camera pointer accepted");
    }
    {
        Fixture f(0, 12); const auto prefix = diamond(f);
        auto early = std::make_unique<Block>();
        auto* early_ptr = early.get(); f.program.block_storage.push_back(std::move(early));
        prefix[0]->AddBranch(early_ptr); f.program.blocks.push_back(early_ptr);
        f.program.block_info.push_back({.id = 4, .terminator = {.kind = Kind::Return}});
        Check(missing(f), "position metadata: extra bypass edge accepted");
    }
    {
        Fixture f(0, 12);
        auto entry = std::make_unique<Block>(); auto early = std::make_unique<Block>();
        auto* e = entry.get(); auto* r = early.get(); e->AddBranch(f.block); e->AddBranch(r);
        f.program.block_storage.push_back(std::move(entry)); f.program.block_storage.push_back(std::move(early));
        f.program.blocks = {e, f.block, r};
        f.program.block_info = {
            {.id = 0, .terminator = {.kind = Kind::ConditionalBranch, .true_block = 1, .false_block = 2}, .condition = Value(true)},
            {.id = 1, .terminator = {.kind = Kind::Return}}, {.id = 2, .terminator = {.kind = Kind::Return}}};
        Check(missing(f), "position metadata: valid CFG return bypasses POS0");
    }
    {
        Fixture f(0, 12); f.block->AddBranch(f.block);
        f.program.block_info[0].terminator = {.kind = Kind::Branch, .true_block = 0};
        Check(missing(f), "position metadata: cycle accepted");
    }
    {
        Fixture f(0, 12); f.Export();
        Check(missing(f), "position metadata: multiple POS0 accepted");
        f.program.export_info.back().index = 1; f.program.export_info.back().en = 4;
        Check(matches(f), "position metadata: auxiliary position export rejected");
    }
    {
        Fixture f(0, 12); f.exported->SetArg(1, Value(false));
        Check(missing(f), "position metadata: predicated POS0 accepted");
    }
    {
        Fixture f(0, 12); f.program.export_info[0].en = 7;
        Check(missing(f), "position metadata: incomplete POS0 accepted");
    }
    {
        Fixture f(0, 12); f.program.position_matrix = ProvePositionMatrixContract(f.program);
        const auto info = std::move(f.program).TakeCompiledInfo();
        Check(info.position_matrix.user_sgpr == 12 && info.position_matrix.byte_offset == 80,
            "position metadata lost when native IR discarded");
        const std::array<uint32_t, 1> spirv {0x07230203};
        std::vector<uint8_t> bytes; ShaderDiskCache::EncodePermutationRecord(spirv, info, bytes);
        ShaderDiskCache::PermutationRecord decoded;
        Check(ShaderDiskCache::DecodePermutationRecord(bytes, decoded) &&
            decoded.program.position_matrix.kind == PositionMatrixKind::UserDataRowMajor4x4V1 &&
            decoded.program.position_matrix.user_sgpr == 12 && decoded.program.position_matrix.byte_offset == 80 &&
            ShaderDiskCache::PermutationRecordRoundTrips(bytes), "position metadata cache round-trip changed proof");
    }
}

void TestGt7CameraShaderContract() {
    TestPositionMatrixContract();
    using namespace ShaderRecompiler::IR;
    using Gt7CameraShaderTest::Fixture;
    for (unsigned mode = 0; mode != 5; ++mode) {
        Fixture f(mode);
        Check(f.Matches(), "GT7 camera: FMA/MAD/reordered add/implicit w/precomposed matrix rejected");
    }
    {
        Fixture f(4);
        f.output[1].TryInstruction()->SetArg(0, f.source[0]);
        Check(!f.Matches(), "GT7 camera: precomposed matrix with different column source accepted");
    }
    {
        Fixture f;
        const auto nonlinear = f.Emit(ValueOpcode::FPMul32, {f.output[2], f.matrix[0]});
        f.exported->Arg(0).TryInstruction()->SetArg(2,
            f.Emit(ValueOpcode::BitCastU32F32, {nonlinear}));
        Check(!f.Matches(), "GT7 camera: quadratic camera coefficient accepted");
    }
    {
        Fixture f(4);
        f.Export();
        f.program.export_info.back().index = 1;
        f.program.export_info.back().en = 4;
        Check(f.Matches(), "GT7 camera: auxiliary POS1 incorrectly treated as another POS0");
    }
    {
        Fixture f;
        auto result = f.output[0];
        for (unsigned i = 0; i != 90; ++i)
            result = f.Emit(ValueOpcode::FPAdd32, {result, Value::F32(0.0f)});
        f.exported->Arg(0).TryInstruction()->SetArg(0,
            f.Emit(ValueOpcode::BitCastU32F32, {result}));
        Check(!f.Matches(), "GT7 camera: expression depth cap not enforced");
    }
    {
        Fixture f(4);
        auto result = f.output[0];
        for (unsigned i = 0; i != 3; ++i)
            result = f.Emit(ValueOpcode::FPAdd32, {result, result});
        f.exported->Arg(0).TryInstruction()->SetArg(0,
            f.Emit(ValueOpcode::BitCastU32F32, {result}));
        Check(!f.Matches(), "GT7 camera: polynomial term cap not enforced");
    }
    {
        Fixture f;
        f.program.user_data_base = 4;
        f.program.user_data_count = 8;
        Check(f.Matches(), "GT7 camera: logical SGPR base incorrectly treated as snapshot offset");
    }
    {
        Fixture f;
        auto entry = std::make_unique<Block>();
        entry->AddBranch(f.block);
        f.program.blocks.insert(f.program.blocks.begin(), entry.get());
        f.program.block_storage.push_back(std::move(entry));
        Check(f.Matches(), "GT7 camera: normal straight-line entry block rejected");
    }
    {
        Fixture f;
        auto* data = f.exported->Arg(0).TryInstruction();
        for (size_t i = 0; i != 4; ++i) data->SetArg(i, Value(0u));
        Check(!f.Matches(), "GT7 camera: unused plausible matrix loads accepted");
    }
    {
        Fixture f;
        const auto replacement = f.Emit(ValueOpcode::IAdd32,
            {f.address.TryInstruction()->Arg(0), Value(16u)});
        f.address.TryInstruction()->SetArg(0, replacement);
        Check(!f.Matches(), "GT7 camera: overwritten s10 pointer accepted");
    }
    {
        Fixture f;
        const auto other = f.Emit(ValueOpcode::GetUserData, {Value(ScalarReg(8))});
        f.address.TryInstruction()->SetArg(0, other);
        Check(!f.Matches(), "GT7 camera: wrong base register accepted");
    }
    {
        Fixture f;
        f.loads[0]->SetArg(1, Value(144u));
        Check(!f.Matches(), "GT7 camera: other matrix offset accepted");
    }
    {
        Fixture f;
        f.loads[0]->SetArg(3, Value(false));
        Check(!f.Matches(), "GT7 camera: disabled scalar load accepted");
    }
    {
        Fixture f;
        f.program.memory_info[1].offset = 0;
        Check(!f.Matches(), "GT7 camera: transposed/wrong column accepted");
    }
    {
        Fixture f;
        auto* top = f.output[1].TryInstruction();
        top->SetArg(1, f.source[3]);
        Check(!f.Matches(), "GT7 camera: inconsistent source vector across columns accepted");
    }
    {
        Fixture f;
        auto* top = f.output[0].TryInstruction();
        top->SetArg(0, f.matrix[4]);
        Check(!f.Matches(), "GT7 camera: duplicate/missing matrix row accepted");
    }
    {
        Fixture f;
        auto* vector = f.exported->Arg(0).TryInstruction();
        const auto bias = f.Emit(ValueOpcode::FPAdd32, {f.output[2], Value::F32(0.01f)});
        vector->SetArg(2, f.Emit(ValueOpcode::BitCastU32F32, {bias}));
        Check(!f.Matches(), "GT7 camera: post-projection depth bias accepted");
    }
    {
        Fixture f;
        auto* vector = f.exported->Arg(0).TryInstruction();
        const auto clamp = f.Emit(ValueOpcode::FPSaturate32, {f.output[2]});
        vector->SetArg(2, f.Emit(ValueOpcode::BitCastU32F32, {clamp}));
        Check(!f.Matches(), "GT7 camera: nonlinear clip-coordinate transform accepted");
    }
    {
        Fixture f;
        f.program.export_info[0].en = 7;
        Check(!f.Matches(), "GT7 camera: partial position export accepted");
    }
    {
        Fixture f;
        f.program.export_info[0].index = 1;
        Check(!f.Matches(), "GT7 camera: auxiliary position substituted for clip position");
    }
    {
        Fixture f;
        f.exported->SetArg(1, Value(false));
        Check(!f.Matches(), "GT7 camera: predicated-away position export accepted");
    }
    {
        Fixture f;
        f.Export();
        Check(!f.Matches(), "GT7 camera: second position write accepted");
    }
    {
        Fixture f;
        f.program.dispatcher_fallback = true;
        Check(!f.Matches(), "GT7 camera: dispatcher fallback accepted");
    }
    {
        Fixture f;
        f.program.block_info.push_back({});
        f.program.block_info.back().terminator.kind = ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
        Check(!f.Matches(), "GT7 camera: conditional branch accepted");
    }
    {
        Fixture f;
        f.block->AddBranch(f.block);
        Check(!f.Matches(), "GT7 camera: loop accepted");
    }
    {
        Fixture f;
        f.Emit(ValueOpcode::Phi);
        Check(!f.Matches(), "GT7 camera: unresolved Phi accepted");
    }
    {
        Fixture f;
        f.program.stage = ShaderType::Mesh;
        Check(!f.Matches(), "GT7 camera: mesh stage accepted");
    }
    {
        Fixture f;
        f.program.user_data_count = 11;
        Check(!f.Matches(), "GT7 camera: missing s11 user register accepted");
    }
    {
        Fixture f;
        f.program.gt7_camera_position = f.Matches();
        const auto compiled = std::move(f.program).TakeCompiledInfo();
        Check(compiled.gt7_camera_position, "GT7 camera: metadata lost when native IR discarded");
        for (const bool accepted : {false, true}) {
            auto info = compiled;
            info.gt7_camera_position = accepted;
            const std::array<uint32_t, 1> spirv {0x07230203};
            std::vector<uint8_t> bytes;
            ShaderDiskCache::EncodePermutationRecord(spirv, info, bytes);
            ShaderDiskCache::PermutationRecord decoded;
            Check(ShaderDiskCache::DecodePermutationRecord(bytes, decoded) &&
                      decoded.program.gt7_camera_position == accepted &&
                      ShaderDiskCache::PermutationRecordRoundTrips(bytes),
                  "GT7 camera: true/false proof metadata changed on disk-cache hit");
        }
    }
}
