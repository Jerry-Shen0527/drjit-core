#include "ssa.h"
#include "jit.h"
#include "log.h"
#include "eval.h"

/// Return the size of a given variable type
size_t jit_type_size(uint32_t type) {
    switch (type) {
        case EnokiType::UInt8:
        case EnokiType::Int8:
        case EnokiType::Bool:    return 1;
        case EnokiType::UInt16:
        case EnokiType::Int16:   return 2;
        case EnokiType::UInt32:
        case EnokiType::Int32:
        case EnokiType::Float32: return 4;
        case EnokiType::UInt64:
        case EnokiType::Int64:
        case EnokiType::Pointer:
        case EnokiType::Float64: return 8;
        default: jit_fail("jit_type_size(): invalid type!");
    }
}

/// Return the readable name for the given variable type
const char *jit_type_name(uint32_t type) {
    switch (type) {
        case EnokiType::Int8:    return "i8 "; break;
        case EnokiType::UInt8:   return "u8 "; break;
        case EnokiType::Int16:   return "i16"; break;
        case EnokiType::UInt16:  return "u16"; break;
        case EnokiType::Int32:   return "i32"; break;
        case EnokiType::UInt32:  return "u32"; break;
        case EnokiType::Int64:   return "i64"; break;
        case EnokiType::UInt64:  return "u64"; break;
        case EnokiType::Float16: return "f16"; break;
        case EnokiType::Float32: return "f32"; break;
        case EnokiType::Float64: return "f64"; break;
        case EnokiType::Bool:    return "msk"; break;
        case EnokiType::Pointer: return "ptr"; break;
        default: jit_fail("jit_type_name(): invalid type!");
    }
}

/// Access a variable by ID, terminate with an error if it doesn't exist
Variable *jit_var(uint32_t index) {
    auto it = state.variables.find(index);
    if (unlikely(it == state.variables.end()))
        jit_fail("jit_var(%u): unknown variable!", index);
    return &it.value();
}

/// Cleanup handler, called when the internal/external reference count reaches zero
void jit_var_free(uint32_t index, Variable *v) {
    jit_log(Trace, "jit_var_free(%u) = " PTR ".", index, v->data);

    VariableKey key(*v);
    state.variable_from_key.erase(key);

    uint32_t dep[3], extra_dep = v->extra_dep;
    memcpy(dep, v->dep, sizeof(uint32_t) * 3);

    // Release GPU memory
    if (v->free_variable && v->data)
        jit_free(v->data);

    // Free strings
    free(v->stmt);
    free(v->label);

    if (v->direct_pointer) {
        auto it = state.variable_from_ptr.find(v->data);
        if (unlikely(it == state.variable_from_ptr.end()))
            jit_fail("jit_var_free(): direct pointer not found!");
        state.variable_from_ptr.erase(it);
    }

    // Remove from hash table ('v' invalid from here on)
    state.variables.erase(index);

    // Decrease reference count of dependencies
    for (int i = 0; i < 3; ++i)
        jit_dec_ref_int(dep[i]);

    jit_dec_ref_ext(extra_dep);
}

/// Increase the external reference count of a given variable
void jit_inc_ref_ext(uint32_t index, Variable *v) {
    v->ref_count_ext++;
    jit_log(Trace, "jit_inc_ref_ext(%u) -> %u", index, v->ref_count_ext);
}

/// Increase the external reference count of a given variable
void jit_inc_ref_ext(uint32_t index) {
    if (index != 0)
        jit_inc_ref_ext(index, jit_var(index));
}

/// Increase the internal reference count of a given variable
void jit_inc_ref_int(uint32_t index, Variable *v) {
    v->ref_count_int++;
    jit_log(Trace, "jit_inc_ref_int(%u) -> %u", index, v->ref_count_int);
}

/// Increase the internal reference count of a given variable
void jit_inc_ref_int(uint32_t index) {
    if (index != 0)
        jit_inc_ref_int(index, jit_var(index));
}

/// Decrease the external reference count of a given variable
void jit_dec_ref_ext(uint32_t index) {
    if (index == 0 || state.variables.empty())
        return;
    Variable *v = jit_var(index);

    if (unlikely(v->ref_count_ext == 0))
        jit_fail("jit_dec_ref_ext(): variable %u has no external references!", index);

    jit_log(Trace, "jit_dec_ref_ext(%u) -> %u", index, v->ref_count_ext - 1);
    v->ref_count_ext--;

    if (v->ref_count_ext == 0)
        active_stream->todo.erase(index);

    if (v->ref_count_ext == 0 && v->ref_count_int == 0)
        jit_var_free(index, v);
}

