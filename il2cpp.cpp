#include "pch.h"
#include <cstring>
#include "il2cpp.h"
#include "common.h"

namespace il2 {

void* (*domain_get)();
void* (*thread_attach)(void*);
void** (*domain_get_assemblies)(void*, size_t*);
void* (*assembly_get_image)(void*);
const char* (*image_get_name)(void*);
size_t (*image_get_class_count)(void*);
void* (*image_get_class)(void*, size_t);
const char* (*class_get_name)(void*);
const char* (*class_get_namespace)(void*);
void* (*class_get_parent)(void*);
void* (*class_get_fields)(void*, void**);
void* (*class_get_methods)(void*, void**);
const char* (*field_get_name)(void*);
void* (*field_get_type)(void*);
size_t (*field_get_offset)(void*);
int (*field_get_flags)(void*);
char* (*type_get_name)(void*);
void (*il2_free)(void*);
const char* (*method_get_name)(void*);
uint32_t (*method_get_param_count)(void*);
void* (*method_get_return_type)(void*);
void* (*method_get_param)(void*, uint32_t);
const char* (*method_get_param_name)(void*, uint32_t);
uint32_t (*method_get_flags)(void*, uint32_t*);
void* (*class_from_name)(void*, const char*, const char*);
void* (*class_get_field_from_name)(void*, const char*);
void (*field_static_get_value)(void*, void*);
void* (*class_get_method_from_name)(void*, const char*, int);
void* (*object_get_class)(void*);
void* (*string_new)(const char*);
void* (*object_new)(void*);
void* (*class_get_type)(void*);
void* (*type_get_object)(void*);
void* (*runtime_invoke)(void*, void*, void**, void**);
void* (*array_new)(void*, uintptr_t);
uint32_t (*gchandle_new)(void*, int);

static HMODULE s_mod;
static int s_missing;

template <class T>
static void load(T& fn, const char* name, bool required = true) {
    fn = reinterpret_cast<T>(GetProcAddress(s_mod, name));
    if (!fn && required) {
        logf("il2cpp export missing: %s", name);
        s_missing++;
    }
}

bool init(HMODULE ga) {
    s_mod = ga;
    s_missing = 0;
    load(domain_get, "il2cpp_domain_get");
    load(thread_attach, "il2cpp_thread_attach");
    load(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    load(assembly_get_image, "il2cpp_assembly_get_image");
    load(image_get_name, "il2cpp_image_get_name");
    load(image_get_class_count, "il2cpp_image_get_class_count");
    load(image_get_class, "il2cpp_image_get_class");
    load(class_get_name, "il2cpp_class_get_name");
    load(class_get_namespace, "il2cpp_class_get_namespace");
    load(class_get_parent, "il2cpp_class_get_parent");
    load(class_get_fields, "il2cpp_class_get_fields");
    load(class_get_methods, "il2cpp_class_get_methods");
    load(field_get_name, "il2cpp_field_get_name");
    load(field_get_type, "il2cpp_field_get_type");
    load(field_get_offset, "il2cpp_field_get_offset");
    load(field_get_flags, "il2cpp_field_get_flags");
    load(type_get_name, "il2cpp_type_get_name");
    load(il2_free, "il2cpp_free", false);
    load(method_get_name, "il2cpp_method_get_name");
    load(method_get_param_count, "il2cpp_method_get_param_count");
    load(method_get_return_type, "il2cpp_method_get_return_type");
    load(method_get_param, "il2cpp_method_get_param");
    load(method_get_param_name, "il2cpp_method_get_param_name");
    load(method_get_flags, "il2cpp_method_get_flags");
    load(class_from_name, "il2cpp_class_from_name");
    load(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    load(field_static_get_value, "il2cpp_field_static_get_value");
    load(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    load(object_get_class, "il2cpp_object_get_class");
    load(string_new, "il2cpp_string_new");
    load(object_new, "il2cpp_object_new");
    load(class_get_type, "il2cpp_class_get_type");
    load(type_get_object, "il2cpp_type_get_object");
    load(runtime_invoke, "il2cpp_runtime_invoke");
    load(array_new, "il2cpp_array_new", false);
    load(gchandle_new, "il2cpp_gchandle_new", false);
    return s_missing == 0;
}

void* find_image(const char* name) {
    void* dom = domain_get();
    if (!dom) return nullptr;
    size_t count = 0;
    void** asms = domain_get_assemblies(dom, &count);
    if (!asms) return nullptr;
    for (size_t i = 0; i < count; i++) {
        void* img = assembly_get_image(asms[i]);
        if (!img) continue;
        const char* n = image_get_name(img);
        if (n && lstrcmpiA(n, name) == 0) return img;
    }
    return nullptr;
}

void* find_class(void* image, const char* ns, const char* name) {
    return image ? class_from_name(image, ns, name) : nullptr;
}

void* find_field(void* klass, const char* name) {
    return klass ? class_get_field_from_name(klass, name) : nullptr;
}

int field_off(void* klass, const char* name) {
    void* f = find_field(klass, name);
    if (!f) {
        logf("field not found: %s", name);
        return -1;
    }
    return static_cast<int>(field_get_offset(f));
}

void* find_method(void* klass, const char* name, int argc) {
    void* m = klass ? class_get_method_from_name(klass, name, argc) : nullptr;
    if (!m) logf("method not found: %s (%d args)", name, argc);
    return m;
}

void* find_method_sig(void* klass, const char* name, const char* const* params, int argc) {
    if (!klass) return nullptr;
    void* it = nullptr;
    while (void* m = class_get_methods(klass, &it)) {
        const char* n = method_get_name(m);
        if (!n || strcmp(n, name) != 0) continue;
        if (static_cast<int>(method_get_param_count(m)) != argc) continue;
        bool ok = true;
        for (int i = 0; i < argc && ok; i++) {
            char* tn = type_get_name(method_get_param(m, static_cast<uint32_t>(i)));
            ok = tn && strcmp(tn, params[i]) == 0;
            if (tn && il2_free) il2_free(tn);
        }
        if (ok) return m;
    }
    logf("method signature not found: %s/%d", name, argc);
    return nullptr;
}

std::string to_utf8(void* s) {
    if (!s) return std::string();
    // System.String: header 0x10, int32 length at 0x10, UTF-16 chars at 0x14
    int len = *reinterpret_cast<int*>(static_cast<char*>(s) + 0x10);
    if (len <= 0 || len > 4096) return std::string();
    const wchar_t* w = reinterpret_cast<const wchar_t*>(static_cast<char*>(s) + 0x14);
    char buf[8200];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, buf, static_cast<int>(sizeof(buf)) - 1, nullptr, nullptr);
    if (n <= 0) return std::string();
    return std::string(buf, static_cast<size_t>(n));
}

}  // namespace il2
