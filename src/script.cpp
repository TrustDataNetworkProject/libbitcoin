#include <bitcoin/script.hpp>

#include <stack>

#include <boost/optional.hpp>

#include <bitcoin/constants.hpp>
#include <bitcoin/messages.hpp>
#include <bitcoin/transaction.hpp>
#include <bitcoin/format.hpp>
#include <bitcoin/utility/elliptic_curve_key.hpp>
#include <bitcoin/utility/assert.hpp>
#include <bitcoin/utility/logger.hpp>
#include <bitcoin/utility/ripemd.hpp>
#include <bitcoin/utility/sha256.hpp>

namespace libbitcoin {

typedef boost::optional<size_t> optional_number;

static const data_chunk stack_true_value{1};
static const data_chunk stack_false_value;  // False is an empty

bool script::conditional_stack::closed() const
{
    return stack_.empty();
}
bool script::conditional_stack::has_failed_branches() const
{
    return std::count(stack_.begin(), stack_.end(), false) > 0;
}

void script::conditional_stack::clear()
{
    stack_.clear();
}
void script::conditional_stack::open(bool value)
{
    stack_.push_back(value);
}
void script::conditional_stack::else_()
{
    stack_.back() = !stack_.back();
}
void script::conditional_stack::close()
{
    stack_.pop_back();
}

void script::join(const script& other)
{
    operations_.insert(operations_.end(),
        other.operations_.begin(), other.operations_.end());
}

void script::push_operation(operation oper)
{
    operations_.push_back(oper);
}

const operation_stack& script::operations() const
{
    return operations_;
}

inline bool cast_to_big_number(const data_chunk& raw_number,
    big_number& result)
{
    // Satoshi bitcoin does it this way.
    // Copy its quack behaviour
    if (raw_number.size() > 4)
        return false;
    big_number mid;
    mid.set_data(raw_number);
    result.set_data(mid.data());
    return true;
}

inline bool cast_to_bool(const data_chunk& values)
{
    for (auto it = values.begin(); it != values.end(); ++it)
    {
        if (*it != 0)
        {
            // Can be negative zero
            if (it == values.end() - 1 && *it == 0x80)
                return false;
            return true;
        }
    }
    return false;
}

bool is_push_only(const operation_stack& operations)
{
    auto is_push =
        [](opcode code)
        {
            return code == opcode::zero
                || code == opcode::special
                || code == opcode::pushdata1
                || code == opcode::pushdata2
                || code == opcode::pushdata4
                || code == opcode::negative_1
                || code == opcode::op_1
                || code == opcode::op_2
                || code == opcode::op_3
                || code == opcode::op_4
                || code == opcode::op_5
                || code == opcode::op_6
                || code == opcode::op_7
                || code == opcode::op_8
                || code == opcode::op_9
                || code == opcode::op_10
                || code == opcode::op_11
                || code == opcode::op_12
                || code == opcode::op_13
                || code == opcode::op_14
                || code == opcode::op_15
                || code == opcode::op_16;
        };
    for (const operation& op: operations)
        if (!is_push(op.code))
            return false;
    return true;
}

bool script::run(script input_script, const message::transaction& parent_tx,
    uint32_t input_index, bool bip16_enabled)
{
    stack_.clear();
    input_script.stack_.clear();
    if (!input_script.run(parent_tx, input_index))
        return false;
    stack_ = input_script.stack_;
    if (!run(parent_tx, input_index))
        return false;
    if (stack_.empty())
        return false;
    if (!cast_to_bool(stack_.back()))
        return false;
    // Additional validation for spend-to-script-hash transactions
    if (bip16_enabled && type() == payment_type::script_hash)
    {
        if (!is_push_only(input_script.operations()))
            return false;
        // Load last input_script stack item as a script
        data_stack eval_stack = input_script.stack_;
        script eval_script = parse_script(input_script.stack_.back());
        // Pop last item and copy as starting stack to eval script
        eval_stack.pop_back();
        eval_script.stack_ = eval_stack;
        // Run script
        if (!eval_script.run(parent_tx, input_index))
            return false;
        if (eval_script.stack_.empty())
            return false;
        return cast_to_bool(eval_script.stack_.back());
    }
    return true;
}

bool opcode_is_disabled(opcode code)
{
    switch (code)
    {
        /*cat:
        substr:
        left:
        right:
        invert:
        and:
        or:
        xor:
        2mul:
        2div:
        mul:
        div:
        mod:
        lshift:
        rshift:*/
        // return true;

        default:
            return false;
    }
    return true;
}

bool script::run(const message::transaction& parent_tx, uint32_t input_index)
{
    alternate_stack_.clear();
    codehash_begin_ = operations_.begin();
    conditional_stack_.clear();
    for (auto it = operations_.begin(); it != operations_.end(); ++it)
        if (!next_step(it, parent_tx, input_index))
            return false;
    if (!conditional_stack_.closed())
        return false;
    return true;
}

bool script::next_step(operation_stack::iterator it,
    const message::transaction& parent_tx, uint32_t input_index)
{
    const operation& op = *it;
    if (opcode_is_disabled(op.code))
        return false;
    auto is_condition_opcode =
        [](opcode code)
        {
            return code == opcode::if_
                || code == opcode::notif
                || code == opcode::else_
                || code == opcode::endif;
        };
    bool allow_execution = !conditional_stack_.has_failed_branches();
    // continue onwards to next command.
    if (!allow_execution && !is_condition_opcode(op.code))
        return true;
    // push data to the stack
    if (op.code == opcode::zero)
        stack_.push_back(data_chunk());
    // These operations may also push empty data (opcode zero)
    // Hence we check the opcode over the shorter !op.data.empty()
    else if (op.code == opcode::special
        || op.code == opcode::pushdata1
        || op.code == opcode::pushdata2
        || op.code == opcode::pushdata4)
    {
        stack_.push_back(op.data);
    }
    else if (op.code == opcode::codeseparator)
        codehash_begin_ = it;
    // opcodes above should assert inside run_operation
    else if (!run_operation(op, parent_tx, input_index))
        return false;
    //log_debug() << "--------------------";
    //log_debug() << "Run: " << opcode_to_string(op.code);
    //log_debug() << "Stack:";
    //for (auto s: stack_)
    //    log_debug() << "[" << pretty_hex(s) << "]";
    return true;
}

data_chunk script::pop_stack()
{
    data_chunk value = stack_.back();
    stack_.pop_back();
    return value;
}

bool script::arithmetic_start(big_number& number_a, big_number& number_b)
{
    if (stack_.size() < 2)
        return false;
    if (!cast_to_big_number(pop_stack(), number_a))
        return false;
    if (!cast_to_big_number(pop_stack(), number_b))
        return false;
    return true;
}

bool script::op_negative_1()
{
    big_number neg1;
    neg1.set_int64(-1);
    stack_.push_back(neg1.data());
    return true;
}

bool script::op_x(opcode code)
{
    uint8_t value_diff =
        static_cast<uint8_t>(code) -
            static_cast<uint8_t>(opcode::op_1) + 1;
    big_number big_repr(value_diff);
    stack_.push_back(big_repr.data());
    return true;
}

bool script::op_if()
{
    bool value = false;
    if (!conditional_stack_.has_failed_branches())
    {
        if (stack_.size() < 1)
            return false;
        value = cast_to_bool(pop_stack());
    }
    conditional_stack_.open(value);
    return true;
}
bool script::op_notif()
{
    // A bit hackish...
    // Open IF statement but then invert it to get NOTIF
    if (!op_if())
        return false;
    conditional_stack_.else_();
    return true;
}
bool script::op_else()
{
    if (conditional_stack_.closed())
        return false;
    conditional_stack_.else_();
    return true;
}
bool script::op_endif()
{
    if (conditional_stack_.closed())
        return false;
    conditional_stack_.close();
    return true;
}

bool script::op_verify()
{
    if (stack_.size() < 1)
        return false;
    if (!cast_to_bool(stack_.back()))
        return false;
    pop_stack();
    return true;
}

bool script::op_toaltstack()
{
    if (stack_.size() < 1)
        return false;
    data_chunk move_data = pop_stack();
    alternate_stack_.push_back(move_data);
    return true;
}

bool script::op_fromaltstack()
{
    if (alternate_stack_.size() < 1)
        return false;
    stack_.push_back(alternate_stack_.back());
    alternate_stack_.pop_back();
    return true;
}

bool script::op_ifdup()
{
    if (stack_.size() < 1)
        return false;
    if (cast_to_bool(stack_.back()))
        stack_.push_back(stack_.back());
    return true;
}

bool script::op_depth()
{
    big_number stack_size(stack_.size());
    stack_.push_back(stack_size.data());
    return true;
}

bool script::op_drop()
{
    if (stack_.size() < 1)
        return false;
    stack_.pop_back();
    return true;
}

bool script::op_dup()
{
    if (stack_.size() < 1)
        return false;
    stack_.push_back(stack_.back());
    return true;
}

bool script::op_nip()
{
    if (stack_.size() < 2)
        return false;
    stack_.erase(stack_.end() - 2);
    return true;
}

bool script::op_over()
{
    if (stack_.size() < 2)
        return false;
    stack_.push_back(*(stack_.end() - 2));
    return true;
}

template <typename DataStack>
bool pick_roll_impl(DataStack& stack, bool is_roll)
{
    if (stack.size() < 2)
        return false;
    big_number number_n;
    if (!cast_to_big_number(stack.back(), number_n))
        return false;
    stack.pop_back();
    uint32_t n = number_n.uint32();
    if (n >= stack.size())
        return false;
    auto slice_iter = stack.end() - n - 1;
    data_chunk item = *slice_iter;
    if (is_roll)
        stack.erase(slice_iter);
    stack.push_back(item);
    return true;
}

bool script::op_pick()
{
    return pick_roll_impl(stack_, false);
}

bool script::op_roll()
{
    return pick_roll_impl(stack_, true);
}

bool script::op_size()
{
    if (stack_.size() < 1)
        return false;
    big_number top_size = stack_.back().size();
    stack_.push_back(top_size.data());
    return true;
}

bool script::op_not()
{
    if (stack_.size() < 1)
        return false;
    big_number number_n;
    if (!cast_to_big_number(pop_stack(), number_n))
        return false;
    stack_.push_back(
        big_number(number_n == big_number(0)).data());
    return true;
}

bool script::op_boolor()
{
    big_number number_a, number_b;
    if (!arithmetic_start(number_a, number_b))
        return false;
    big_number zero(0), result = number_a != zero || number_b != zero;
    stack_.push_back(result.data());
    return true;
}

bool script::op_min()
{
    big_number number_a, number_b;
    if (!arithmetic_start(number_a, number_b))
        return false;
    if (number_a < number_b)
        stack_.push_back(number_a.data());
    else
        stack_.push_back(number_b.data());
    return true;
}

bool script::op_sha256()
{
    if (stack_.size() < 1)
        return false;
    data_chunk data = pop_stack();
    data_chunk hash(sha256_length);
    SHA256(data.data(), data.size(), hash.data());
    stack_.push_back(hash);
    return true;
}

bool script::op_hash160()
{
    if (stack_.size() < 1)
        return false;
    data_chunk data = pop_stack();
    short_hash hash = generate_ripemd_hash(data);
    data_chunk raw_hash(hash.begin(), hash.end());
    stack_.push_back(raw_hash);
    return true;
}

bool script::op_equal()
{
    if (stack_.size() < 2)
        return false;
    if (pop_stack() == pop_stack())
        stack_.push_back(stack_true_value);
    else
        stack_.push_back(stack_false_value);
    return true;
}

bool script::op_equalverify()
{
    if (stack_.size() < 2)
        return false;
    return pop_stack() == pop_stack();
}

bool script::op_add()
{
    big_number number_a, number_b;
    if (!arithmetic_start(number_a, number_b))
        return false;
    big_number result = number_a + number_b;
    stack_.push_back(result.data());
    return true;
}

bool script::op_greaterthanorequal()
{
    big_number number_a, number_b;
    if (!arithmetic_start(number_a, number_b))
        return false;
    big_number result = number_a >= number_b;
    stack_.push_back(result.data());
    return true;
}

inline void nullify_input_sequences(
    message::transaction_input_list& inputs, uint32_t except_input)
{
    for (size_t i = 0; i < inputs.size(); ++i)
        if (i != except_input)
            inputs[i].sequence = 0;
}

hash_digest script::generate_signature_hash(
    message::transaction parent_tx, uint32_t input_index,
    const script& script_code, uint32_t hash_type)
{
    BITCOIN_ASSERT(input_index < parent_tx.inputs.size());

    if ((hash_type & 0x1f) == sighash::none)
    {
        parent_tx.outputs.clear();
        nullify_input_sequences(parent_tx.inputs, input_index);
    }
    else if ((hash_type & 0x1f) == sighash::single)
    {
        uint32_t output_index = input_index;
        if (output_index >= parent_tx.outputs.size())
        {
            log_error() << "sighash::single the output_index is out of range";
            return null_hash;
        }
        parent_tx.outputs.resize(output_index + 1);
        for (message::transaction_output& output: parent_tx.outputs)
        {
            output.value = ~0;
            output.output_script = script();
        }
        nullify_input_sequences(parent_tx.inputs, input_index);
    }

    if (hash_type & sighash::anyone_can_pay)
    {
        parent_tx.inputs[0] = parent_tx.inputs[input_index];
        parent_tx.inputs.resize(1);
    }

    if (input_index >= parent_tx.inputs.size())
    {
        log_fatal() << "script::op_checksig() : input_index " << input_index
                << " is out of range.";
        return null_hash;
    }

    message::transaction tx_tmp = parent_tx;
    // Blank all other inputs' signatures
    for (message::transaction_input& input: tx_tmp.inputs)
        input.input_script = script();
    tx_tmp.inputs[input_index].input_script = script_code;

    return hash_transaction(tx_tmp, hash_type);
}

bool check_signature(data_chunk signature,
    const data_chunk& pubkey, const script& script_code,
    const message::transaction& parent_tx, uint32_t input_index)
{
    elliptic_curve_key key;
    if (!key.set_public_key(pubkey))
        return false;

    uint32_t hash_type = 0;
    hash_type = signature.back();
    signature.pop_back();

    hash_digest tx_hash =
        script::generate_signature_hash(
            parent_tx, input_index, script_code, hash_type);
    if (tx_hash == null_hash)
        return false;
    return key.verify(tx_hash, signature);
}

bool script::op_checksig(
    const message::transaction& parent_tx, uint32_t input_index)
{
    if (op_checksigverify(parent_tx, input_index))
        stack_.push_back(stack_true_value);
    else
        stack_.push_back(stack_false_value);
    return true;
}

bool script::op_checksigverify(
    const message::transaction& parent_tx, uint32_t input_index)
{
    if (stack_.size() < 2)
        return false;
    data_chunk pubkey = pop_stack(), signature = pop_stack();

    script script_code;
    for (auto it = codehash_begin_; it != operations_.end(); ++it)
    {
        const operation op = *it;
        if (op.data == signature || op.code == opcode::codeseparator)
            continue;
        script_code.push_operation(op);
    }
    return check_signature(signature, pubkey,
        script_code, parent_tx, input_index);
}

bool script::op_checkmultisig(
    const message::transaction& parent_tx, uint32_t input_index)
{
    if (op_checkmultisigverify(parent_tx, input_index))
        stack_.push_back(stack_true_value);
    else
        stack_.push_back(stack_false_value);
    return true;
}

bool script::read_section(data_stack& section)
{
    if (stack_.empty())
        return false;
    big_number count_big_num;
    if (!cast_to_big_number(pop_stack(), count_big_num))
        return false;
    const size_t count = count_big_num.uint32();

    if (stack_.size() < count)
        return false;
    for (size_t i = 0; i < count; ++i)
        section.push_back(pop_stack());
    return true;
}

bool script::op_checkmultisigverify(
    const message::transaction& parent_tx, uint32_t input_index)
{
    data_stack pubkeys;
    if (!read_section(pubkeys))
        return false;

    data_stack signatures;
    if (!read_section(signatures))
        return false;

    auto is_signature =
        [&signatures](const data_chunk& data)
        {
            return std::find(signatures.begin(), signatures.end(),
                data) != signatures.end();
        };
    script script_code;
    for (auto it = codehash_begin_; it != operations_.end(); ++it)
    {
        const operation op = *it;
        if (op.code == opcode::codeseparator)
            continue;
        if (is_signature(op.data))
            continue;
        script_code.push_operation(op);
    }

    // When checking the signatures against our public keys,
    // we always advance forwards until we either run out of pubkeys (fail)
    // or finish with our signatures (pass)
    auto pubkey_current = pubkeys.begin();
    for (const data_chunk& signature: signatures)
    {
        for (auto pubkey_iter = pubkey_current; ;)
        {
            if (check_signature(signature, *pubkey_iter,
                script_code, parent_tx, input_index))
            {
                pubkey_current = pubkey_iter;
                break;
            }
            // pubkeys are only exhausted when script failed
            ++pubkey_iter;
            if (pubkey_iter == pubkeys.end())
                return false;
        }
    }

    return true;
}

bool script::run_operation(const operation& op, 
        const message::transaction& parent_tx, uint32_t input_index)
{
    switch (op.code)
    {
        case opcode::zero:
        case opcode::special:
        case opcode::pushdata1:
        case opcode::pushdata2:
        case opcode::pushdata4:
            BITCOIN_ASSERT_MSG(op.code == opcode::bad_operation,
                "Invalid push operation for run_operation");
            return true;

        case opcode::negative_1:
            return op_negative_1();

        case opcode::reserved:
            return false;

        case opcode::op_1:
        case opcode::op_2:
        case opcode::op_3:
        case opcode::op_4:
        case opcode::op_5:
        case opcode::op_6:
        case opcode::op_7:
        case opcode::op_8:
        case opcode::op_9:
        case opcode::op_10:
        case opcode::op_11:
        case opcode::op_12:
        case opcode::op_13:
        case opcode::op_14:
        case opcode::op_15:
        case opcode::op_16:
            return op_x(op.code);

        case opcode::nop:
            return true;

        case opcode::ver:
            return false;

        case opcode::if_:
            return op_if();

        case opcode::notif:
            return op_notif();

        case opcode::verif:
        case opcode::vernotif:
            return false;

        case opcode::else_:
            return op_else();

        case opcode::endif:
            return op_endif();

        case opcode::verify:
            return op_verify();

        case opcode::toaltstack:
            return op_toaltstack();

        case opcode::fromaltstack:
            return op_fromaltstack();

        case opcode::ifdup:
            return op_ifdup();

        case opcode::depth:
            return op_depth();

        case opcode::drop:
            return op_drop();

        case opcode::dup:
            return op_dup();

        case opcode::nip:
            return op_nip();

        case opcode::over:
            return op_over();

        case opcode::pick:
            return op_pick();

        case opcode::roll:
            return op_roll();

        case opcode::size:
            return op_size();

        case opcode::reserved1:
        case opcode::reserved2:
            return false;

        case opcode::not_:
            return op_not();

        case opcode::boolor:
            return op_boolor();

        case opcode::min:
            return op_min();

        case opcode::sha256:
            return op_sha256();

        case opcode::hash160:
            return op_hash160();

        case opcode::equal:
            return op_equal();

        case opcode::equalverify:
            return op_equalverify();

        case opcode::add:
            return op_add();

        case opcode::greaterthanorequal:
            return op_greaterthanorequal();

        case opcode::codeseparator:
            // This is set in the main run(...) loop
            // codehash_begin_ is updated to the current
            // operations_ iterator
            BITCOIN_ASSERT_MSG(op.code == opcode::bad_operation,
                "Invalid operation (codeseparator) for run_operation");
            return true;

        case opcode::checksig:
            return op_checksig(parent_tx, input_index);

        case opcode::checksigverify:
            return op_checksigverify(parent_tx, input_index);

        case opcode::checkmultisig:
            return op_checkmultisig(parent_tx, input_index);

        case opcode::checkmultisigverify:
            return op_checkmultisigverify(parent_tx, input_index);

        case opcode::op_nop1:
        case opcode::op_nop2:
        case opcode::op_nop3:
        case opcode::op_nop4:
        case opcode::op_nop5:
        case opcode::op_nop6:
        case opcode::op_nop7:
        case opcode::op_nop8:
        case opcode::op_nop9:
        case opcode::op_nop10:
            return true;

        case opcode::raw_data:
            return false;

        default:
            log_fatal() << "Unimplemented operation <none " 
                << static_cast<int>(op.code) << ">";
            return false;
    }
    return false;
}

bool is_pubkey_type(const operation_stack& ops)
{
    return ops.size() == 2 &&
        ops[0].code == opcode::special &&
        ops[1].code == opcode::checksig;
}
bool is_pubkey_hash_type(const operation_stack& ops)
{
    return ops.size() == 5 &&
        ops[0].code == opcode::dup &&
        ops[1].code == opcode::hash160 &&
        ops[2].code == opcode::special &&
        ops[2].data.size() == 20 &&
        ops[3].code == opcode::equalverify &&
        ops[4].code == opcode::checksig;
}
bool is_script_hash_type(const operation_stack& ops)
{
    return ops.size() == 3 &&
        ops[0].code == opcode::hash160 &&
        ops[1].code == opcode::special &&
        ops[1].data.size() == 20 &&
        ops[2].code == opcode::equal;
}
bool is_multisig_type(const operation_stack& ops)
{
    return false;
}

payment_type script::type() const
{
    if (is_pubkey_type(operations_))
        return payment_type::pubkey;
    if (is_pubkey_hash_type(operations_))
        return payment_type::pubkey_hash;
    if (is_script_hash_type(operations_))
        return payment_type::script_hash;
    if (is_multisig_type(operations_))
        return payment_type::multisig;
    return payment_type::non_standard;
}

std::string opcode_to_string(opcode code)
{
    switch (code)
    {
        case opcode::zero:
            return "zero";
        case opcode::special:
            return "special";
        case opcode::pushdata1:
            return "pushdata1";
        case opcode::pushdata2:
            return "pushdata2";
        case opcode::pushdata4:
            return "pushdata4";
        case opcode::negative_1:
            return "-1";
        case opcode::reserved:
            return "reserved";
        case opcode::op_1:
            return "1";
        case opcode::op_2:
            return "2";
        case opcode::op_3:
            return "3";
        case opcode::op_4:
            return "4";
        case opcode::op_5:
            return "5";
        case opcode::op_6:
            return "6";
        case opcode::op_7:
            return "7";
        case opcode::op_8:
            return "8";
        case opcode::op_9:
            return "9";
        case opcode::op_10:
            return "10";
        case opcode::op_11:
            return "11";
        case opcode::op_12:
            return "12";
        case opcode::op_13:
            return "13";
        case opcode::op_14:
            return "14";
        case opcode::op_15:
            return "15";
        case opcode::op_16:
            return "16";
        case opcode::nop:
            return "nop";
        case opcode::ver:
            return "ver";
        case opcode::if_:
            return "if";
        case opcode::notif:
            return "notif";
        case opcode::else_:
            return "else";
        case opcode::endif:
            return "endif";
        case opcode::verify:
            return "verify";
        case opcode::toaltstack:
            return "toaltstack";
        case opcode::fromaltstack:
            return "fromaltstack";
        case opcode::ifdup:
            return "ifdup";
        case opcode::depth:
            return "depth";
        case opcode::drop:
            return "drop";
        case opcode::dup:
            return "dup";
        case opcode::nip:
            return "nip";
        case opcode::over:
            return "over";
        case opcode::pick:
            return "pick";
        case opcode::roll:
            return "roll";
        case opcode::size:
            return "size";
        case opcode::reserved1:
            return "reserved1";
        case opcode::reserved2:
            return "reserved2";
        case opcode::not_:
            return "not";
        case opcode::boolor:
            return "boolor";
        case opcode::min:
            return "min";
        case opcode::sha256:
            return "sha256";
        case opcode::hash160:
            return "hash160";
        case opcode::equal:
            return "equal";
        case opcode::equalverify:
            return "equalverify";
        case opcode::add:
            return "add";
        case opcode::greaterthanorequal:
            return "greaterthanorequal";
        case opcode::codeseparator:
            return "codeseparator";
        case opcode::checksig:
            return "checksig";
        case opcode::checksigverify:
            return "checksigverify";
        case opcode::checkmultisig:
            return "checkmultisig";
        case opcode::checkmultisigverify:
            return "checkmultisigverify";
        case opcode::op_nop1:
            return "op_nop1";
        case opcode::op_nop2:
            return "op_nop2";
        case opcode::op_nop3:
            return "op_nop3";
        case opcode::op_nop4:
            return "op_nop4";
        case opcode::op_nop5:
            return "op_nop5";
        case opcode::op_nop6:
            return "op_nop6";
        case opcode::op_nop7:
            return "op_nop7";
        case opcode::op_nop8:
            return "op_nop8";
        case opcode::op_nop9:
            return "op_nop9";
        case opcode::op_nop10:
            return "op_nop10";
        case opcode::raw_data:
            return "raw_data";
        default:
        {
            std::ostringstream ss;
            ss << "<none " << static_cast<int>(code) << ">";
            return ss.str();
        }
    }
}
opcode string_to_opcode(const std::string& code_repr)
{
    if (code_repr == "zero")
        return opcode::zero;
    else if (code_repr == "special")
        return opcode::special;
    else if (code_repr == "pushdata1")
        return opcode::pushdata1;
    else if (code_repr == "pushdata2")
        return opcode::pushdata2;
    else if (code_repr == "pushdata4")
        return opcode::pushdata4;
    else if (code_repr == "-1")
        return opcode::negative_1;
    else if (code_repr == "reserved")
        return opcode::reserved;
    else if (code_repr == "1")
        return opcode::op_1;
    else if (code_repr == "2")
        return opcode::op_2;
    else if (code_repr == "3")
        return opcode::op_3;
    else if (code_repr == "4")
        return opcode::op_4;
    else if (code_repr == "5")
        return opcode::op_5;
    else if (code_repr == "6")
        return opcode::op_6;
    else if (code_repr == "7")
        return opcode::op_7;
    else if (code_repr == "8")
        return opcode::op_8;
    else if (code_repr == "9")
        return opcode::op_9;
    else if (code_repr == "10")
        return opcode::op_10;
    else if (code_repr == "11")
        return opcode::op_11;
    else if (code_repr == "12")
        return opcode::op_12;
    else if (code_repr == "13")
        return opcode::op_13;
    else if (code_repr == "14")
        return opcode::op_14;
    else if (code_repr == "15")
        return opcode::op_15;
    else if (code_repr == "16")
        return opcode::op_16;
    else if (code_repr == "nop")
        return opcode::nop;
    else if (code_repr == "ver")
        return opcode::ver;
    else if (code_repr == "if")
        return opcode::if_;
    else if (code_repr == "notif")
        return opcode::notif;
    else if (code_repr == "verif")
        return opcode::verif;
    else if (code_repr == "vernotif")
        return opcode::vernotif;
    else if (code_repr == "else")
        return opcode::else_;
    else if (code_repr == "endif")
        return opcode::endif;
    else if (code_repr == "verify")
        return opcode::verify;
    else if (code_repr == "toaltstack")
        return opcode::toaltstack;
    else if (code_repr == "fromaltstack")
        return opcode::fromaltstack;
    else if (code_repr == "ifdup")
        return opcode::ifdup;
    else if (code_repr == "depth")
        return opcode::depth;
    else if (code_repr == "drop")
        return opcode::drop;
    else if (code_repr == "dup")
        return opcode::dup;
    else if (code_repr == "nip")
        return opcode::nip;
    else if (code_repr == "over")
        return opcode::over;
    else if (code_repr == "pick")
        return opcode::pick;
    else if (code_repr == "roll")
        return opcode::roll;
    else if (code_repr == "size")
        return opcode::size;
    else if (code_repr == "reserved1")
        return opcode::reserved1;
    else if (code_repr == "reserved2")
        return opcode::reserved2;
    else if (code_repr == "not")
        return opcode::not_;
    else if (code_repr == "boolor")
        return opcode::boolor;
    else if (code_repr == "min")
        return opcode::min;
    else if (code_repr == "sha256")
        return opcode::sha256;
    else if (code_repr == "hash160")
        return opcode::hash160;
    else if (code_repr == "equal")
        return opcode::equal;
    else if (code_repr == "equalverify")
        return opcode::equalverify;
    else if (code_repr == "add")
        return opcode::add;
    else if (code_repr == "greaterthanorequal")
        return opcode::greaterthanorequal;
    else if (code_repr == "codeseparator")
        return opcode::codeseparator;
    else if (code_repr == "checksig")
        return opcode::checksig;
    else if (code_repr == "checksigverify")
        return opcode::checksigverify;
    else if (code_repr == "checkmultisig")
        return opcode::checkmultisig;
    else if (code_repr == "checkmultisigverify")
        return opcode::checkmultisigverify;
    else if (code_repr == "op_nop1")
        return opcode::op_nop1;
    else if (code_repr == "op_nop2")
        return opcode::op_nop2;
    else if (code_repr == "op_nop3")
        return opcode::op_nop3;
    else if (code_repr == "op_nop4")
        return opcode::op_nop4;
    else if (code_repr == "op_nop5")
        return opcode::op_nop5;
    else if (code_repr == "op_nop6")
        return opcode::op_nop6;
    else if (code_repr == "op_nop7")
        return opcode::op_nop7;
    else if (code_repr == "op_nop8")
        return opcode::op_nop8;
    else if (code_repr == "op_nop9")
        return opcode::op_nop9;
    else if (code_repr == "op_nop10")
        return opcode::op_nop10;
    else if (code_repr == "raw_data")
        return opcode::raw_data;
    // ERROR: unknown... 
    return opcode::bad_operation;
}

std::string pretty(const script& source_script)
{
    const operation_stack& opers = source_script.operations();
    std::ostringstream ss;
    for (auto it = opers.begin(); it != opers.end(); ++it)
    {
        if (it != opers.begin())
            ss << " ";
        const operation& op = *it;
        if (op.data.empty())
            ss << opcode_to_string(op.code);
        else
            ss << "[ " << pretty_hex(op.data) << " ]";
    }
    return ss.str();
}

std::ostream& operator<<(std::ostream& stream, const script& source_script)
{
    stream << pretty(source_script);
    return stream;
}

// Read next n bytes while advancing iterator
// Used for seeing length of data to push to stack with pushdata2/4
template <typename Iterator>
inline data_chunk read_back_from_iterator(Iterator& it, size_t total)
{
    data_chunk number_bytes;
    for (size_t i = 0; i < total; ++i)
    {
        ++it;
        number_bytes.push_back(*it);
    }
    return number_bytes;
}

template <typename Iterator>
optional_number number_of_bytes_from_opcode(
    opcode code, byte raw_byte, Iterator& it)
{
    switch (code)
    {
        case opcode::zero:
        case opcode::special:
            return raw_byte;
        case opcode::pushdata1:
            ++it;
            return static_cast<uint8_t>(*it);
        case opcode::pushdata2:
            return cast_chunk<uint16_t>(read_back_from_iterator(it, 2));
        case opcode::pushdata4:
            return cast_chunk<uint32_t>(read_back_from_iterator(it, 4));
        default:
            return optional_number();
    }
    // Should never reach here!
}

script coinbase_script(const data_chunk& raw_script)
{
    script script_object;
    operation op;
    op.code = opcode::raw_data;
    op.data = raw_script;
    script_object.push_operation(op);
    return script_object;
}

template <typename Iterator>
bool read_push_data(Iterator& it, const Iterator& raw_script_end,
    size_t read_n_bytes, data_chunk& op_data)
{
    for (size_t byte_count = 0; byte_count < read_n_bytes; ++byte_count)
    {
        ++it;
        if (it == raw_script_end)
        {
            log_warning() << "Premature end of script.";
            return false;
        }
        op_data.push_back(*it);
    }
    return true;
}

script parse_script(const data_chunk& raw_script)
{
    script script_object;
    for (auto it = raw_script.begin(); it != raw_script.end(); ++it)
    {
        byte raw_byte = *it;
        operation op;
        op.code = static_cast<opcode>(raw_byte);
        // raw_byte is unsigned so it's always >= 0
        if (raw_byte == 0)
            op.code = opcode::zero;
        else if (0 < raw_byte && raw_byte <= 75)
            op.code = opcode::special;
        optional_number read_n_bytes =
            number_of_bytes_from_opcode(op.code, raw_byte, it);
        if (read_n_bytes && *read_n_bytes != 0)
        {
            // OP_0/OP_FALSE pushes a nothing to the stack
            // op.data = data_chunk();
            BITCOIN_ASSERT(read_n_bytes);
            if (!read_push_data(it, raw_script.cend(),
                    *read_n_bytes, op.data))
                return script();
        }
        script_object.push_operation(op);
    }
    return script_object;
}

inline data_chunk operation_metadata(const opcode code, size_t data_size)
{
    switch (code)
    {
        case opcode::pushdata1:
            return uncast_type<uint8_t>(data_size);
        case opcode::pushdata2:
            return uncast_type<uint16_t>(data_size);
        case opcode::pushdata4:
            return uncast_type<uint32_t>(data_size);
        default:
            return data_chunk();
    }
}
data_chunk save_script(const script& scr)
{
    const operation_stack& operations = scr.operations();
    if (operations.empty())
        return data_chunk();
    else if (operations[0].code == opcode::raw_data)
        return operations[0].data;
    data_chunk raw_script;
    for (const operation& op: scr.operations())
    {
        byte raw_byte = static_cast<byte>(op.code);
        if (op.code == opcode::special)
            raw_byte = op.data.size();
        raw_script.push_back(raw_byte);
        extend_data(raw_script, operation_metadata(op.code, op.data.size()));
        extend_data(raw_script, op.data);
    }
    return raw_script;
}

} // namespace libbitcoin