/// Decrease the internal reference count of a given variable
void jit_dec_ref_int(uint32_t index) {
    if (index == 0 || state.variables.empty())
        return;
    Variable *v = jit_var(index);

    if (unlikely(v->ref_count_int == 0))
        jit_fail("jit_dec_ref_int(): variable %u has no internal references!", index);

    jit_log(Trace, "jit_dec_ref_int(%u) -> %u", index, v->ref_count_int - 1);
    v->ref_count_int--;

    if (v->ref_count_ext == 0 && v->ref_count_int == 0)
        jit_var_free(index, v);
}

/// Append the given variable to the instruction trace and return its ID
std::pair<uint32_t, Variable *> jit_trace_append(Variable &v) {
    v.stmt = strdup(v.stmt);

#if defined(ENOKI_CUDA)
    if (v.type != EnokiType::Float32) {
        char *offset = strstr(v.stmt, ".ftz");
        if (offset)
            strcat(offset, offset + 4);
    }
#endif

    // Check if this exact instruction already exists.
    VariableKey key(v);
    auto [key_it, key_inserted] = state.variable_from_key.try_emplace(key, 0);

    uint32_t idx;
    Variable *v_out;

    if (key_inserted) {
        idx = state.variable_index++;
        auto [var_it, var_inserted] = state.variables.try_emplace(idx, v);
        if (unlikely(!var_inserted))
            jit_fail("jit_trace_append(): could not append instruction!");
        key_it.value() = idx;
        v_out = &var_it.value();
    } else {
        free(v.stmt);
        idx = key_it.value();
        v_out = jit_var(idx);
    }

    return std::make_pair(idx, v_out);
}

/// Query the pointer variable associated with a given variable
void *jit_var_ptr(uint32_t index) {
    return jit_var(index)->data;
}

/// Query the size of a given variable
size_t jit_var_size(uint32_t index) {
    return jit_var(index)->size;
}

/// Set the size of a given variable (if possible, otherwise throw)
uint32_t jit_var_set_size(uint32_t index, size_t size, bool copy) {
    Variable *var = jit_var(index);
    if (var->size == size)
        return index;

    if (var->data != nullptr || var->ref_count_int > 0) {
        if (var->size == 1 && copy) {
            uint32_t index_new =
                jit_trace_append(var->type, "mov.$t1 $r1, $r2", index);
            jit_var(index_new)->size = size;
            jit_dec_ref_ext(index);
            return index_new;
        }

        jit_raise("cuda_var_set_size(): attempted to resize variable %u,"
                  "which was already allocated (current size = %zu, "
                  "requested size = %zu)",
                  index, var->size, size);
    }

    var->size = (uint32_t) size;
    jit_log(Debug, "jit_var_set_size(%u) -> %zu.", index, size);
    return index;
}

/// Query the descriptive label associated with a given variable
const char *jit_var_label(uint32_t index) {
    return jit_var(index)->label;
}

/// Assign a descriptive label to a given variable
void jit_var_set_label(uint32_t index, const char *label) {
    Variable *var = jit_var(index);
    free(var->label);
    var->label = strdup(label);
    jit_log(Debug, "jit_var_set_label(%u) -> \"%s.\"", index, label);
}

/// Append a variable to the instruction trace (no operands)
uint32_t jit_trace_append(uint32_t type, const char *stmt) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_trace_append(): device and stream must be set! "
                  "(call jit_device_set() beforehand)!");

    Variable v;
    v.type = type;
    v.size = 1;
    v.stmt = (char *) stmt;
    v.tsize = 1;

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_trace_append(%u): %s%s.",
            idx, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_inc_ref_ext(idx, vo);
    stream->todo.insert(idx);

    return idx;
}

/// Append a variable to the instruction trace (1 operand)
uint32_t jit_trace_append(uint32_t type, const char *stmt,
                          uint32_t arg1) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_trace_append(): device and stream must be set! "
                  "(call jit_device_set() beforehand)!");
    else if (unlikely(arg1 == 0))
        jit_raise("jit_trace_append(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(arg1);

    Variable v;
    v.type = type;
    v.size = v1->size;
    v.stmt = (char *) stmt;
    v.dep[0] = arg1;
    v.tsize = 1 + v1->tsize;

    if (unlikely(v1->dirty)) {
        jit_eval();
        v1 = jit_var(arg1);
        v.tsize = 2;
    }

    jit_inc_ref_int(arg1, v1);

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_trace_append(%u <- %u): %s%s.",
            idx, arg1, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_inc_ref_ext(idx, vo);
    stream->todo.insert(idx);

    return idx;
}

