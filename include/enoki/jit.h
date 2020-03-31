/*
    enoki/jit.h -- Self-contained JIT compiler for CUDA & LLVM.

    This library implements a self-contained tracing JIT compiler that supports
    both CUDA PTX and LLVM IR as intermediate languages. It takes care of many
    tricky aspects, such as asynchronous memory allocation and release,
    multi-device computation, kernel caching and reuse, common subexpression
    elimination, etc.

    While the library is internally implemented using C++17, this header file
    provides a compact C99-compatible API that can be used to access all
    functionality. The library is thread-safe: multiple threads can
    simultaneously dispatch computation to one or more CPUs/GPUs.

    As an alternative to the fairly low-level API defined here, you may prefer
    to use the functionality in 'enoki/jitvar.h', which provides a header-only
    C++ array class with operator overloading, which dispatches to the C API.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <stdlib.h>
#include <stdint.h>

#define JITC_EXPORT    __attribute__ ((visibility("default")))
#if defined(__cplusplus)
#  define JITC_CONSTEXPR constexpr
#else
#  define JITC_CONSTEXPR inline
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// ====================================================================
//         Initialization, device enumeration, and management
// ====================================================================

/**
 * \brief Initialize core data structures of the JIT compiler
 *
 * This function must be called before using any of the remaining API. It
 * detects the available devices and initializes them for later use. It does
 * nothing when initialization has already occurred. Note that it is possible
 * to re-initialize the JIT following a call to \ref jitc_shutdown(), which can
 * be useful to start from a known state, e.g., in testcases.
 *
 * The \c llvm and \c cuda arguments should be set to \c 1 to initialize the
 * corresponding backend, and \c 0 otherwise.
 */
extern JITC_EXPORT void jitc_init(int llvm, int cuda);

/**
 * \brief Launch an ansynchronous thread that will execute jitc_init() and
 * return immediately
 *
 * On machines with several GPUs, \ref jitc_init() sets up a CUDA environment
 * on all devices, which can be a rather slow operation (e.g. 1 second). This
 * function provides a convenient alternative to hide this latency, for
 * instance when importing this library from an interactive Python session
 * which doesn't actually need the JIT right away.
 *
 * The \c llvm and \c cuda arguments should be set to \c 1 to initialize the
 * corresponding backend, and \c 0 otherwise.
 *
 * It is safe to call jitc_* API functions following \ref jitc_init_async(),
 * since it acquires a lock to the internal data structures.
 */
extern JITC_EXPORT void jitc_init_async(int llvm, int cuda);

/// Check whether the LLVM backend was successfully initialized
extern JITC_EXPORT int jitc_has_llvm();

/// Check whether the CUDA backend was successfully initialized
extern JITC_EXPORT int jitc_has_cuda();

/**
 * \brief Release resources used by the JIT compiler, and report reference leaks.
 *
 * If <tt>light=1</tt>, this function performs a "light" shutdown, which
 * flushes any still running computation and releases unused memory back to the
 * OS or GPU. It will also warn about leaked variables and memory allocations.
 *
 * If <tt>light=0</tt>, the function furthermore completely unloads the LLVM or
 * CUDA backends. This frees up more memory but means that a later call to \ref
 * jitc_init() or \ref jitc_init_async() will be slow.
 */
extern JITC_EXPORT void jitc_shutdown(int light);

/**
 * \brief Return the number of target devices
 *
 * This function returns the number of available devices. Note that this refers
 * to the number of compatible CUDA devices, excluding the host CPU.
 */
extern JITC_EXPORT int32_t jitc_device_count();

/**
 * Set the currently active device and stream
 *
 * \param device
 *     Specifies the device index, a number between -1 and
 *     <tt>jitc_device_count() - 1</tt>. The number <tt>-1</tt> indicates that
 *     execution should take place on the host CPU (via LLVM). <tt>0</tt> is
 *     the first GPU (execution via CUDA), <tt>1</tt> is the second GPU, etc.
 *
 * \param stream
 *     CUDA devices can concurrently execute computation from multiple streams.
 *     When accessing the JIT compiler in a multi-threaded program, each thread
 *     should specify a separate stream to exploit this additional opportunity
 *     for parallelization. When executing on the host CPU
 *     (<tt>device==-1</tt>), this argument is ignored.
 */
