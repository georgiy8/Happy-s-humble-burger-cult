#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Thin wrappers over the il2cpp_* exports of GameAssembly.dll.
// Everything is looked up by NAME at runtime, so nothing is hard-coded.
namespace il2 {

extern void* (*domain_get)();
extern void* (*thread_attach)(void*);
extern void** (*domain_get_assemblies)(void*, size_t*);
extern void* (*assembly_get_image)(void*);
extern const char* (*image_get_name)(void*);
extern size_t (*image_get_class_count)(void*);
extern void* (*image_get_class)(void*, size_t);
extern const char* (*class_get_name)(void*);
extern const char* (*class_get_namespace)(void*);
extern void* (*class_get_parent)(void*);
extern void* (*class_get_fields)(void*, void**);
extern void* (*class_get_methods)(void*, void**);
extern const char* (*field_get_name)(void*);
extern void* (*field_get_type)(void*);
extern size_t (*field_get_offset)(void*);
extern int (*field_get_flags)(void*);
extern char* (*type_get_name)(void*);
extern void (*il2_free)(void*);
extern const char* (*method_get_name)(void*);
extern uint32_t (*method_get_param_count)(void*);
extern void* (*method_get_return_type)(void*);
extern void* (*method_get_param)(void*, uint32_t);
extern const char* (*method_get_param_name)(void*, uint32_t);
extern uint32_t (*method_get_flags)(void*, uint32_t*);
extern void* (*class_from_name)(void*, const char*, const char*);
extern void* (*class_get_field_from_name)(void*, const char*);
extern void (*field_static_get_value)(void*, void*);
extern void* (*class_get_method_from_name)(void*, const char*, int);
extern void* (*object_get_class)(void*);
extern void* (*string_new)(const char*);
extern void* (*object_new)(void*);
extern void* (*class_get_type)(void*);
extern void* (*type_get_object)(void*);
extern void* (*runtime_invoke)(void*, void*, void**, void**);
extern void* (*array_new)(void*, uintptr_t);          // optional: il2cpp_array_new(elementClass, length)
extern uint32_t (*gchandle_new)(void*, int);            // optional: il2cpp_gchandle_new(obj, pinned)

bool init(HMODULE gameAssembly);              // false if a required export is missing
void* find_image(const char* name);           // e.g. "Assembly-CSharp.dll"
void* find_class(void* image, const char* ns, const char* name);
void* find_field(void* klass, const char* name);
int field_off(void* klass, const char* name); // instance field offset, -1 if missing
void* find_method(void* klass, const char* name, int argc);  // MethodInfo*
// find a method by name + exact parameter type names, e.g. {"UnityEngine.Rect","System.String"}
void* find_method_sig(void* klass, const char* name, const char* const* params, int argc);
inline void* native(void* methodInfo) { return methodInfo ? *reinterpret_cast<void**>(methodInfo) : nullptr; }
std::string to_utf8(void* il2cppString);      // System.String* -> std::string

}  // namespace il2
