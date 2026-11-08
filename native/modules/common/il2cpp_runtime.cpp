#include "il2cpp_runtime.h"

#include <windows.h>

#include <climits>

#include "il2cpp_exports.h"

namespace ark::il2cpp {
namespace {

// METHOD_ATTRIBUTE_* flags from ECMA-335.
constexpr uint32_t kMethodAttributeStatic = 0x10;

constexpr int kExceptionTextCapacity = 2048;
constexpr int kMaxStringLength = 65536;

[[noreturn]] void throw_managed_exception(void (*format_exception)(void*, char*, int),
                                          const std::string& context, void* exception) {
    char text[kExceptionTextCapacity]{};
    format_exception(exception, text, sizeof(text));
    throw std::runtime_error(context + ": " + text);
}

// IL2CPP's MethodInfo begins with the native method pointer; it is null when no AOT
// implementation exists for that method.
bool has_native_code(void* method_info) {
    return *static_cast<void**>(method_info) != nullptr;
}

bool is_static_method(Runtime& runtime, void* method) {
    uint32_t implementation_flags = 0;
    return (runtime.method_flags(method, &implementation_flags) & kMethodAttributeStatic) != 0;
}

} // namespace

void Runtime::initialize() {
#define ARK_BIND_EXPORT(member, symbol) member = resolve_export<decltype(member)>("il2cpp_" symbol)
    ARK_BIND_EXPORT(domain_get, "domain_get");
    ARK_BIND_EXPORT(thread_current, "thread_current");
    ARK_BIND_EXPORT(thread_attach, "thread_attach");
    ARK_BIND_EXPORT(thread_detach, "thread_detach");
    ARK_BIND_EXPORT(corlib, "get_corlib");
    ARK_BIND_EXPORT(attached_threads, "thread_get_all_attached_threads");
    ARK_BIND_EXPORT(domain_assemblies, "domain_get_assemblies");
    ARK_BIND_EXPORT(assembly_image, "assembly_get_image");
    ARK_BIND_EXPORT(image_name, "image_get_name");
    ARK_BIND_EXPORT(class_from_name, "class_from_name");
    ARK_BIND_EXPORT(class_parent, "class_get_parent");
    ARK_BIND_EXPORT(class_name, "class_get_name");
    ARK_BIND_EXPORT(class_namespace, "class_get_namespace");
    ARK_BIND_EXPORT(class_assignable, "class_is_assignable_from");
    ARK_BIND_EXPORT(class_type, "class_get_type");
    ARK_BIND_EXPORT(class_from_type, "class_from_type");
    ARK_BIND_EXPORT(class_is_value, "class_is_valuetype");
    ARK_BIND_EXPORT(class_field, "class_get_field_from_name");
    ARK_BIND_EXPORT(class_value_size, "class_value_size");
    ARK_BIND_EXPORT(class_rank, "class_get_rank");
    ARK_BIND_EXPORT(class_element, "class_get_element_class");
    ARK_BIND_EXPORT(class_init, "runtime_class_init");
    ARK_BIND_EXPORT(class_nested, "class_get_nested_types");
    ARK_BIND_EXPORT(array_length, "array_length");
    ARK_BIND_EXPORT(field_static_get, "field_static_get_value");
    ARK_BIND_EXPORT(class_fields, "class_get_fields");
    ARK_BIND_EXPORT(field_type, "field_get_type");
    ARK_BIND_EXPORT(field_name, "field_get_name");
    ARK_BIND_EXPORT(field_get, "field_get_value");
    ARK_BIND_EXPORT(field_get_object, "field_get_value_object");
    ARK_BIND_EXPORT(class_methods, "class_get_methods");
    ARK_BIND_EXPORT(method_name, "method_get_name");
    ARK_BIND_EXPORT(method_count, "method_get_param_count");
    ARK_BIND_EXPORT(method_param, "method_get_param");
    ARK_BIND_EXPORT(method_class, "method_get_class");
    ARK_BIND_EXPORT(method_return, "method_get_return_type");
    ARK_BIND_EXPORT(method_flags, "method_get_flags");
    ARK_BIND_EXPORT(type_name, "type_get_name");
    ARK_BIND_EXPORT(free_memory, "free");
    ARK_BIND_EXPORT(method_is_generic, "method_is_generic");
    ARK_BIND_EXPORT(object_class, "object_get_class");
    ARK_BIND_EXPORT(object_unbox, "object_unbox");
    ARK_BIND_EXPORT(invoke_method, "runtime_invoke");
    ARK_BIND_EXPORT(format_exception, "format_exception");
    ARK_BIND_EXPORT(string_new, "string_new");
    ARK_BIND_EXPORT(string_length, "string_length");
    ARK_BIND_EXPORT(string_chars, "string_chars");
    ARK_BIND_EXPORT(gc_new, "gchandle_new");
    ARK_BIND_EXPORT(gc_free, "gchandle_free");
    ARK_BIND_EXPORT(gc_target, "gchandle_get_target");
#undef ARK_BIND_EXPORT

    size_t registered = 0;
    if (!corlib() || !attached_threads(&registered) || !registered) {
        throw std::runtime_error("IL2CPP GC/thread initialization is not ready");
    }
    if (!domain_get()) {
        throw std::runtime_error("IL2CPP domain is not ready");
    }
}

Runtime::Attachment::Attachment(Runtime& runtime) : runtime_(runtime) {
    if (runtime.thread_current()) {
        return;
    }
    attached_ = runtime.thread_attach(runtime.domain_get());
    if (!attached_) {
        throw std::runtime_error("IL2CPP thread attachment failed");
    }
}

Runtime::Attachment::~Attachment() {
    if (attached_) {
        runtime_.thread_detach(attached_);
    }
}

Runtime::Root::Root(Runtime& runtime, void* object) : runtime_(&runtime) {
    if (!object) {
        return;
    }
    handle_ = runtime.gc_new(object, false);
    if (!handle_) {
        throw std::runtime_error("IL2CPP GC root allocation failed");
    }
}

Runtime::Root::~Root() {
    if (handle_) {
        runtime_->gc_free(handle_);
    }
}

Runtime::Root::Root(Root&& other) noexcept
    : runtime_(other.runtime_),
      handle_(std::exchange(other.handle_, 0)) {}

Runtime::Root& Runtime::Root::operator=(Root&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            runtime_->gc_free(handle_);
        }
        runtime_ = other.runtime_;
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

void* Runtime::Root::get() const {
    return handle_ ? runtime_->gc_target(handle_) : nullptr;
}

std::string Runtime::type(void* value) {
    if (!value) {
        throw std::runtime_error("Null IL2CPP type");
    }
    char* name = type_name(value);
    if (!name) {
        throw std::runtime_error("Missing IL2CPP type name");
    }
    std::string result(name);
    free_memory(name);
    return result;
}

std::string Runtime::klass_name(void* klass) {
    if (!klass) {
        return "<null>";
    }
    const char* name_space = class_namespace(klass);
    std::string prefix = name_space && *name_space ? std::string(name_space) + "." : "";
    return prefix + class_name(klass);
}

void* Runtime::find(const char* name_space, const char* name) {
    size_t count = 0;
    void** assemblies = domain_assemblies(domain_get(), &count);
    void* found = nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = assembly_image(assemblies[i]);
        if (!image) {
            continue;
        }
        void* candidate = class_from_name(image, name_space, name);
        if (!candidate) {
            continue;
        }
        if (found && found != candidate) {
            throw std::runtime_error(std::string("Ambiguous class: ") + name_space + "." + name);
        }
        found = candidate;
    }
    if (!found) {
        throw std::runtime_error(std::string("Missing class: ") + name_space + "." + name);
    }
    return found;
}