extern JITC_EXPORT void jitc_device_set(int32_t device, uint32_t stream);

/**
 * \brief Override the target CPU, features, and vector witdth of the LLVM backend
 *
 * The LLVM backend normally generates code for the detected native hardware
 * architecture akin to compiling with <tt>-march=native</tt>. This function
 * can be used to change the following code generation-related parameters:
 *
 * \param target_cpu
 *     Target CPU (e.g. <tt>haswell</tt>)
 *
 * \param target_features
 *     Comma-separated list of LLVM feature flags (e.g. <tt>+avx512f</tt>).
 *     This should be set to <tt>nullptr</tt> if you do not wish to specify
 *     individual featureas.
 *
 * \param vector_width
 *     Width of vector registers (e.g. 8 for AVX)
 */
extern JITC_EXPORT void jitc_llvm_set_target(const char *target_cpu,
                                             const char *target_features,
                                             uint32_t vector_width);

/**
 * \brief Convenience function for intrinsic function selection
 *
 * Returns \c 1 if the current vector width is is at least as large as a
 * provided value, and when the host CPU provides a given target feature (e.g.
 * "+avx512f").
 */
extern JITC_EXPORT int jitc_llvm_if_at_least(uint32_t vector_width,
                                             const char *feature);

/**
 * \brief Dispatch computation to multiple parallel streams?
 *
 * The JIT compiler attempts to fuse all queued computation into a single
 * kernel to maximize efficiency. But computation involving arrays of different
 * size must necessarily run in separate kernels, which means that it is
 * serialized if taking place within the same device and stream. If desired,
 * jitc_eval() can detect this and dispatch multiple kernels to separate
 * streams that execute in parallel. The default is \c 1 (i.e. to enable
 * parallel dispatch).
 *
 * This feature is currently only used in CPU mode.
 */
extern JITC_EXPORT void jitc_parallel_set_dispatch(int enable);

/// Return whether or not parallel dispatch is enabled. Returns \c 0 or \c 1.
extern JITC_EXPORT int jitc_parallel_dispatch();
/**
 * \brief Wait for all computation on the current stream to finish
 *
 * No-op when the target device is the host CPU.
 */
extern JITC_EXPORT void jitc_sync_stream();

/**
 * \brief Wait for all computation on the current device to finish
 *
 * No-op when the target device is the host CPU.
 */
extern JITC_EXPORT void jitc_sync_device();

// ====================================================================
//                        Logging infrastructure
// ====================================================================

#if defined(__cplusplus)
enum class LogLevel : uint32_t {
    Disable, Error, Warn, Info, Debug, Trace
};
#else
enum LogLevel {
    LogLevelDisable, LogLevelError, LogLevelWarn,
    LogLevelInfo, LogLevelDebug, LogLevelTrace
};
#endif

/**
 * \brief Control the destination of log messages (stderr)
 *
 * By default, this library prints all log messages to the console (\c stderr).
 * This function can be used to control the minimum log level for such output
 * or prevent it entirely. In the latter case, you may wish to enable logging
 * via a callback in \ref jitc_set_log_callback(). Both destinations can also
 * be enabled simultaneously, pontentially using different log levels.
 */
extern JITC_EXPORT void jitc_log_set_stderr(enum LogLevel level);

/// Return the currently set minimum log level for output to \c stderr
extern JITC_EXPORT enum LogLevel jitc_log_stderr();


/**
 * \brief Control the destination of log messages (callback)
 *
 * This function can be used to specify an optional callback that will be
 * invoked with the contents of library log messages, whose severity matches or
 * exceeds the specified \c level.
 */
typedef void (*LogCallback)(enum LogLevel, const char *);
extern JITC_EXPORT void jitc_set_log_callback(enum LogLevel level, LogCallback callback);

/// Return the currently set minimum log level for output to a callback
extern JITC_EXPORT enum LogLevel jitc_log_callback();

/// Print a log message with the specified log level and message
extern JITC_EXPORT void jitc_log(enum LogLevel level, const char* fmt, ...);

/// Raise an exception message with the specified message
extern JITC_EXPORT void jitc_raise(const char* fmt, ...);

/// Terminate the application due to a non-recoverable error
extern JITC_EXPORT void jitc_fail(const char* fmt, ...);

