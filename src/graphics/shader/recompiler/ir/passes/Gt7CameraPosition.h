#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace Gt7CameraPositionDetail {

// This is a deliberately narrow proof, not a search for a familiar constant-buffer layout.
// Run after SSA/identity simplification, before resource tracking replaces scalar reads by
// snapshots. The four final clip coordinates must be one row vector times the
// selected original user-data pointer's +80 matrix. The older observer remains s10-only.
inline Value Bits(Value value) {
    for (unsigned count = 0; count != 16; ++count) {
        const auto* inst = value.TryInstruction();
        if (inst == nullptr) break;
        const auto op = inst->GetOpcode();
        if (op != ValueOpcode::Identity && op != ValueOpcode::BitCastF32U32 &&
            op != ValueOpcode::BitCastU32F32) break;
        if (inst->NumArgs() != 1) return {};
        value = inst->Arg(0);
    }
    if (value.IsImmediate() && (value.GetType() == Type::F32 || value.GetType() == Type::U32))
        return Value(static_cast<uint32_t>(value.ImmediateBits()));
    return value;
}

inline bool Immediate(Value value, Type type, uint64_t expected) {
    return value.IsImmediate() && value.GetType() == type && value.ImmediateBits() == expected;
}

inline bool UserRegister(Value value, uint32_t reg) {
    const auto* inst = Bits(value).TryInstruction();
    return inst != nullptr && inst->GetOpcode() == ValueOpcode::GetUserData &&
           inst->NumArgs() == 1 && inst->Arg(0).GetType() == Type::ScalarReg &&
           RegIndex(inst->Arg(0).ScalarRegister()) == reg;
}

inline uint32_t MatrixIndex(const Program& program, Value value, uint32_t reg = 10) {
    constexpr auto Missing = std::numeric_limits<uint32_t>::max();
    const auto* load = Bits(value).TryInstruction();
    if (load == nullptr || load->GetOpcode() != ValueOpcode::LoadAddressU32 ||
        load->NumArgs() != 4 || !Immediate(load->Arg(2), Type::U32, 0) ||
        !Immediate(load->Arg(3), Type::U1, 1)) return Missing;
    const auto index = load->Flags<MemoryFlags>().index;
    if (index >= program.memory_info.size()) return Missing;
    const auto& memory = program.memory_info[index];
    if (memory.kind != ResourceKind::ScalarAddress || memory.data_bits != 32 ||
        memory.data_dwords != 1 || !load->Arg(1).IsImmediate() ||
        load->Arg(1).GetType() != Type::U32) return Missing;
    const auto* address = Bits(load->Arg(0)).TryInstruction();
    if (address == nullptr || address->GetOpcode() != ValueOpcode::GetAddressResource ||
        address->NumArgs() != 2 || !UserRegister(address->Arg(0), reg) ||
        !UserRegister(address->Arg(1), reg + 1)) return Missing;
    // SrtWalker aligns both offset components separately and sign-extends the
    // instruction offset. The proof uses only their unambiguous aligned subset.
    if (memory.offset > INT32_MAX || (memory.offset & 3u) || (load->Arg(1).U32() & 3u)) return Missing;
    const uint64_t offset = uint64_t(memory.offset) + load->Arg(1).U32();
    if (offset < 80 || offset > 140 || (offset & 3u) != 0) return Missing;
    return static_cast<uint32_t>((offset - 80) / 4);
}