/// Append a variable to the instruction trace (2 operands)
uint32_t jit_trace_append(uint32_t type, const char *stmt,
                          uint32_t arg1, uint32_t arg2) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_trace_append(): device and stream must be set! "
                  "(call jit_device_set() beforehand)!");
    else if (unlikely(arg1 == 0 || arg2 == 0))
        jit_raise("jit_trace_append(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(arg1),
             *v2 = jit_var(arg2);

    Variable v;
    v.type = type;
    v.size = std::max(v1->size, v2->size);
    v.stmt = (char *) stmt;
    v.dep[0] = arg1;
    v.dep[1] = arg2;
    v.tsize = 1 + v1->tsize + v2->tsize;

    if (unlikely((v1->size != 1 && v1->size != v.size) ||
                 (v2->size != 1 && v2->size != v.size))) {
        jit_raise(
            "jit_trace_append(): arithmetic involving arrays of incompatible "
            "size (%zu and %zu). The instruction was \"%s\".",
            v1->size, v2->size, stmt);
    } else if (unlikely(v1->dirty || v2->dirty)) {
        jit_eval();
        v1 = jit_var(arg1);
        v2 = jit_var(arg2);
        v.tsize = 3;
    }

    jit_inc_ref_int(arg1, v1);
    jit_inc_ref_int(arg2, v2);

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_trace_append(%u <- %u, %u): %s%s.",
            idx, arg1, arg2, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_inc_ref_ext(idx, vo);
    stream->todo.insert(idx);

    return idx;
}

/// Append a variable to the instruction trace (3 operands)
uint32_t jit_trace_append(uint32_t type, const char *stmt,
                          uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_trace_append(): device and stream must be set! "
                  "(call jit_device_set() beforehand)!");
    else if (unlikely(arg1 == 0 || arg2 == 0 || arg3 == 0))
        jit_raise("jit_trace_append(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(arg1),
             *v2 = jit_var(arg2),
             *v3 = jit_var(arg3);

    Variable v;
    v.type = type;
    v.size = std::max({ v1->size, v2->size, v3->size });
    v.stmt = (char *) stmt;
    v.dep[0] = arg1;
    v.dep[1] = arg2;
    v.dep[2] = arg3;
    v.tsize = 1 + v1->tsize + v2->tsize + v3->tsize;

    if (unlikely((v1->size != 1 && v1->size != v.size) ||
                 (v2->size != 1 && v2->size != v.size) ||
                 (v3->size != 1 && v3->size != v.size))) {
        jit_raise(
            "jit_trace_append(): arithmetic involving arrays of incompatible "
            "size (%zu, %zu, and %zu). The instruction was \"%s\".",
            v1->size, v2->size, v3->size, stmt);
    } else if (unlikely(v1->dirty || v2->dirty || v3->dirty)) {
        jit_eval();
        v1 = jit_var(arg1);
        v2 = jit_var(arg2);
        v3 = jit_var(arg3);
        v.tsize = 4;
    }

    jit_inc_ref_int(arg1, v1);
    jit_inc_ref_int(arg2, v2);
    jit_inc_ref_int(arg3, v3);

#if defined(ENOKI_CUDA)
    if (strstr(stmt, "st.global") || strstr(stmt, "atom.global.add")) {
        v.extra_dep = state.scatter_gather_operand;
        jit_inc_ref_ext(v.extra_dep);
    }
#endif

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_trace_append(%u <- %u, %u, %u): %s%s.",
            idx, arg1, arg2, arg3, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_inc_ref_ext(idx, vo);
    stream->todo.insert(idx);

    return idx;
}

/// Register an existing variable with the JIT compiler
uint32_t jit_var_register(uint32_t type, void *ptr,
                          size_t size, bool free) {
    if (unlikely(size == 0))
        jit_raise("jit_var_register: size must be > 0!");

    Variable v;
    v.type = type;
    v.data = ptr;
    v.size = (uint32_t) size;
    v.free_variable = free;
    v.tsize = 1;

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_var_register(%u): " PTR ", size=%zu, free=%i.",
            idx, ptr, size, (int) free);

    jit_inc_ref_ext(idx, vo);

    return idx;
}