// ====================================================================
//                         Memory allocation
// ====================================================================

#if defined(__cplusplus)
enum class AllocType : uint32_t {
    /// Memory that is located on the host (i.e., the CPU)
    Host,

    /**
     * Memory on the host that is "pinned" and thus cannot be paged out.
     * Host-pinned memory is accessible (albeit slowly) from CUDA-capable GPUs
     * as part of the unified memory model, and it also can be a source or
     * destination of asynchronous host <-> device memcpy operations.
     */
    HostPinned,

    /// Memory that is located on a device (i.e., one of potentially several GPUs)
    Device,

    /// Memory that is mapped in the address space of both host & all GPU devices
    Managed,

    /// Like \c Managed, but more efficient when almost all accesses are reads
    ManagedReadMostly,

    /// Number of AllocType entries
    Count
};
#else
enum AllocType {
    AllocTypeHost,
    AllocTypeHostPinned,
    AllocTypeDevice,
    AllocTypeManaged,
    AllocTypeManagedReadMostly,
    AllocTypeCount,
};
#endif

/**
 * \brief Allocate memory of the specified type
 *
 * Under the hood, Enoki implements a custom allocation scheme that tries to
 * reuse allocated memory regions instead of giving them back to the OS/GPU.
 * This eliminates inefficient synchronization points in the context of CUDA
 * programs, and it can also improve performance on the CPU when working with
 * large allocations.
 *
 * The returned pointer is guaranteed to be sufficiently aligned for any kind
 * of use.
 *
 */
extern JITC_EXPORT void *jitc_malloc(enum AllocType type, size_t size)
    __attribute__((malloc));

/**
 * \brief Release a given pointer asynchronously
 *
 * For CPU-only arrays (\ref AllocType::Host), <tt>jitc_free()</tt> is
 * synchronous and very similar to <tt>free()</tt>, except that the released
 * memory is placed in Enoki's internal allocation cache instead of being
 * returned to the OS. The function \ref jitc_malloc_trim() can optionally be
 * called to also clear this cache.
 *
 * When \c ptr is a GPU-accessible pointer (\ref AllocType::Device, \ref
 * AllocType::HostPinned, \ref AllocType::Managed, \ref
 * AllocType::ManagedReadMostly), the associated memory region is quite likely
 * still being used by a running kernel, and it is therefore merely *scheduled*
 * to be reclaimed once this kernel finishes. Allocation thus runs in the
 * execution context of a CUDA device, i.e., it is asynchronous with respect to
 * the CPU. This means that some care must be taken in the context of programs
 * that use multiple streams or GPUs: it is not permissible to e.g. allocate
 * memory in one context, launch a kernel using it, then immediately switch
 * context to another GPU or stream on the same GPU via \ref jitc_set_device()
 * and release the memory region there. Calling \ref jitc_sync_stream() or
 * \ref jitc_sync_device() before context switching defuses this situation.
 */
extern JITC_EXPORT void jitc_free(void *ptr);

/**
 * \brief Asynchronously change the flavor of an allocated memory region and
 * return the new pointer
 *
 * The operation is *always* asynchronous and, hence, will need to be followed
 * by an explicit synchronization via \ref jitc_sync_stream() if memory is
 * migrated from the GPU to the CPU and expected to be accessed on the CPU
 * before the transfer has finished. Nothing needs to be done in the other
 * direction, e.g. when migrating memory that is subsequently accessed by
 * a GPU kernel.
 *
 * When no migration is necessary, the function simply returns the input
 * pointer. Otherwise, it returns a new pointer and asynchronously frees the
 * old one via (via \ref jitc_free()). When both source and target are of
 * type \ref AllocType::Device, and when the currently active device
 * (determined by the last call to \ref jitc_device_set()) does not match the
 * device associated with the allocation, a peer-to-peer migration is
 * performed.
 */
extern JITC_EXPORT void* jitc_malloc_migrate(void *ptr, enum AllocType type);

/// Release all currently unused memory to the GPU / OS
extern JITC_EXPORT void jitc_malloc_trim();