// Never call an unsupported expression opaque until every input (including
// select/compare/EXEC/address inputs and Phi incoming values) is independent.
// SSA makes architectural flags explicit. Remaining register state or cycles
// are unknown, rather than independent leaves.
class Dependency {
public:
    enum : uint8_t { Independent = 0, Pointer = 1, Matrix = 2, Unknown = 4 };
    Dependency(const Program& program, uint32_t reg) : program(program), reg(reg) {}
    uint8_t Get(Value value, unsigned depth = 0) {
        value = Bits(value);
        if (value.IsEmpty() || depth > 128) return Unknown;
        if (MatrixIndex(program, value, reg) != UINT32_MAX) return Matrix;
        const auto* inst = value.TryInstruction();
        if (!inst) return value.IsImmediate() ? Independent : Unknown;
        if (const auto found = cache.find(inst); found != cache.end()) return found->second;
        if (remaining == 0) return Unknown;
        --remaining;
        cache.emplace(inst, Unknown);
        const auto op = inst->GetOpcode();
        if ((op >= ValueOpcode::GetThreadBitScalarRegister && op <= ValueOpcode::SetM0) ||
            (op >= ValueOpcode::UndefU1 && op <= ValueOpcode::UndefU64) ||
            SharedAccessOf(op) != SharedAccess::None) return Unknown;
        if (op == ValueOpcode::GetUserData) {
            if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) return Unknown;
            const auto result = UserRegister(value, reg) || UserRegister(value, reg + 1) ? Pointer : Independent;
            cache[inst] = result;
            return result;
        }
        uint8_t result = Independent;
        if (op == ValueOpcode::Phi && (inst->NumArgs() == 0 || inst->NumPhiBlocks() != inst->NumArgs()))
            return Unknown;
        for (size_t i = 0; i < inst->NumArgs(); ++i) result |= Get(inst->Arg(i), depth + 1);
        if (AddressOpcodeInfoOf(op).access == AddressAccess::Read && (result & Pointer)) {
            // A constant disjoint scalar read from the same block is allowed;
            // arithmetic/dynamic/partial loads that might cover the matrix are not.
            bool disjoint = false;
            if (op == ValueOpcode::LoadAddressU32 && inst->NumArgs() == 4 &&
                Immediate(inst->Arg(2), Type::U32, 0) && Immediate(inst->Arg(3), Type::U1, 1) &&
                inst->Arg(1).GetType() == Type::U32 && inst->Arg(1).IsImmediate()) {
                const auto index = inst->Flags<MemoryFlags>().index;
                const auto* address = Bits(inst->Arg(0)).TryInstruction();
                if (index < program.memory_info.size() && address &&
                    address->GetOpcode() == ValueOpcode::GetAddressResource && address->NumArgs() == 2 &&
                    UserRegister(address->Arg(0), reg) && UserRegister(address->Arg(1), reg + 1)) {
                    const auto& memory = program.memory_info[index];
                    const uint64_t offset = uint64_t(memory.offset) + inst->Arg(1).U32();
                    disjoint = memory.kind == ResourceKind::ScalarAddress && memory.data_bits == 32 &&
                        memory.data_dwords == 1 && memory.offset <= INT32_MAX && !(memory.offset & 3u) &&
                        !(inst->Arg(1).U32() & 3u) && (offset < 80 || offset >= 144);
                }
            }
            result = disjoint && !(result & (Matrix | Unknown)) ? Independent : Unknown;
        }
        cache[inst] = result;
        return result;
    }
private:
    const Program& program;
    uint32_t reg;
    unsigned remaining = 8192;
    std::unordered_map<const Inst*, uint8_t> cache;
};

// The compiler can emit vertex * (model * camera), so checking only a final four-term dot
// misses otherwise identical camera producers. Expand just the arithmetic surrounding camera
// coefficients. Each final column must have the same multiset of source products for each row.
// Opaque source expressions are compared by SSA identity; no numerical sampling is involved.
struct Term {
    uint32_t matrix = 0;
    uint32_t factor_count = 0;
    std::array<Value, 4> factors {};
};

struct Expression {
    bool valid = false;
    Value opaque;
    std::vector<Term> terms;
};

inline bool IsZero(const Expression& expr) {
    return expr.valid && expr.terms.empty() && Immediate(expr.opaque, Type::U32, 0);
}

inline Expression Multiply(const Expression& a, const Expression& b, Value opaque) {
    if (!a.valid || !b.valid) return {};
    if (a.terms.empty() && b.terms.empty()) return {true, opaque, {}};
    if (!a.terms.empty() && !b.terms.empty()) return {}; // Quadratic camera coefficients.
    auto result = a.terms.empty() ? b : a;
    const auto factor = a.terms.empty() ? a.opaque : b.opaque;
    if (factor.IsEmpty()) return {};
    if (Immediate(factor, Type::U32, 0x3f800000u)) return result;
    for (auto& term : result.terms) {
        if (term.factor_count == term.factors.size()) return {};
        term.factors[term.factor_count++] = factor;
    }
    return result;
}