void* Runtime::nested(void* klass, const char* name) {
    if (!klass) {
        throw std::runtime_error("Null enclosing class");
    }
    void* iterator = nullptr;
    while (void* candidate = class_nested(klass, &iterator)) {
        if (!std::strcmp(class_name(candidate), name)) {
            return candidate;
        }
    }
    throw std::runtime_error("Missing nested class: " + klass_name(klass) + "." + name);
}

void* Runtime::field(void* klass, const char* name) {
    if (!klass) {
        throw std::runtime_error(std::string("Null class for field: ") + name);
    }
    void* result = class_field(klass, name);
    if (!result) {
        throw std::runtime_error("Missing field: " + klass_name(klass) + "." + name);
    }
    return result;
}

void* Runtime::method(void* klass, const char* name, int count,
                      const std::vector<std::string>& signature) {
    const auto matches_signature = [&](void* candidate) {
        if (signature.empty()) {
            return true;
        }
        for (int i = 0; i < count; ++i) {
            if (type(method_param(candidate, i)) != signature.at(i)) {
                return false;
            }
        }
        return true;
    };

    for (void* owner = klass; owner; owner = class_parent(owner)) {
        void* iterator = nullptr;
        void* result = nullptr;
        while (void* candidate = class_methods(owner, &iterator)) {
            if (std::strcmp(method_name(candidate), name) ||
                method_count(candidate) != static_cast<uint32_t>(count)) {
                continue;
            }
            if (!matches_signature(candidate)) {
                continue;
            }
            if (result) {
                throw std::runtime_error("Ambiguous method: " + klass_name(owner) + "." + name);
            }
            result = candidate;
        }
        if (result) {
            if (!has_native_code(result)) {
                throw std::runtime_error("Unavailable open generic method: " + klass_name(owner) +
                                         "." + name);
            }
            return result;
        }
    }
    throw std::runtime_error("Missing method: " + klass_name(klass) + "." + name + "/" +
                             std::to_string(count));
}

