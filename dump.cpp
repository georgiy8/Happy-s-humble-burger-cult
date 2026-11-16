#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

// ============================================================================
//  FULL DUMP  (runs on the game's main thread, requested from the menu)
//    hhbc_details.txt  - every class / field / method of Assembly-CSharp.dll
//    hhbc_kitchen.txt  - only kitchen / order / item / hand / challenge classes
//    hhbc_recipes.txt  - every Recipe at runtime: flags, price, ingredient names
//    hhbc_world.txt    - live stations (grill, board, drinks, fryer, oven ...)
//                        with their world positions + the player's position
// ============================================================================

namespace {

bool icontains(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

bool is_kitchen(const std::string& name) {
    static const char* kw[] = {
        "grill", "fry", "oven", "shake", "cook", "patty", "burger", "fridge", "station",
        "appliance", "machine", "bun", "drink", "soda", "bake", "tray", "ticket",
        "food", "recipe", "ingredient", "kitchen", "assembl", "toaster", "slicer", "cutting",
        "meat", "generic", "hand", "usable", "inventory", "throw", "grab", "hold", "carry",
        "bag", "bell", "dispens", "nugget", "pie", "cookie", "dough", "cup", "sauce",
        "challenge", "closeout", "duty", "task", "knob", "register", "order", "quota",
        "interact", "slot"
    };
    for (const char* k : kw) if (icontains(name, k)) return true;
    return false;
}

void write_class(FILE* f, void* k) {
    const char* ns = il2::class_get_namespace(k);
    const char* nm = il2::class_get_name(k);
    void* parent = il2::class_get_parent(k);
    const char* pn = parent ? il2::class_get_name(parent) : nullptr;
    fprintf(f, "class %s%s%s : %s\n", ns && *ns ? ns : "", ns && *ns ? "." : "", nm ? nm : "?", pn ? pn : "-");
    void* it = nullptr;
    while (void* fld = il2::class_get_fields(k, &it)) {
        char* tn = il2::type_get_name(il2::field_get_type(fld));
        fprintf(f, "  field  %s %s offset=0x%zx%s\n", tn ? tn : "?", il2::field_get_name(fld),
                il2::field_get_offset(fld), (il2::field_get_flags(fld) & 0x10) ? "  static" : "");
        if (tn && il2::il2_free) il2::il2_free(tn);
    }
    it = nullptr;
    while (void* m = il2::class_get_methods(k, &it)) {
        char* rt = il2::type_get_name(il2::method_get_return_type(m));
        uint32_t iflags = 0;
        uint32_t fl = il2::method_get_flags(m, &iflags);
        fprintf(f, "  method %s %s(", rt ? rt : "?", il2::method_get_name(m));
        if (rt && il2::il2_free) il2::il2_free(rt);
        uint32_t pc = il2::method_get_param_count(m);
        for (uint32_t p = 0; p < pc; p++) {
            char* tn = il2::type_get_name(il2::method_get_param(m, p));
            fprintf(f, "%s%s", p ? ", " : "", tn ? tn : "?");
            if (tn && il2::il2_free) il2::il2_free(tn);
        }
        fprintf(f, ")%s\n", (fl & 0x10) ? "  static" : "");
    }
    fputc('\n', f);
}

// ---- small helpers for live objects ---------------------------------------
void* s_getName = nullptr;
void* s_getTransform = nullptr;
void* s_getPosition = nullptr;
void* s_findObjects = nullptr;
int o_listItems = 0x10, o_listSize = 0x18;

std::string uname(void* obj) {
    if (!obj || !s_getName) return std::string();
    void* exc = nullptr;
    void* r = il2::runtime_invoke(s_getName, obj, nullptr, &exc);
    if (exc || !r) return std::string();
    return il2::to_utf8(r);
}

bool position_of(void* comp, float out[3]) {
    if (!comp || !s_getTransform || !s_getPosition) return false;
    void* exc = nullptr;
    void* tr = il2::runtime_invoke(s_getTransform, comp, nullptr, &exc);
    if (exc || !tr) return false;
    void* box = il2::runtime_invoke(s_getPosition, tr, nullptr, &exc);
    if (exc || !box) return false;
    const float* v = reinterpret_cast<const float*>(static_cast<char*>(box) + 0x10);
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return true;
}

void resolve_helpers() {
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* kObj = il2::find_class(core, "UnityEngine", "Object");
    void* kComp = il2::find_class(core, "UnityEngine", "Component");
    void* kTr = il2::find_class(core, "UnityEngine", "Transform");
    s_getName = kObj ? il2::find_method(kObj, "get_name", 0) : nullptr;
    s_getTransform = kComp ? il2::find_method(kComp, "get_transform", 0) : nullptr;
    s_getPosition = kTr ? il2::find_method(kTr, "get_position", 0) : nullptr;
    const char* tp[] = { "System.Type" };
    s_findObjects = kObj ? il2::find_method_sig(kObj, "FindObjectsOfType", tp, 1) : nullptr;

    void* ms = il2::find_image("mscorlib.dll");
    void* kList = il2::find_class(ms, "System.Collections.Generic", "List`1");
    if (kList) {
        int a = il2::field_off(kList, "_items");
        int b = il2::field_off(kList, "_size");
        if (a > 0 && b > 0) { o_listItems = a; o_listSize = b; }
    }
}

inline void* rd_ptr(void* b, int off) { return *reinterpret_cast<void**>(static_cast<char*>(b) + off); }
inline int rd_int(void* b, int off) { return *reinterpret_cast<int*>(static_cast<char*>(b) + off); }
inline bool rd_bool(void* b, int off) { return off >= 0 && *(static_cast<char*>(b) + off) != 0; }

// ---- instance snapshot of one scriptable object / component: its simple fields ----
std::string describe_simple(void* obj, int maxFields) {
    std::string out;
    if (!obj) return out;
    void* k = il2::object_get_class(obj);
    int shown = 0;
    for (void* kk = k; kk && shown < maxFields; kk = il2::class_get_parent(kk)) {
        const char* cn = il2::class_get_name(kk);
        if (cn && (strcmp(cn, "Object") == 0 || strcmp(cn, "ScriptableObject") == 0)) break;
        void* it = nullptr;
        while (void* f = il2::class_get_fields(kk, &it)) {
            if (shown >= maxFields) break;
            int fl = il2::field_get_flags(f);
            if ((fl & 0x10) || (fl & 0x40)) continue;
            size_t off = il2::field_get_offset(f);
            if (off < 0x10 || off > 0x4000) continue;
            char* tn = il2::type_get_name(il2::field_get_type(f));
            std::string t = tn ? tn : "";
            if (tn && il2::il2_free) il2::il2_free(tn);
            char tmp[200] = {};
            char* base = static_cast<char*>(obj) + off;
            const char* fn = il2::field_get_name(f);
            if (t == "System.String") {
                std::string v = il2::to_utf8(*reinterpret_cast<void**>(base));
                if (v.empty()) continue;
                snprintf(tmp, sizeof(tmp), " %s='%s'", fn, v.c_str());
            } else if (t == "System.Int32") {
                snprintf(tmp, sizeof(tmp), " %s=%d", fn, *reinterpret_cast<int*>(base));
            } else if (t == "System.Boolean") {
                snprintf(tmp, sizeof(tmp), " %s=%d", fn, *reinterpret_cast<unsigned char*>(base) ? 1 : 0);
            } else if (t == "System.Single") {
                snprintf(tmp, sizeof(tmp), " %s=%.3f", fn, static_cast<double>(*reinterpret_cast<float*>(base)));
            } else continue;
            out += tmp;
            shown++;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
void dump_recipes_inner(void* image) {
    std::string path = g_gameDir + "hhbc_recipes.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { logf("dump: cannot open %s", path.c_str()); return; }

    void* kOm = il2::find_class(image, "", "OrderManager");
    void* kRecipe = il2::find_class(image, "", "Recipe");
    if (!kOm || !kRecipe) { fprintf(f, "OrderManager or Recipe class missing\n"); fclose(f); return; }
    void* fInst = il2::find_field(kOm, "instance");
    void* om = nullptr;
    if (fInst) il2::field_static_get_value(fInst, &om);
    if (!om) { fprintf(f, "OrderManager.instance is null - start a shift first, then dump again\n"); fclose(f); return; }

    int oName = il2::field_off(kRecipe, "recipeName");
    int oSand = il2::field_off(kRecipe, "isSandwich");
    int oDrink = il2::field_off(kRecipe, "isDrink");
    int oSide = il2::field_off(kRecipe, "isSide");
    int oBaked = il2::field_off(kRecipe, "isBakedGood");
    int oPrice = il2::field_off(kRecipe, "price");
    int oIng = il2::field_off(kRecipe, "ingredients");

    std::set<void*> done;
    auto dump_recipe = [&](void* r) {
        if (!r || !done.insert(r).second) return;
        fprintf(f, "Recipe '%s' asset='%s' sandwich=%d drink=%d side=%d baked=%d price=%d\n",
                il2::to_utf8(rd_ptr(r, oName)).c_str(), uname(r).c_str(),
                rd_bool(r, oSand), rd_bool(r, oDrink), rd_bool(r, oSide), rd_bool(r, oBaked),
                oPrice >= 0 ? rd_int(r, oPrice) : -1);
        void* list = oIng >= 0 ? rd_ptr(r, oIng) : nullptr;
        int n = list ? rd_int(list, o_listSize) : 0;
        void* arr = list ? rd_ptr(list, o_listItems) : nullptr;
        for (int i = 0; arr && i < n && i < 40; i++) {
            void* ing = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
            fprintf(f, "    ingredient[%d] asset='%s'%s\n", i, uname(ing).c_str(), describe_simple(ing, 8).c_str());
        }
    };
    auto dump_array = [&](const char* label, const char* field) {
        int off = il2::field_off(kOm, field);
        void* arr = off >= 0 ? rd_ptr(om, off) : nullptr;
        int n = arr ? static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18)) : 0;
        fprintf(f, "\n== %s (%d)\n", label, n);
        for (int i = 0; arr && i < n && i < 200; i++) dump_recipe(*reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i));
    };
    auto dump_group = [&](const char* label, const char* field) {
        int off = il2::field_off(kOm, field);
        void* grp = off >= 0 ? rd_ptr(om, off) : nullptr;
        fprintf(f, "\n== %s\n", label);
        if (!grp) return;
        void* kg = il2::object_get_class(grp);
        int oRecipes = il2::field_off(kg, "recipes");
        void* list = oRecipes >= 0 ? rd_ptr(grp, oRecipes) : nullptr;
        int n = list ? rd_int(list, o_listSize) : 0;
        void* arr = list ? rd_ptr(list, o_listItems) : nullptr;
        fprintf(f, "group has %d recipes\n", n);
        for (int i = 0; arr && i < n && i < 200; i++) dump_recipe(*reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i));
    };

    dump_array("availableRecipes", "availableRecipes");
    dump_array("_sandwichRecipes", "_sandwichRecipes");
    dump_array("_nonSandwichRecipes", "_nonSandwichRecipes");
    dump_group("levelRecipes", "levelRecipes");
    dump_group("levelVariantRecipes", "levelVariantRecipes");
    int oWeird = il2::field_off(kOm, "weirdSandwich");
    fprintf(f, "\n== weirdSandwich\n");
    if (oWeird >= 0) dump_recipe(rd_ptr(om, oWeird));
    fclose(f);
    logf("dump written: %s (%d recipes)", path.c_str(), static_cast<int>(done.size()));
}

// ---------------------------------------------------------------------------
void dump_world_inner(void* image) {
    std::string path = g_gameDir + "hhbc_world.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { logf("dump: cannot open %s", path.c_str()); return; }
    if (!s_findObjects) { fprintf(f, "Object.FindObjectsOfType(Type) not found\n"); fclose(f); return; }

    // the player first
    void* kPlayer = il2::find_class(image, "", "ScytheNetworkPlayer");
    void* fInst = kPlayer ? il2::find_field(kPlayer, "instance") : nullptr;
    void* player = nullptr;
    if (fInst) il2::field_static_get_value(fInst, &player);
    float p[3] = {};
    if (player && position_of(player, p)) fprintf(f, "PLAYER pos=(%.2f, %.2f, %.2f)\n\n", p[0], p[1], p[2]);
    else fprintf(f, "PLAYER not found\n\n");

    static const char* classes[] = {
        "CookingZone", "Meat", "MeatCookingVisual", "AssemblyBoard", "AssemblyBoardTrigger", "AssemblyBoardHelper",
        "DrinkMachine", "DrinkMachineTrigger", "DrinkMachineButton", "ScytheCupDispenser",
        "ShakeMachine", "Fryer", "FryerBasket", "FryerControl", "FryerTrigger",
        "Oven", "OvenTray", "OvenTrigger", "BakedGood", "BakedGoodsFoodSources", "BakedGoodsDumpStation",
        "CashRegister", "BaggingSystem", "BaggedOrder", "IngredientBoxGrab", "SauceDispenser",
        "GenericButton", "FoodContainer"
    };
    for (const char* cn : classes) {
        void* k = il2::find_class(image, "", cn);
        if (!k) { fprintf(f, "== %s: class not found\n", cn); continue; }
        void* typeObj = il2::type_get_object(il2::class_get_type(k));
        void* args[] = { typeObj };
        void* exc = nullptr;
        void* arr = il2::runtime_invoke(s_findObjects, nullptr, args, &exc);
        int n = (!exc && arr) ? static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18)) : -1;
        fprintf(f, "== %s: %d instance(s)\n", cn, n);
        for (int i = 0; arr && i < n && i < 40; i++) {
            void* o = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
            if (!o) continue;
            float pos[3] = {};
            bool ok = position_of(o, pos);
            fprintf(f, "   [%d] name='%s' ptr=%p pos=(%.2f, %.2f, %.2f)%s\n", i, uname(o).c_str(), o,
                    static_cast<double>(pos[0]), static_cast<double>(pos[1]), static_cast<double>(pos[2]), ok ? "" : " (no position)");
            std::string d = describe_simple(o, 10);
            if (!d.empty()) fprintf(f, "        fields:%s\n", d.c_str());
        }
    }
    fclose(f);
    logf("dump written: %s", path.c_str());
}

