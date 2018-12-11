// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2018 Pawel Bylica.
// Licensed under the Apache License, Version 2.0.

#include "execution.hpp"
#include "analysis.hpp"

#include <evmc/instructions.h>

namespace evmone
{
namespace
{
bool check_memory(execution_state& state, const uint256& offset, const uint256& size) noexcept
{
    if (size == 0)
        return true;

    constexpr auto limit = uint32_t(-1);

    if (limit < offset || limit < size)  // TODO: Revert order of args in <.
    {
        state.run = false;
        state.status = EVMC_OUT_OF_GAS;
        return false;
    }

    const auto o = static_cast<int64_t>(offset);
    const auto s = static_cast<int64_t>(size);

    const auto m = static_cast<int64_t>(state.memory.size());

    const auto new_size = o + s;
    if (m < s)
    {
        auto w = (new_size + 31) / 32;
        auto new_cost = 3 * w + w * w / 512;
        auto cost = new_cost - state.memory_prev_cost;
        state.memory_prev_cost = new_cost;

        state.gas_left -= cost;
        if (state.gas_left < 0)
        {
            state.run = false;
            state.status = EVMC_OUT_OF_GAS;
            return false;
        }

        state.memory.resize(static_cast<size_t>(new_size));
    }

    return true;
}


void op_stop(execution_state& state, const bytes32*) noexcept
{
    state.run = false;
}

void op_add(execution_state& state, const bytes32*) noexcept
{
    state.item(1) += state.item(0);
    state.stack.pop_back();
}

void op_mstore(execution_state& state, const bytes32*) noexcept
{
    auto index = state.item(0);
    auto x = state.item(1);

    if (!check_memory(state, index, 32))
        return;

    intx::be::store(&state.memory[static_cast<size_t>(index)], x);

    state.stack.pop_back();
    state.stack.pop_back();
}

void op_gas(execution_state& state, const bytes32* extra) noexcept
{
    (void)extra;
    (void)state;
}

void op_push_full(execution_state& state, const bytes32* extra) noexcept
{
    // OPT: For smaller pushes, use pointer data directly.
    auto x = intx::be::uint256(extra->bytes);
    state.stack.push_back(x);
}

void op_pop(execution_state& state, const bytes32*) noexcept
{
    state.stack.pop_back();
}

void op_return(execution_state& state, const bytes32*) noexcept
{
    state.run = false;
    state.output_offset = static_cast<size_t>(state.item(0));
    state.output_size = static_cast<size_t>(state.item(1));

    // TODO: Check gas cost
}

exec_fn_table op_table = []() noexcept
{
    exec_fn_table table{};
    table[OP_STOP] = op_stop;
    table[OP_ADD] = op_add;
    table[OP_GAS] = op_gas;
    table[OP_POP] = op_pop;
    table[OP_MSTORE] = op_mstore;
    for (size_t op = OP_PUSH1; op <= OP_PUSH32; ++op)
        table[op] = op_push_full;
    table[OP_RETURN] = op_return;
    return table;
}
();
}  // namespace


evmc_result execute(int64_t gas, const uint8_t* code, size_t code_size) noexcept
{
    auto analysis = analyze(op_table, code, code_size);

    execution_state state;
    state.gas_left = gas;
    while (state.run)
    {
        auto& instr = analysis.instrs[state.pc];
        if (instr.block_index >= 0)
        {
            auto& block = analysis.blocks[static_cast<size_t>(instr.block_index)];

            state.gas_left -= block.gas_cost;
            if (state.gas_left <= 0)
            {
                state.status = EVMC_OUT_OF_GAS;
                break;
            }

            if (static_cast<int>(state.stack.size()) < block.stack_req)
            {
                state.status = EVMC_STACK_UNDERFLOW;
                break;
            }

            if (static_cast<int>(state.stack.size()) + block.stack_max > 1024)
            {
                state.status = EVMC_STACK_OVERFLOW;
                break;
            }
        }

        // TODO: Change to pointer in analysis.
        auto* extra = &analysis.extra[static_cast<size_t>(instr.extra_data_index)];

        // Advance the PC not to allow jump opcodes to overwrite it.
        ++state.pc;

        instr.fn(state, extra);
    }

    evmc_result result{};
    result.status_code = state.status;
    if (state.status == EVMC_SUCCESS || state.status == EVMC_REVERT)
        result.gas_left = state.gas_left;

    if (state.output_size > 0)
    {
        result.output_size = state.output_size;
        auto output_data = static_cast<uint8_t*>(std::malloc(result.output_size));
        std::memcpy(output_data, &state.memory[state.output_offset], result.output_size);
        result.output_data = output_data;
        result.release = [](const evmc_result* result) noexcept
        {
            std::free(const_cast<uint8_t*>(result->output_data));
        };
    }

    return result;
}

}  // namespace evmone