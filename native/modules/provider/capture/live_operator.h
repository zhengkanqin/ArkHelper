#pragma once
#include <array>
#include "live_data.h"

namespace ark::arkhelper::capture::live {

// UnitManager.characters is Torappu.UnorderedArray<Unit> in the current client. Its lowercase
// get_count is not Runtime::count's System collection contract; expose the checked live prefix
// of its real backing array instead. No managed reference survives the current Update callback.
inline bool operator_sequence(il2cpp::Runtime& rt, void* characters, void*& items, int& count) {
    items = nullptr; count = 0;
    if (!characters) return false;
    void* klass = rt.object_class(characters);
    if (rt.class_rank(klass) == 1) {
        items = characters;
        count = rt.count(items);
        return count >= 0 && count <= 256;
    }
    if (rt.klass_name(klass) != "Torappu.UnorderedArray`1") return false;
    void* items_field = field(rt, klass, "m_items");
    void* count_field = field(rt, klass, "<count>k__BackingField");
    if (!items_field || !count_field) return false;
    void* boxed_count = rt.field_get_object(count_field, characters);
    if (!boxed_count || rt.klass_name(rt.object_class(boxed_count)) != "System.Int32") return false;
    count = rt.unbox<std::int32_t>(boxed_count);
    items = rt.field_get_object(items_field, characters);
    if (!items || rt.class_rank(rt.object_class(items)) != 1 || count < 0 || count > 256 || count > rt.count(items)) {
        items = nullptr; count = 0; return false;
    }
    return true;
}

class OperatorReader {
    struct Access {
        void* klass = nullptr;
        void* id = nullptr;
        void* alive = nullptr;
        void* born = nullptr;
        void* disappeared = nullptr;
        void* uid = nullptr;
        void* dummy = nullptr;
    };
    std::array<Access, 32> cache_{};
    std::size_t next_ = 0;
    Access& access(il2cpp::Runtime& rt, void* klass) {
        for (auto& entry : cache_) if (entry.klass == klass) return entry;
        Access& entry = cache_[next_++ % cache_.size()];
        entry = {}; entry.klass = klass;
        entry.id = method(rt, klass, "get_characterId");
        entry.alive = method(rt, klass, "get_alive");
        entry.born = method(rt, klass, "get_borned");
        entry.disappeared = method(rt, klass, "get_isDisappeared");
        entry.uid = method(rt, klass, "get_instanceUid");
        entry.dummy = method(rt, klass, "get_isDummy");
        return entry;
    }
    static bool boolean(il2cpp::Runtime& rt, void* object, void* method, bool expected) {
        if (!method) return false;
        void* boxed = rt.invoke(method, object);
        return boxed && rt.klass_name(rt.object_class(boxed)) == "System.Boolean" &&
               rt.unbox<bool>(boxed) == expected;
    }
public:
    void clear() { cache_ = {}; next_ = 0; }
    bool read(il2cpp::Runtime& rt, void* object, std::string& id, std::uint64_t& uid) {
        uid = 0;
        if (!object) return false;
        const Access entry = access(rt, rt.object_class(object));
        if (!entry.id || !entry.uid || !boolean(rt, object, entry.alive, true) ||
            (entry.dummy && !boolean(rt, object, entry.dummy, false)) ||
            !boolean(rt, object, entry.born, true) || !boolean(rt, object, entry.disappeared, false)) return false;
        id = text(rt, rt.invoke(entry.id, object));
        void* boxed_uid = rt.invoke(entry.uid, object);
        if (!boxed_uid) return false;
        const auto uid_type = rt.klass_name(rt.object_class(boxed_uid));
        if (uid_type == "System.UInt32") uid = rt.unbox<std::uint32_t>(boxed_uid);
        else if (uid_type == "System.Int32") {
            const auto value = rt.unbox<std::int32_t>(boxed_uid);
            if (value > 0) uid = static_cast<std::uint32_t>(value);
        } else return false;
        return uid && !id.empty() && id.size() < sizeof(OperatorSample::actor_id);
    }
};
} // namespace ark::arkhelper::capture::live