/// Register pointer literal as a special variable within the JIT compiler
uint32_t jit_var_register_ptr(const void *ptr) {
    auto it = state.variable_from_ptr.find(ptr);
    if (it != state.variable_from_ptr.end()) {
        uint32_t idx = it.value();
        jit_inc_ref_ext(idx);
        return idx;
    }

    Variable v;
    v.type = EnokiType::Pointer;
    v.data = (void *) ptr;
    v.size = 1;
    v.tsize = 0;
    v.free_variable = false;
    v.direct_pointer = true;

    auto [idx, vo] = jit_trace_append(v);
    jit_log(Debug, "jit_var_register_ptr(%u): " PTR ".", idx, ptr);

    jit_inc_ref_ext(idx, vo);
    state.variable_from_ptr[ptr] = idx;
    return idx;
}

/// Copy a memory region onto the device and return its variable index
uint32_t jit_var_copy_to_device(uint32_t type,
                                const void *value,
                                size_t size) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_fail("jit_var_copy_to_device(): device and stream must be set! "
                 "(call jit_device_set() beforehand)!");

    size_t total_size = size * jit_type_size(type);

    void *host_ptr   = jit_malloc(AllocType::HostPinned, total_size),
         *device_ptr = jit_malloc(AllocType::Device, total_size);

    memcpy(host_ptr, value, total_size);
    cuda_check(cudaMemcpyAsync(device_ptr, host_ptr, total_size,
                               cudaMemcpyHostToDevice, stream->handle));

    jit_free(host_ptr);
    uint32_t idx = jit_var_register(type, device_ptr, size, true);
    jit_log(Debug, "jit_var_copy_to_device(%u, %zu).", idx, size);
    return idx;
}

/// Migrate a variable to a different flavor of memory
void jit_var_migrate(uint32_t idx, AllocType type) {
    if (idx == 0)
        return;

    Variable *v = jit_var(idx);
    if (v->data == nullptr || v->dirty) {
        jit_eval();
        v = jit_var(idx);
    }

    jit_log(Debug, "jit_var_migrate(%u, " PTR ") -> %s", idx, v->data,
            alloc_type_names[(int) type]);

    v->data = jit_malloc_migrate(v->data, type);
}

void jit_var_mark_side_effect(uint32_t index) {
    jit_log(Debug, "jit_var_mark_side_effect(%u)", index);
    jit_var(index)->side_effect = true;
}

void jit_var_mark_dirty(uint32_t index) {
    jit_log(Debug, "jit_var_mark_dirty(%u)", index);
    jit_var(index)->dirty = true;
}

const char *jit_whos() {
    buffer.clear();
    buffer.put("\n  ID        Type   E/I Refs   Size        Memory     Ready    Label");
    buffer.put("\n  =================================================================\n");

    std::vector<uint32_t> indices;
    indices.reserve(state.variables.size());
    for (const auto& it : state.variables)
        indices.push_back(it.first);
    std::sort(indices.begin(), indices.end());

    size_t mem_size_scheduled = 0,
           mem_size_ready = 0,
           mem_size_arith = 0;

    for (uint32_t index: indices) {
        const Variable *v = jit_var(index);
        size_t mem_size = v->size * jit_type_size(v->type);

        buffer.fmt("  %-9u %s    ", index, jit_type_name(v->type));
        size_t sz = buffer.fmt("%u / %u", v->ref_count_ext, v->ref_count_int);
        buffer.fmt("%*s%-12u%-12s[%c]     %s\n", 11 - sz, "", v->size,
                   jit_mem_string(mem_size), v->data ? 'x' : ' ',
                   v->label ? v->label : "");

        if (v->data) {
            mem_size_ready += mem_size;
        } else {
            if (v->ref_count_ext == 0)
                mem_size_arith += mem_size;
            else
                mem_size_scheduled += mem_size;
        }
    }

    buffer.put("  =================================================================\n\n");
    buffer.put("  JIT compiler\n");
    buffer.put("  ============\n");
    buffer.fmt("   - Memory usage (ready)     : %s.\n",
               jit_mem_string(mem_size_ready));
    buffer.fmt("   - Memory usage (scheduled) : %s + %s = %s.\n",
               std::string(jit_mem_string(mem_size_ready)).c_str(),
               std::string(jit_mem_string(mem_size_scheduled)).c_str(),
               std::string(jit_mem_string(mem_size_ready + mem_size_scheduled)).c_str());
    buffer.fmt("   - Memory savings           : %s.\n\n",
               jit_mem_string(mem_size_arith));

    buffer.put("  Memory allocator\n");
    buffer.put("  ================\n");
    for (int i = 0; i < 5; ++i)
        buffer.fmt("   - %-20s: %s used (max. %s).\n",
                   alloc_type_names[i],
                   std::string(jit_mem_string(state.alloc_usage[i])).c_str(),
                   std::string(jit_mem_string(state.alloc_watermark[i])).c_str());

    return buffer.get();
}