/**
 * \brief Asynchronously prefetch a memory region allocated using \ref
 * jitc_malloc() so that it is available on a specified device
 *
 * This operation prefetches a memory region so that it is available on the CPU
 * (<tt>device==-1</tt>) or specified CUDA device (<tt>device&gt;=0</tt>). This
 * operation only make sense for allocations of type <tt>AllocType::Managed<tt>
 * and <tt>AllocType::ManagedReadMostly</tt>. In the former case, the memory
 * region will be fully migrated to the specified device, and page mappings
 * established elswhere are cleared. For the latter, a read-only copy is
 * created on the target device in addition to other copies that may exist
 * elsewhere.
 *
 * The function also takes a special argument <tt>device==-2</tt>, which
 * creates a read-only mapping on *all* available GPUs.
 *
 * The prefetch operation is enqueued on the current device and stream and runs
 * asynchronously with respect to the CPU, hence a \ref jitc_sync_stream()
 * operation is advisable if data is <tt>target==-1</tt> (i.e. prefetching into
 * CPU memory).
 */
extern JITC_EXPORT void jitc_malloc_prefetch(void *ptr, int device);


// ====================================================================
//                          Pointer registry
// ====================================================================

/**
 * \brief Register a pointer with Enoki's pointer registry
 *
 * Enoki provides a central registry that maps registered pointer values to
 * low-valued 32-bit IDs. The main application is efficient virtual function
 * dispatch via \ref jitc_vcall(), through the registry could be used for other
 * applications as well.
 *
 * This function registers the specified pointer \c ptr with the registry,
 * returning the associated ID value, which is guaranteed to be unique within
 * the specified domain \c domain. The domain is normally an identifier that is
 * associated with the "flavor" of the pointer (e.g. instances of a particular
 * class), and which ensures that the returned ID values are as low as
 * possible.
 *
 * Caution: for reasons of efficiency, the \c domain parameter is assumed to a
 * static constant that will remain alive. The RTTI identifier
 * <tt>typeid(MyClass).name()<tt> is a reasonable choice that satisfies this
 * requirement.
 *
 * Returns zero if <tt>ptr == nullptr</tt> and throws if the pointer is already
 * registered (with *any* domain).
 */
extern JITC_EXPORT uint32_t jitc_registry_put(const char *domain, void *ptr);

/**
 * \brief Remove a pointer from the registry
 *
 * No-op if <tt>ptr == nullptr</tt>. Throws an exception if the pointer is not
 * currently registered.
 */
extern JITC_EXPORT void jitc_registry_remove(void *ptr);

/**
 * \brief Query the ID associated a registered pointer
 *
 * Returns 0 if <tt>ptr==nullptr</tt> and throws if the pointer is not known.
 */
extern JITC_EXPORT uint32_t jitc_registry_get_id(const void *ptr);

/**
 * \brief Query the domain associated a registered pointer
 *
 * Returns \c nullptr if <tt>ptr==nullptr</tt> and throws if the pointer is not
 * known.
 */
extern JITC_EXPORT const char *jitc_registry_get_domain(const void *ptr);

/**
 * \brief Query the pointer associated a given domain and ID
 *
 * Returns \c nullptr if <tt>id==0</tt> and throws if the (domain, ID)
 * combination is not known.
 */
extern JITC_EXPORT void *jitc_registry_get_ptr(const char *domain, uint32_t id);

/// Provide a bound (<=) on the largest ID associated with a domain
extern JITC_EXPORT uint32_t jitc_registry_get_max(const char *domain);

/**
 * \brief Compact the registry and release unused IDs
 *
 * It's a good idea to call this function following a large number of calls to
 * \ref jitc_registry_remove().
 */
extern JITC_EXPORT void jitc_registry_trim();

// ====================================================================
//                        Variable management
// ====================================================================

#if defined(__cplusplus)
/// Variable types supported by the JIT compiler
enum class VarType : uint32_t {
    Invalid, Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64,
    Float16, Float32, Float64, Bool, Pointer, Count
};
#else
enum VarType {
    VarTypeInvalid, VarTypeInt8, VarTypeUInt8, VarTypeInt16, VarTypeUInt16,
    VarTypeInt32, VarTypeUInt32, VarTypeInt64, VarTypeUInt64, VarTypeFloat16,
    VarTypeFloat32, VarTypeFloat64, VarTypeBool, VarTypePointer, VarTypeCount
};
#endif