inline Expression Add(const Expression& a, const Expression& b, Value opaque) {
    if (!a.valid || !b.valid) return {};
    if (a.terms.empty() && b.terms.empty()) return {true, opaque, {}};
    if (a.terms.empty()) return IsZero(a) ? b : Expression {};
    if (b.terms.empty()) return IsZero(b) ? a : Expression {};
    if (a.terms.size() + b.terms.size() > 64) return {};
    auto result = a;
    result.terms.insert(result.terms.end(), b.terms.begin(), b.terms.end());
    return result;
}

class Expand {
public:
    explicit Expand(const Program& program, uint32_t reg = 10) : program(program), reg(reg), dependency(program, reg) {}

    Expression Get(Value value, unsigned depth = 0) {
        value = Bits(value);
        if (value.IsEmpty() || depth > 80) return {};
        const auto matrix = MatrixIndex(program, value, reg);
        if (matrix != std::numeric_limits<uint32_t>::max())
            return {true, {}, {{matrix, 0, {}}}};
        const auto* inst = value.TryInstruction();
        if (inst == nullptr) return {true, value, {}};
        if (const auto found = cache.find(inst); found != cache.end()) return found->second;
        if (remaining == 0) return {};
        --remaining;
        // Insert a failed sentinel first: malformed cyclic IR must not recurse indefinitely.
        cache.emplace(inst, Expression {});
        Expression result;
        const auto op = inst->GetOpcode();
        if (op == ValueOpcode::FPMul32 || op == ValueOpcode::FPAdd32 ||
            op == ValueOpcode::FPFma32 || op == ValueOpcode::FPMad32) {
            const auto expected_args = op == ValueOpcode::FPMul32 || op == ValueOpcode::FPAdd32 ? 2u : 3u;
            if (inst->NumArgs() != expected_args) return {};
            const auto a = Get(inst->Arg(0), depth + 1);
            const auto b = Get(inst->Arg(1), depth + 1);
            if (op == ValueOpcode::FPAdd32) result = Add(a, b, value);
            else if (op == ValueOpcode::FPMul32) result = Multiply(a, b, value);
            else {
                const auto c = Get(inst->Arg(2), depth + 1);
                if (a.valid && b.valid && c.valid && a.terms.empty() &&
                    b.terms.empty() && c.terms.empty()) result = {true, value, {}};
                else result = Add(Multiply(a, b, {}), c, value);
            }
        } else if (dependency.Get(value) == Dependency::Independent) {
            result = {true, value, {}};
        }
        cache[inst] = result;
        return result;
    }

private:
    const Program& program;
    uint32_t reg;
    Dependency dependency;
    unsigned remaining = 8192;
    std::unordered_map<const Inst*, Expression> cache;
};

