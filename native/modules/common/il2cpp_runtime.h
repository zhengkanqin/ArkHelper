#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ark::il2cpp {

// Thin, checked wrapper over the IL2CPP embedding API exported by GameAssembly.dll.
//
// All lookups throw std::runtime_error on failure. Managed calls must run on a thread
// attached to the IL2CPP domain (see Attachment); objects that must survive across
// managed calls should be pinned with a Root.
class Runtime {
public:
    // Raw IL2CPP exports, bound by initialize().

    // Domain, threads and assemblies.
    void* (*domain_get)() = nullptr;
    void* (*corlib)() = nullptr;
    void** (*attached_threads)(size_t*) = nullptr;
    void* (*thread_current)() = nullptr;
    void* (*thread_attach)(void*) = nullptr;
    void (*thread_detach)(void*) = nullptr;
    void** (*domain_assemblies)(void*, size_t*) = nullptr;
    void* (*assembly_image)(void*) = nullptr;
    const char* (*image_name)(void*) = nullptr;

    // Classes.
    void* (*class_from_name)(void*, const char*, const char*) = nullptr;
    void* (*class_parent)(void*) = nullptr;
    const char* (*class_name)(void*) = nullptr;
    const char* (*class_namespace)(void*) = nullptr;
    void* (*class_type)(void*) = nullptr;
    void* (*class_from_type)(void*) = nullptr;
    bool (*class_is_value)(void*) = nullptr;
    bool (*class_assignable)(void*, void*) = nullptr;
    int32_t (*class_value_size)(void*, uint32_t*) = nullptr;
    void (*class_init)(void*) = nullptr;
    int (*class_rank)(void*) = nullptr;
    void* (*class_element)(void*) = nullptr;
    void* (*class_nested)(void*, void**) = nullptr;

    // Arrays.
    uintptr_t (*array_length)(void*) = nullptr;

    // Fields.
    void* (*class_field)(void*, const char*) = nullptr;
    void* (*class_fields)(void*, void**) = nullptr;
    void* (*field_type)(void*) = nullptr;
    const char* (*field_name)(void*) = nullptr;
    void (*field_get)(void*, void*, void*) = nullptr;
    void* (*field_get_object)(void*, void*) = nullptr;
    void (*field_static_get)(void*, void*) = nullptr;

    // Methods.
    void* (*class_methods)(void*, void**) = nullptr;
    const char* (*method_name)(void*) = nullptr;
    void* (*method_class)(void*) = nullptr;
    uint32_t (*method_count)(void*) = nullptr;
    void* (*method_param)(void*, uint32_t) = nullptr;
    void* (*method_return)(void*) = nullptr;
    uint32_t (*method_flags)(void*, uint32_t*) = nullptr;
    bool (*method_is_generic)(void*) = nullptr;

    // Types, objects and strings.
    char* (*type_name)(void*) = nullptr;
    void (*free_memory)(void*) = nullptr;
    void* (*object_class)(void*) = nullptr;
    void* (*object_unbox)(void*) = nullptr;
    void* (*invoke_method)(void*, void*, void**, void**) = nullptr;
    void (*format_exception)(void*, char*, int) = nullptr;
    void* (*string_new)(const char*) = nullptr;
    int (*string_length)(void*) = nullptr;
    const wchar_t* (*string_chars)(void*) = nullptr;

    // GC handles.
    uint32_t (*gc_new)(void*, bool) = nullptr;
    void (*gc_free)(uint32_t) = nullptr;
    void* (*gc_target)(uint32_t) = nullptr;

    // Binds every export and verifies that the runtime has finished starting up.
    void initialize();

    // Attaches the current thread to the IL2CPP domain for the object's lifetime
    // unless it is already attached.
    class Attachment {
    public:
        explicit Attachment(Runtime& runtime);
        ~Attachment();
        Attachment(const Attachment&) = delete;
        Attachment& operator=(const Attachment&) = delete;

    private:
        Runtime& runtime_;
        void* attached_ = nullptr;
    };

    // Strong GC handle that keeps a managed object alive across managed calls.
    class Root {
    public:
        Root() = default;
        Root(Runtime& runtime, void* object);
        ~Root();
        Root(const Root&) = delete;
        Root& operator=(const Root&) = delete;
        Root(Root&& other) noexcept;
        Root& operator=(Root&& other) noexcept;

        void* get() const;

    private:
        Runtime* runtime_ = nullptr;
        uint32_t handle_ = 0;
    };

    // Metadata lookup.
    std::string type(void* value);
    std::string klass_name(void* klass);
    void* find(const char* name_space, const char* name);
    void* nested(void* klass, const char* name);
    void* field(void* klass, const char* name);
    // Finds a method by name and arity on `klass` or its nearest declaring base class.
    // A non-empty `signature` must list the full type name of every parameter.
    void* method(void* klass, const char* name, int count,
                 const std::vector<std::string>& signature = {});

    // Managed invocation. Reference arguments are passed as object pointers, value-type
    // arguments as pointers to their storage.
    void* invoke(void* method, void* object, std::initializer_list<void*> arguments = {});
    void* checked_class(void* object);
    // Invokes a method on the receiver's runtime class, caching the lookup per class.
    void* call(void* object, const char* name, std::initializer_list<void*> arguments = {});
    // Read-only field and value access. No game-state writers are exposed.
    template <class T>
    T unbox(void* object);
    template <class T>
    T get(void* object, const char* name);
    void* get_object(void* object, const char* name);
    template <class T>
    T static_value(void* klass, const char* name);

    // Converts a System.String to UTF-8; nullptr yields an empty string.
    std::string string(void* value);
    // Returns `klass.instance`, which must be non-null.
    void* singleton(void* klass);
    int enum_value(void* klass, const char* name);

    // Arrays and indexable collections (Count / get_Item).
    int count(void* sequence);
    void* at(void* sequence, int index);

private:
    struct CallAccess {
        std::string name;
        size_t arity;
        void* target;
    };

    struct SequenceAccess {
        void* count = nullptr;
        void* item = nullptr;
    };

    // Throws unless the managed storage type `klass` can be copied as a T.
    template <class T>
    void check_storage(void* klass);

    std::unordered_map<void*, std::vector<CallAccess>> calls_;
    std::unordered_map<void*, SequenceAccess> sequences_;
};

template <class T>
T Runtime::unbox(void* object) {
    check_storage<T>(checked_class(object));
    T result{};
    std::memcpy(&result, object_unbox(object), sizeof(result));
    return result;
}

template <class T>
T Runtime::get(void* object, const char* name) {
    void* item = field(checked_class(object), name);
    check_storage<T>(class_from_type(field_type(item)));
    T result{};
    field_get(object, item, &result);
    return result;
}

template <class T>
T Runtime::static_value(void* klass, const char* name) {
    void* item = field(klass, name);
    check_storage<T>(class_from_type(field_type(item)));
    T result{};
    field_static_get(item, &result);
    return result;
}

template <class T>
void Runtime::check_storage(void* klass) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!klass) {
        throw std::runtime_error("Missing managed storage type");
    }
    if constexpr (std::is_pointer_v<T>) {
        if (class_is_value(klass)) {
            throw std::runtime_error("Expected managed reference: " + klass_name(klass));
        }
    } else {
        uint32_t alignment = 0;
        if (!class_is_value(klass) ||
            static_cast<size_t>(class_value_size(klass, &alignment)) != sizeof(T)) {
            throw std::runtime_error("Managed value size mismatch: " + klass_name(klass));
        }
    }
}

} // namespace ark::il2cpp