/// Convenience function to check for an integer operand
JITC_CONSTEXPR int jitc_is_integral(enum VarType type) {
#if defined(__cplusplus)
    return ((uint32_t) type >= (uint32_t) VarType::Int8 &&
            (uint32_t) type <= (uint32_t) VarType::UInt64) ? 1 : 0;
#else
    return ((uint32_t) type >= (uint32_t) VarTypeInt8 &&
            (uint32_t) type <= (uint32_t) VarTypeUInt64) ? 1 : 0;
#endif
}

/// Convenience function to check for a floating point operand
JITC_CONSTEXPR uint32_t jitc_is_floating_point(enum VarType type) {
#if defined(__cplusplus)
    return ((uint32_t) type >= (uint32_t) VarType::Float16 &&
            (uint32_t) type <= (uint32_t) VarType::Float64) ? 1 : 0;
#else
    return ((uint32_t) type >= (uint32_t) VarTypeFloat16 &&
            (uint32_t) type <= (uint32_t) VarTypeFloat64) ? 1 : 0;
#endif
}

/// Convenience function to check for an arithmetic operand
JITC_CONSTEXPR uint32_t jitc_is_arithmetic(enum VarType type) {
#if defined(__cplusplus)
    return ((uint32_t) type >= (uint32_t) VarType::Int8 &&
            (uint32_t) type <= (uint32_t) VarType::Float64) ? 1 : 0;
#else
    return ((uint32_t) type >= (uint32_t) VarTypeInt8 &&
            (uint32_t) type <= (uint32_t) VarTypeFloat64) ? 1 : 0;
#endif
}

/// Convenience function to check for a mask operand
JITC_CONSTEXPR int jitc_is_mask(enum VarType type) {
#if defined(__cplusplus)
    return type == VarType::Bool;
#else
    return type == VarTypeBool;
#endif
}

/**
 * Register an existing memory region as a variable in the JIT compiler, and
 * return its index. Its external reference count is initialized to \c 1.
 *
 * \param type
 *    Type of the variable to be created, see \ref VarType for details.
 *
 * \param ptr
 *    Point of the memory region
 *
 * \param size
 *    Number of elements, rather than the size in bytes
 *
 * \param free
 *    If free != 0, the JIT compiler will free the memory region via
 *    \ref jitc_free() once it goes out of scope.
 *
 * \sa jitc_var_copy()
 */
extern JITC_EXPORT uint32_t jitc_var_map(enum VarType type, void *ptr,
                                         uint32_t size, int free);


/**
 * Copy a memory region from the host to onto the device and return its
 * variable index. Its external reference count is initialized to \c 1.
 *
 * \param type
 *    Type of the variable to be created, see \ref VarType for details.
 *
 * \param ptr
 *    Point of the memory region
 *
 * \param size
 *    Number of elements, rather than the size in bytes
 *
 * \sa jitc_var_map()
 */
extern JITC_EXPORT uint32_t jitc_var_copy(enum VarType type,
                                          const void *ptr,
                                          uint32_t size);

/**
 * Register a pointer literal as a variable within the JIT compiler
 *
 * When working with memory (gathers, scatters) using the JIT compiler, we must
 * often refer to memory addresses. These addresses should not baked nto the
 * JIT-compiled code, since they change over time, which limits the ability to
 * re-use of compiled kernels.
 *
 * This function registers a pointer literal that accomplishes this. It is
 * functionally equivalent to
 *
 * \code
 * void *my_ptr = ...;
 * uint32_t index = jitc_var_copy(VarType::Pointer, &my_ptr, 1);
 * \endcode
 *
 * but results in more efficient generated code.
 */
extern JITC_EXPORT uint32_t jitc_var_copy_ptr(const void *ptr);