inline bool SameSourceProduct(const Term& a, const Term& b) {
    if (a.matrix / 4 != b.matrix / 4 || a.factor_count != b.factor_count) return false;
    std::array<bool, 4> used {};
    for (uint32_t i = 0; i != a.factor_count; ++i) {
        bool found = false;
        for (uint32_t j = 0; j != b.factor_count; ++j) {
            if (!used[j] && a.factors[i] == b.factors[j]) {
                used[j] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

inline bool SameSourceVector(const Expression& a, const Expression& b) {
    if (a.terms.size() != b.terms.size()) return false;
    std::array<bool, 64> used {};
    for (const auto& term : a.terms) {
        bool found = false;
        for (size_t i = 0; i != b.terms.size(); ++i) {
            if (!used[i] && SameSourceProduct(term, b.terms[i])) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

inline bool StraightLine(const Program& program) {
    if (program.blocks.empty() || program.dispatcher_fallback ||
        program.cfg_failure_kind != CFG::FailureKind::None) return false;
    for (const auto& block : program.block_info) {
        if (block.terminator.loop_header ||
            (block.terminator.kind != CFG::TerminatorKind::Branch &&
             block.terminator.kind != CFG::TerminatorKind::Return)) return false;
    }
    const Block* current = program.blocks.front();
    for (size_t count = 0; count != program.blocks.size(); ++count) {
        if (current == nullptr || current->ImmPredecessors().size() != (count == 0 ? 0u : 1u) ||
            current->ImmSuccessors().size() > 1) return false;
        if (current->ImmSuccessors().empty()) return count + 1 == program.blocks.size();
        current = current->ImmSuccessors().front();
    }
    return false; // Cycle or a disconnected/unvisited block.
}

// Bounded acyclic native CFG. The unique POS0 block must dominate every return
// (equivalently postdominate entry in this finite DAG). Validate SSA use/edge
// dominance too: a load on only one branch cannot masquerade as a merged value.
class PositionDag {
public:
    bool Build(const Program& program, const Inst* position) {
        const size_t count = program.blocks.size();
        if (!position || count == 0 || count > 256 || program.block_info.size() != count ||
            program.dispatcher_fallback || program.cfg_failure_kind != CFG::FailureKind::None) return false;
        std::unordered_map<uint32_t, size_t> ids;
        for (size_t i = 0; i < count; ++i) {
            if (!program.blocks[i] || !blocks.emplace(program.blocks[i], i).second ||
                !ids.emplace(program.block_info[i].id, i).second) return false;
            size_t order = 0;
            for (const auto& inst : *program.blocks[i]) {
                if (instructions.size() >= 8192 || !instructions.emplace(&inst, Location{i, order++}).second)
                    return false;
                const auto shared = SharedAccessOf(inst.GetOpcode());
                if (shared != SharedAccess::None) return false;
            }
        }
        if (HasShaderMemoryWrites(program)) return false;
        for (size_t i = 0; i < count; ++i) {
            const auto* block = program.blocks[i];
            const auto& info = program.block_info[i];
            const auto& term = info.terminator;
            if (term.loop_header || !info.indirect_target.IsEmpty()) return false;
            std::vector<size_t> expected;
            const auto target = [&](uint32_t id) {
                const auto found = ids.find(id);
                if (found == ids.end()) return false;
                if (std::find(expected.begin(), expected.end(), found->second) == expected.end())
                    expected.push_back(found->second);
                return true;
            };
            if (term.kind == CFG::TerminatorKind::Branch) {
                if (!target(term.true_block)) return false;
            } else if (term.kind == CFG::TerminatorKind::ConditionalBranch) {
                if (!target(term.true_block) || !target(term.false_block) ||
                    info.condition.IsEmpty() || info.condition.GetType() != Type::U1) return false;
            } else if (term.kind != CFG::TerminatorKind::Return) return false;
            if (expected.size() != block->ImmSuccessors().size()) return false;
            for (const auto* next : block->ImmSuccessors()) {
                const auto found = blocks.find(next);
                if (found == blocks.end() || std::find(expected.begin(), expected.end(), found->second) == expected.end() ||
                    std::find(next->ImmPredecessors().begin(), next->ImmPredecessors().end(), block) == next->ImmPredecessors().end())
                    return false;
            }
            for (const auto* previous : block->ImmPredecessors()) {
                if (!blocks.contains(previous) || std::find(previous->ImmSuccessors().begin(), previous->ImmSuccessors().end(), block) ==
                    previous->ImmSuccessors().end()) return false;
            }
        }
        if (!program.blocks.front()->ImmPredecessors().empty()) return false;
        std::vector<uint8_t> color(count);
        std::vector<size_t> order;
        const auto visit = [&](auto&& self, size_t index) -> bool {
            if (color[index] == 1) return false;
            if (color[index] == 2) return true;
            color[index] = 1;
            for (const auto* next : program.blocks[index]->ImmSuccessors())
                if (!self(self, blocks.at(next))) return false;
            color[index] = 2;
            order.push_back(index);
            return true;
        };
        if (!visit(visit, 0) || order.size() != count) return false;
        std::reverse(order.begin(), order.end());
        dominates.assign(count, std::vector<bool>(count));
        for (const auto index : order) {
            const auto predecessors = program.blocks[index]->ImmPredecessors();
            if (index != 0) {
                if (predecessors.empty()) return false;
                dominates[index] = dominates[blocks.at(predecessors.front())];
                for (const auto* previous : predecessors)
                    for (size_t j = 0; j < count; ++j)
                        dominates[index][j] = dominates[index][j] && dominates[blocks.at(previous)][j];
            }
            dominates[index][index] = true;
        }
        const auto output = instructions.find(position);
        if (output == instructions.end()) return false;
        bool returned = false;
        for (size_t i = 0; i < count; ++i) {
            const auto& info = program.block_info[i];
            if (info.terminator.kind == CFG::TerminatorKind::Return) {
                returned = true;
                if (!dominates[i][output->second.block]) return false;
            }
            if (!info.condition.IsEmpty() && !Available(info.condition, i, SIZE_MAX)) return false;
            for (const auto& inst : *program.blocks[i]) {
                const bool phi = inst.GetOpcode() == ValueOpcode::Phi;
                const auto predecessors = program.blocks[i]->ImmPredecessors();
                if (phi && (inst.NumArgs() == 0 || inst.NumArgs() != predecessors.size() ||
                    inst.NumPhiBlocks() != inst.NumArgs())) return false;
                std::vector<const Block*> seen;
                for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
                    if (phi) {
                        if (inst.Arg(arg).GetType() != Value(const_cast<Inst*>(&inst)).GetType()) return false;
                        const auto* edge = inst.PhiBlock(arg);
                        if (std::find(predecessors.begin(), predecessors.end(), edge) == predecessors.end() ||
                            std::find(seen.begin(), seen.end(), edge) != seen.end()) return false;
                        seen.push_back(edge);
                        if (!Available(inst.Arg(arg), blocks.at(edge), SIZE_MAX)) return false;
                    } else if (!Available(inst.Arg(arg), i, instructions.at(&inst).order)) return false;
                }
            }
        }
        return returned;
    }
private:
    struct Location { size_t block; size_t order; };
    bool Available(Value value, size_t block, size_t order) const {
        if (value.IsImmediate()) return true;
        const auto* inst = value.TryInstruction();
        const auto found = instructions.find(inst);
        return found != instructions.end() && dominates[block][found->second.block] &&
            (found->second.block != block || found->second.order < order);
    }
    std::unordered_map<const Block*, size_t> blocks;
    std::unordered_map<const Inst*, Location> instructions;
    std::vector<std::vector<bool>> dominates;
};

inline const Inst* PositionExport(const Program& program) {
    const Inst* position = nullptr;
    for (const auto* block : program.blocks) for (const auto& inst : *block) {
        if (inst.GetOpcode() != ValueOpcode::SetAttribute) continue;
        const auto index = inst.Flags<ExportFlags>().index;
        if (index >= program.export_info.size()) return nullptr;
        const auto& exp = program.export_info[index];
        if (exp.kind != ExportTargetKind::Position || exp.index != 0) continue;
        if (position || exp.en != 0xf || exp.compr || inst.NumArgs() != 2 ||
            !Immediate(Bits(inst.Arg(1)), Type::U1, 1)) return nullptr;
        position = &inst;
    }
    return position;
}

inline bool PositionColumns(const Program& program, const Inst* position, uint32_t reg) {
    const auto* vector = Bits(position->Arg(0)).TryInstruction();
    if (!vector || vector->GetOpcode() != ValueOpcode::CompositeConstructU32x4 || vector->NumArgs() != 4) return false;
    Expand expand(program, reg);
    std::array<Expression, 4> dots {};
    for (uint32_t column = 0; column != 4; ++column) {
        dots[column] = expand.Get(vector->Arg(column));
        if (!dots[column].valid || dots[column].terms.empty()) return false;
        uint32_t rows = 0;
        for (const auto& term : dots[column].terms) {
            if (term.matrix % 4 != column) return false;
            rows |= 1u << (term.matrix / 4);
        }
        if (rows != 0xf || (column != 0 && !SameSourceVector(dots[0], dots[column]))) return false;
    }
    return true;
}

} // namespace Gt7CameraPositionDetail

// Separate from the s10 camera observer: this certifies only the actual position
// matrix at +80. It makes no claim about adjacent view/projection structures.
[[nodiscard]] inline PositionMatrixContract ProvePositionMatrixContract(const Program& program) {
    using namespace Gt7CameraPositionDetail;
    if (program.stage != ShaderType::Vertex || program.user_data_count > NumScalarRegs ||
        uint64_t(program.user_data_base) + program.user_data_count > NumScalarRegs) return {};
    const auto* position = PositionExport(program);
    PositionDag dag;
    if (!dag.Build(program, position)) return {};
    std::vector<uint32_t> candidates;
    for (const auto* block : program.blocks) for (const auto& inst : *block) {
        if (inst.GetOpcode() != ValueOpcode::LoadAddressU32 || inst.NumArgs() != 4) continue;
        const auto* address = Bits(inst.Arg(0)).TryInstruction();
        if (!address || address->GetOpcode() != ValueOpcode::GetAddressResource || address->NumArgs() != 2) continue;
        const auto* low = Bits(address->Arg(0)).TryInstruction();
        if (!low || low->GetOpcode() != ValueOpcode::GetUserData || low->NumArgs() != 1 ||
            low->Arg(0).GetType() != Type::ScalarReg) continue;
        const uint32_t reg = RegIndex(low->Arg(0).ScalarRegister());
        if (reg < program.user_data_base || uint64_t(reg) + 2 > uint64_t(program.user_data_base) + program.user_data_count ||
            MatrixIndex(program, Value(const_cast<Inst*>(&inst)), reg) == UINT32_MAX) continue;
        if (std::find(candidates.begin(), candidates.end(), reg) == candidates.end()) {
            if (candidates.size() == 8) return {};
            candidates.push_back(reg);
        }
    }
    PositionMatrixContract result;
    for (const auto reg : candidates) {
        Dependency dependency(program, reg);
        bool independent_control = true;
        for (const auto& block : program.block_info)
            if (!block.condition.IsEmpty() && dependency.Get(block.condition) != Dependency::Independent)
                independent_control = false;
        if (!independent_control || !PositionColumns(program, position, reg)) continue;
        if (result.kind != PositionMatrixKind::None) return {}; // Ambiguous origin.
        result = {PositionMatrixKind::UserDataRowMajor4x4V1, reg, 80};
    }
    return result;
}

// Metadata only. It never changes shader code or enables frame generation on its own.
// Renderer still has to validate the current camera values, raster state and exact TAA pair.
[[nodiscard]] inline bool HasGt7CameraPositionContract(const Program& program) {
    using namespace Gt7CameraPositionDetail;
    if (program.stage != ShaderType::Vertex || program.user_data_base > 10 ||
        uint64_t(program.user_data_base) + program.user_data_count < 12 ||
        !StraightLine(program)) return false;
    const Inst* position = nullptr;
    for (const auto* block : program.blocks) {
        for (const auto& inst : *block) {
            // Even a selected source coordinate needs a separate control-flow proof.
            if (inst.GetOpcode() == ValueOpcode::Phi) return false;
            if (inst.GetOpcode() != ValueOpcode::SetAttribute) continue;
            const auto index = inst.Flags<ExportFlags>().index;
            if (index >= program.export_info.size()) return false;
            const auto& exp = program.export_info[index];
            // POS1+ carry point/clip/cull/viewport/layer outputs and cannot overwrite POS0.
            // The renderer separately rejects viewport/layer routing for camera producers.
            if (exp.kind != ExportTargetKind::Position || exp.index != 0) continue;
            if (position != nullptr || exp.en != 0xf || exp.compr ||
                inst.NumArgs() != 2 || !Immediate(inst.Arg(1), Type::U1, 1)) return false;
            position = &inst;
        }
    }
    if (position == nullptr) return false;
    const auto* vector = Bits(position->Arg(0)).TryInstruction();
    if (vector == nullptr || vector->GetOpcode() != ValueOpcode::CompositeConstructU32x4 ||
        vector->NumArgs() != 4) return false;
    Expand expand(program);
    std::array<Expression, 4> dots {};
    for (uint32_t column = 0; column != 4; ++column) {
        dots[column] = expand.Get(vector->Arg(column));
        if (!dots[column].valid || dots[column].terms.empty()) return false;
        uint32_t rows = 0;
        for (const auto& term : dots[column].terms) {
            if (term.matrix % 4 != column) return false;
            rows |= 1u << (term.matrix / 4);
        }
        if (rows != 0xf || (column != 0 && !SameSourceVector(dots[0], dots[column]))) return false;
    }
    return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