// SEH guards (no C++ objects with destructors in these functions)
void guard(void (*fn)(void*), void* arg, const char* label) {
    __try { fn(arg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("dump step '%s' crashed (code %08lx) - skipped", label, GetExceptionCode()); }
}

void classes_step(void* image) {
    std::string path = g_gameDir + "hhbc_details.txt";
    std::string path2 = g_gameDir + "hhbc_kitchen.txt";
    FILE* f = nullptr;
    FILE* f2 = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { logf("dump: cannot open %s", path.c_str()); return; }
    if (fopen_s(&f2, path2.c_str(), "w") != 0) f2 = nullptr;
    size_t n = il2::image_get_class_count(image);
    size_t kitchen = 0;
    for (size_t i = 0; i < n; i++) {
        void* k = il2::image_get_class(image, i);
        if (!k) continue;
        write_class(f, k);
        const char* nm = il2::class_get_name(k);
        if (f2 && nm && is_kitchen(nm)) { write_class(f2, k); kitchen++; }
    }
    fclose(f);
    if (f2) fclose(f2);
    logf("dump written: %s (%zu classes), kitchen subset: %zu classes in hhbc_kitchen.txt", path.c_str(), n, kitchen);
}

}  // namespace

void run_dump(void* image) {
    logf("FULL DUMP started (main thread)");
    resolve_helpers();
    guard(classes_step, image, "classes");
    guard(dump_recipes_inner, image, "recipes");
    guard(dump_world_inner, image, "world");
    logf("FULL DUMP finished");
}