/**
 * \brief Append a statement to the instruction trace.
 *
 * This function takes a statement in an intermediate language (CUDA PTX or
 * LLVM IR) and appends it to the list of currently queued operations. It
 * returns the index of the variable that will store the result of the
 * statement, whose external reference count is initialized to \c 1.
 *
 * This function assumes that the operation does not access any operands. See
 * the other <tt>jitc_trace_*</tt> functions for IR statements with 1 to 3
 * additional operands. In these latter versions, the string \c stmt may
 * contain special dollar-prefixed expressions (<tt>$rN</tt>, <tt>$tN</tt>, or
 * <tt>$bN</tt>, where <tt>N</tt> ranges from 0-4) to refer to operands and
 * their types. During compilation, these will then be rewritten into a
 * register name of the variable (<tt>r</tt>), its type (<tt>t</tt>), or a
 * generic binary type of matching size (<tt>b</tt>). Index <tt>0</tt> refers
 * to the variable being generated, while indices <tt>1<tt>-<tt>3</tt> refer to
 * the operands. For instance, a PTX integer addition would be encoded as
 * follows:
 *
 * \code
 * uint32_t result = jitc_trace_append_2(VarType::Int32,
 *                                       "add.$t0 $r0, $r1, $r2",
 *                                       1, op1, op2);
 * \endcode
 *
 * \param type
 *    Type of the variable to be created, see \ref VarType for details.
 *
 * \param stmt
 *    Intermediate language statement.
 *
 * \param stmt_static
 *    When 'stmt' is a static string stored in the data segment of the
 *    executable, it is not necessary to make a copy. In this case, set
 *    <tt>stmt_static == 1</tt>, and <tt>0</tt> otherwise.
 *
 * \param size
 *    Size of the resulting variable. The size is automatically inferred from
 *    the operands and must only be specified for the zero-argument form.
 */
extern JITC_EXPORT uint32_t jitc_trace_append_0(enum VarType type,
                                                const char *stmt,
                                                int stmt_static,
                                                uint32_t size);

/// Append a variable to the instruction trace (1 operand)
extern JITC_EXPORT uint32_t jitc_trace_append_1(enum VarType type,
                                                const char *stmt,
                                                int stmt_static,
                                                uint32_t op1);

/// Append a variable to the instruction trace (2 operands)
extern JITC_EXPORT uint32_t jitc_trace_append_2(enum VarType type,
                                                const char *stmt,
                                                int stmt_static,
                                                uint32_t op1,
                                                uint32_t op2);

/// Append a variable to the instruction trace (3 operands)
extern JITC_EXPORT uint32_t jitc_trace_append_3(enum VarType type,
                                                const char *stmt,
                                                int stmt_static,
                                                uint32_t op1,
                                                uint32_t op2,
                                                uint32_t op3);

/// Increase the external reference count of a given variable
extern JITC_EXPORT void jitc_var_inc_ref_ext(uint32_t index);

/// Decrease the external reference count of a given variable
extern JITC_EXPORT void jitc_var_dec_ref_ext(uint32_t index);

/// Query the pointer variable associated with a given variable
extern JITC_EXPORT void *jitc_var_ptr(uint32_t index);

/// Query the size of a given variable
extern JITC_EXPORT uint32_t jitc_var_size(uint32_t index);

/**
 * Set the size of a given variable (if possible, otherwise throw an
 * exception.)
 *
 * \param index
 *     Index of the variable, whose size should be modified
 *
 * \param size
 *     Target size value
 *
 * \param copy
 *     When the variable has already been evaluated and is a scalar, Enoki can
 *     optionally perform a copy instead of failing if <tt>copy != 0</tt>.
 *
 * Returns the ID of the changed or new variable
 */
extern JITC_EXPORT uint32_t jitc_var_set_size(uint32_t index,
                                              uint32_t size,
                                              int copy);

/// Assign a descriptive label to a given variable
extern JITC_EXPORT void jitc_var_set_label(uint32_t index, const char *label);

/// Query the descriptive label associated with a given variable
extern JITC_EXPORT const char *jitc_var_label(uint32_t index);

/**
 * \brief Asynchronously migrate a variable to a different flavor of memory
 *
 * The operation is asynchronous and, hence, will need to be followed by \ref
 * jitc_sync_stream() if managed memory is subsequently accessed on the CPU.
 *
 * When both source & target are of type \ref AllocType::Device, and if the
 * current device (\ref jitc_device_set()) does not match the device associated
 * with the allocation, a peer-to-peer migration is performed.
 *
 * Note: Migrations involving AllocType::Host are currently not supported.
 */
extern JITC_EXPORT void jitc_var_migrate(uint32_t index, enum AllocType type);

/// Indicate that evaluation of the given variable causes side effects
extern JITC_EXPORT void jitc_var_mark_side_effect(uint32_t index);