void* Runtime::invoke(void* method, void* object, std::initializer_list<void*> arguments) {
    if (!method) {
        throw std::runtime_error("Null managed method");
    }
    if (method_count(method) != arguments.size()) {
        throw std::runtime_error("Managed argument count mismatch");
    }
    if (!is_static_method(*this, method) && !object) {
        throw std::runtime_error(std::string("Null receiver: ") + method_name(method));
    }
    void* receiver = object;
    // A boxed value can inherit a reference-type implementation, notably
    // Enum/Object.ToString. Only methods declared on value types accept an
    // unboxed receiver; their base implementations still need the box.
    if (receiver && class_is_value(method_class(method))) {
        receiver = object_unbox(receiver);
    }
    // runtime_invoke reads references directly from params and value types from
    // the pointed-to value buffer. Never invoke on an unattached thread.
    if (!thread_current()) {
        throw std::runtime_error("Managed invoke on unattached thread");
    }
    void** parameters = arguments.size() ? const_cast<void**>(arguments.begin()) : nullptr;
    void* exception = nullptr;
    void* result = invoke_method(method, receiver, parameters, &exception);
    if (exception) {
        throw_managed_exception(format_exception, method_name(method), exception);
    }
    return result;
}

void* Runtime::checked_class(void* object) {
    if (!object) {
        throw std::runtime_error("Managed API returned a null object");
    }
    return object_class(object);
}

void* Runtime::call(void* object, const char* name, std::initializer_list<void*> arguments) {
    void* klass = checked_class(object);
    auto& entries = calls_[klass];
    for (const auto& entry : entries) {
        if (entry.arity == arguments.size() && entry.name == name) {
            return invoke(entry.target, object, arguments);
        }
    }
    void* target = method(klass, name, static_cast<int>(arguments.size()));
    entries.push_back({name, arguments.size(), target});
    return invoke(target, object, arguments);
}

void* Runtime::get_object(void* object, const char* name) {
    return field_get_object(field(checked_class(object), name), object);
}

std::string Runtime::string(void* value) {
    if (!value) {
        return {};
    }
    if (std::strcmp(class_name(checked_class(value)), "String") ||
        std::strcmp(class_namespace(checked_class(value)), "System")) {
        throw std::runtime_error("Expected System.String");
    }
    const int length = string_length(value);
    if (length < 0 || length > kMaxStringLength) {
        throw std::runtime_error("Invalid managed string length");
    }
    const wchar_t* chars = string_chars(value);
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, nullptr, 0,
                                         nullptr, nullptr);
    if (!size && length) {
        throw std::runtime_error("Invalid managed UTF-16");
    }
    std::string result(size, '\0');
    if (size) {
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, result.data(), size,
                            nullptr, nullptr);
    }
    return result;
}

void* Runtime::singleton(void* klass) {
    void* getter = method(klass, "get_instance", 0);
    void* result = invoke(getter, nullptr);
    if (!result) {
        throw std::runtime_error("Database not initialized: " + klass_name(klass));
    }
    return result;
}

int Runtime::enum_value(void* klass, const char* name) {
    return static_value<int>(klass, name);
}

int Runtime::count(void* sequence) {
    if (!sequence) {
        return 0;
    }
    void* klass = checked_class(sequence);
    if (class_rank(klass) > 0) {
        const uintptr_t size = array_length(sequence);
        if (size > INT32_MAX) {
            throw std::runtime_error("Managed array exceeds supported index range");
        }
        return static_cast<int>(size);
    }
    auto& access = sequences_[klass];
    if (!access.count) {
        access.count = method(klass, "get_Count", 0);
    }
    const int size = unbox<int>(invoke(access.count, sequence));
    if (size < 0) {
        throw std::runtime_error("Negative managed sequence size");
    }
    return size;
}

void* Runtime::at(void* sequence, int index) {
    if (index < 0 || index >= count(sequence)) {
        throw std::runtime_error("Managed sequence index out of range");
    }
    void* klass = checked_class(sequence);
    auto& access = sequences_[klass];
    if (!access.item) {
        access.item = class_rank(klass) > 0 ? method(klass, "GetValue", 1, {"System.Int32"})
                                            : method(klass, "get_Item", 1, {"System.Int32"});
    }
    return invoke(access.item, sequence, {&index});
}

} // namespace ark::il2cpp