/**
 * \brief Mark variable contents as dirty
 *
 * This function must be used to inform the JIT compiler when the memory region
 * underlying a variable is modified using scatter operations. It will then ensure
 * that reads from this variable (while still in dirty state) will trigger jitc_eval().
 */
extern JITC_EXPORT void jitc_var_mark_dirty(uint32_t index);

/**
 * \brief Attach an extra dependency to an existing variables
 *
 * This operation informs the JIT compiler about an additional intra-variable
 * dependency of the variable \c index on \c dep. Such dependencies are
 * required e.g. by gather and scatter operations: under no circumstances
 * should it be possible that the source/target region is garbage collected
 * before the operation had a chance to execute.
 *
 * Depending on the state of the variable \c index the extra dependency
 * manifests in two different ways:
 *
 * <ol>
 *    <li>When \c index is an non-evaluated variable, Enoki guarantees that
 *        the variable \c dep is kept alive until \c index has been evaluated.</li>
 *
 *    <li>When \c index was already evaluated, or when it refers to a memory region
 *        that has been mapped (\ref jitc_var_map()) or copied (\ref
 *        jitc_var_copy), Enoki guarantees that the variable \c dep is kept
 *        alive until \c index is itself garbage-collected.</li>
 * <ol>
 */
extern JITC_EXPORT void jitc_var_set_extra_dep(uint32_t index, uint32_t dep);

/**
 * \brief Is the given variable a mask that has all bits set to '0'?
 *
 * This function can be used to implement simple constant propagation of masks,
 * which unlocks a number of optimizations. Note that this function can only
 * detect matching masks if they have not yet been evaluated.
 */
extern JITC_EXPORT int jitc_var_is_all_false(uint32_t index);

/**
 * \brief Is the given variable a mask that has all bits set to '1'?
 *
 * This function can be used to implement simple constant propagation of masks,
 * which unlocks a number of optimizations. Note that this function can only
 * detect matching masks if they have not yet been evaluated.
 */
extern JITC_EXPORT int jitc_var_is_all_true(uint32_t index);

/**
 * \brief Return a human-readable summary of registered variables
 *
 * Note: the return value points into a static array, whose contents may be
 * changed by later calls to <tt>jitc_*</tt> API functions. Either use it right
 * away or create a copy.
 */
extern JITC_EXPORT const char *jitc_var_whos();

/**
 * \brief Return a human-readable summary of the contents of a variable
 *
 * Note: the return value points into a static array, whose contents may be
 * changed by later calls to <tt>jitc_*</tt> API functions. Either use it right
 * away or create a copy.
 */
extern JITC_EXPORT const char *jitc_var_str(uint32_t index);

/**
 * \brief Read a single element of a variable and write it to 'dst'
 *
 * This function fetches a single entry from the variable with \c index at
 * offset \c offset and writes it to the CPU output buffer \c dst.
 *
 * This function is convenient to spot-check entries of an array, but it should
 * never be used to extract complete array contents due to its low performance.
 * This operation fully synchronizes the host CPU & device.
 */
extern JITC_EXPORT void jitc_var_read(uint32_t index, uint32_t offset,
                                      void *dst);

/**
 * \brief Copy 'dst' to a single element of a variable
 *
 * This function implements the reverse of jitc_var_read(). This function is
 * convenient to change localized entries of an array, but it should never be
 * used to extract complete array contents due to its low performance.
 */
extern JITC_EXPORT void jitc_var_write(uint32_t index, uint32_t offset,
                                       const void *src);

// ====================================================================
//                 Kernel compilation and evaluation
// ====================================================================
//

/// Evaluate all computation that is queued on the current stream
extern JITC_EXPORT void jitc_eval();

/// Call jitc_eval() only if the variable 'index' requires evaluation
extern JITC_EXPORT void jitc_var_eval(uint32_t index);

// ====================================================================
//  Assortment of tuned kernels for initialization, reductions, etc.
// ====================================================================

#if defined(__cplusplus)
/// Potential reduction operations for \ref jitc_reduce
enum class ReductionType : uint32_t { Add, Mul, Min, Max, And, Or, Count };
#else
enum ReductionType {
    ReductionTypeAdd, ReductionTypeMul, ReductionTypeMin,
    ReductionTypeMax, ReductionTypeAnd, ReductionTypeOr,
    ReductionTypeCount
};
#endif
/**
 * \brief Fill a device memory region with constants of a given type
 *
 * This function writes \c size values of type \c type to the output array \c
 * ptr. The specific value is taken from \c src, which must be a CPU pointer to
 * a single int, float, double, etc (depending on \c type).
 */
extern JITC_EXPORT void jitc_fill(enum VarType type, void *ptr, uint32_t size,
                                  const void *src);

/// Perform a synchronous copy operation
extern JITC_EXPORT void jitc_memcpy(void *dst, const void *src, size_t size);

/// Perform an asynchronous copy operation
extern JITC_EXPORT void jitc_memcpy_async(void *dst, const void *src,
                                          size_t size);

/**
 * \brief Reduce the given array to a single value
 *
 * This operation reads \c size values of type \type from the input array \c
 * ptr and performs an specified operation (e.g., addition, multplication,
 * etc.) to combine them into a single value that is written to the device
 * variable \c out.
 */
extern JITC_EXPORT void jitc_reduce(enum VarType type, enum ReductionType rtype,
                                    const void *ptr, uint32_t size, void *out);

/**
 * \brief Perform an exclusive scan / prefix sum over an unsigned integer array
 *
 * If desired, the scan can be performed in place (i.e. with <tt>in == out</tt>).
 *
 * The following comment applies to the GPU implementation: when the array is
 * larger than 4K elements, the implementation will round up \c size to the
 * next largest number of 4K, hence the supplied memory region must be
 * sufficiently large to avoid an out-of-bounds reads and writes. This is not
 * an issue for memory obtained using \ref jitc_malloc(), which internally
 * rounds allocations to the next largest power of two.
 */
extern JITC_EXPORT void jitc_scan(const uint32_t *in, uint32_t *out,
                                  uint32_t size);

/**
 * \brief Reduce an array of boolean values to a single value (AND case)
 *
 * When \c size is not a multiple of 4, the implementation will initialize up
 * to 3 bytes beyond the end of the supplied range so that an efficient 32 bit
 * reduction algorithm can be used. This is fine for allocations made using
 * \ref jitc_malloc(), which allow for this.
 */
extern JITC_EXPORT uint8_t jitc_all(uint8_t *values, uint32_t size);

/**
 * \brief Reduce an array of boolean values to a single value (OR case)
 *
 * When \c size is not a multiple of 4, the implementation will initialize up
 * to 3 bytes beyond the end of the supplied range so that an efficient 32 bit
 * reduction algorithm can be used. This is fine for allocations made using
 * \ref jitc_malloc(), which allow for this.
 */
extern JITC_EXPORT uint8_t jitc_any(uint8_t *values, uint32_t size);

/**
 * \brief Compute a permutation to reorder an integer array into a sorted
 * configuration
 *
 * Given an unsigned integer array \c values of size \c size with entries in
 * the range <tt>0 .. bucket_count - 1</tt>, compute a permutation that can be
 * used to reorder the inputs into a sorted (but non-stable) configuration.
 * When <tt>bucket_count</tt> is relatively small (e.g. < 10K), the
 * implementation is much more efficient than the alternative of actually
 * sorting the array.
 *
 * \param perm
 *     The permutation is written to \c perm, which must point to a buffer in
 *     device memory having size <tt>size * sizeof(uint32_t)</tt>.
 *
 * \param offsets
 *     When \c offset is non-NULL, the parameter should point to a host-pinned
 *     memory region with a size of at least <tt>(bucket_count * 4 + 1) *
 *     sizeof(uint32_t)<tt> bytes that will be used to record the details of
 *     non-empty buckets. It will contain quadruples <tt>(index, start, size,
 *     unused)<tt> where \c index is the bucket index, and \c start and \c end
 *     specify the associated entries of the \c perm array. The last entry
 *     is padding for 16 byte alignment.
 *
 * \return
 *     When \c offsets != NULL, the function returns the number of unique
 *     values found in \c values. Otherwise, it returns zero.
 */
extern JITC_EXPORT uint32_t jitc_mkperm(const uint32_t *values, uint32_t size,
                                        uint32_t bucket_count, uint32_t *perm,
                                        uint32_t *offsets);

#if defined(__cplusplus)
}
#endif
