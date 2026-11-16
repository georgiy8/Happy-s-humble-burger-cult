#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <MinHook.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <cstdarg>
#include <set>
#include <string>
#include <utility>

// ============================================================================
//  DEBUG CONSOLE ("spy")
//  Records what the player does, so the real game logic of appliances, ingredient
//  boxes, drink / shake machines, the register etc. can be studied.
//
//  How: when the user switches Spy ON the first time, we scan Assembly-CSharp and
//  hook (MinHook) these methods, all found BY NAME at runtime:
//    * every  Interact(PurrNet.PlayerID)  - the game's universal "press E" entry
//    * Usable.Use
//    * "*_Original_N" RPC implementations and a few named methods of kitchen classes
//  Every hooked call is logged (class, method, arguments, a few fields of the object)
//  and then forwarded unchanged to the original method.
//  Only methods with <= 2 parameters and simple return types are hooked, so the 4
//  integer registers of the x64 calling convention are enough to forward the call.
// ============================================================================

namespace {

constexpr int kMaxSlots = 512;

struct Slot {
    void* orig = nullptr;
    char cls[64] = {};
    char name[72] = {};
    unsigned char kind[2] = { 0, 0 };   // 0 raw, 1 int/enum, 2 bool, 3 string
    int argc = 0;
    int hits = 0;
    bool isInteract = false;
    ULONGLONG winStart = 0;
    int winCount = 0;
    int hotWindows = 0;
    bool muted = false;
};

Slot g_slots[kMaxSlots];
int g_nSlots = 0;
bool g_installed = false;
ULONGLONG g_t0 = 0;

std::mutex g_mtx;
std::deque<std::string> g_lines;
std::atomic<int> g_version{ 0 };

void push_line(const char* line) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_lines.emplace_back(line);
        while (g_lines.size() > 400) g_lines.pop_front();
    }
    g_version++;
    g.spyEvents++;
    logf("spy %s", line);
}

// ---- name helpers -----------------------------------------------------------
std::string lower(const char* s) {
    std::string r = s ? s : "";
    for (auto& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool kitchenish(const char* name) {
    static const char* kw[] = {
        "grill", "fry", "oven", "shake", "cook", "patty", "burger", "fridge", "station",
        "appliance", "machine", "bun", "drink", "soda", "bake", "tray", "ticket",
        "food", "recipe", "ingredient", "kitchen", "assembl", "toaster", "slicer",
        "cutting", "register", "ordermanager", "basket", "burn", "cup", "sauce", "napkin"
    };
    std::string l = lower(name);
    for (const char* k : kw) if (l.find(k) != std::string::npos) return true;
    return false;
}

bool is_enum_name(const std::string& t) {
    static const char* e[] = { "TemperatureState", "OvenTray.TrayType", "ShakeMachine.ShakeType",
                               "DrinkMachine.DrinkSelection", "FoodType", "IngredientType",
                               "KitchenAppliance", "BakedGoodsType" };
    for (const char* x : e) if (t == x) return true;
    return false;
}

unsigned char kind_of(const std::string& t) {
    if (t == "System.Int32" || is_enum_name(t)) return 1;
    if (t == "System.Boolean") return 2;
    if (t == "System.String") return 3;
    return 0;
}

std::string type_name(void* type) {
    char* tn = type ? il2::type_get_name(type) : nullptr;
    std::string r = tn ? tn : "";
    if (tn && il2::il2_free) il2::il2_free(tn);
    return r;
}

// ---- Unity object name (for GameObject / ScriptableObject fields) -----------
void* s_getName = nullptr;
bool s_getNameTried = false;

std::string unity_name(void* obj) {
    if (!obj) return std::string();
    if (!s_getNameTried) {
        s_getNameTried = true;
        void* core = il2::find_image("UnityEngine.CoreModule.dll");
        void* kObj = il2::find_class(core, "UnityEngine", "Object");
        if (kObj) s_getName = il2::find_method(kObj, "get_name", 0);
    }
    if (!s_getName) return std::string();
    void* exc = nullptr;
    void* r = il2::runtime_invoke(s_getName, obj, nullptr, &exc);
    if (exc || !r) return std::string();
    return il2::to_utf8(r);
}

// ---- describe a few fields of the object that was interacted with ----------
std::string describe_fields(void* self, bool wide) {
    std::string out;
    if (!self) return out;
    void* k = il2::object_get_class(self);
    if (!k) return out;
    void* it = nullptr;
    int shown = 0;
    while (void* f = il2::class_get_fields(k, &it)) {
        if (shown >= (wide ? 8 : 5)) break;
        int fl = il2::field_get_flags(f);
        if (fl & 0x10) continue;      // static
        if (fl & 0x40) continue;      // literal
        size_t off = il2::field_get_offset(f);
        if (off < 0x10 || off > 0x4000) continue;
        const char* fn = il2::field_get_name(f);
        if (!fn) continue;
        std::string t = type_name(il2::field_get_type(f));
        char tmp[160] = {};
        char* base = static_cast<char*>(self) + off;
        if (t == "System.Int32" || is_enum_name(t)) {
            snprintf(tmp, sizeof(tmp), " %s=%d", fn, *reinterpret_cast<int*>(base));
        } else if (t == "System.Boolean") {
            snprintf(tmp, sizeof(tmp), " %s=%d", fn, *reinterpret_cast<unsigned char*>(base) ? 1 : 0);
        } else if (t == "System.String") {
            std::string v = il2::to_utf8(*reinterpret_cast<void**>(base));
            if (v.empty()) continue;
            if (v.size() > 28) v.resize(28);
            snprintf(tmp, sizeof(tmp), " %s='%s'", fn, v.c_str());
        } else if (t == "UnityEngine.GameObject" || t == "Ingredient" || t == "Recipe") {
            void* p = *reinterpret_cast<void**>(base);
            if (!p) continue;
            std::string v = unity_name(p);
            if (v.empty()) continue;
            if (v.size() > 28) v.resize(28);
            snprintf(tmp, sizeof(tmp), " %s=<%s>", fn, v.c_str());
        } else if (t.find('<') == std::string::npos && t.find('[') == std::string::npos &&
                   t.find('`') == std::string::npos && t.compare(0, 6, "System") != 0 &&
                   t.compare(0, 5, "Unity") != 0 && t.compare(0, 5, "PurrN") != 0 &&
                   t.compare(0, 5, "TMPro") != 0 && t.compare(0, 6, "Cysharp") != 0 &&
                   t != "SoundPack" && kitchenish(t.c_str())) {
            void* p = *reinterpret_cast<void**>(base);
            if (!p) continue;
            void* pk = il2::object_get_class(p);
            const char* pn = pk ? il2::class_get_name(pk) : nullptr;
            snprintf(tmp, sizeof(tmp), " %s=%s@%p", fn, pn ? pn : "?", p);
        } else {
            continue;
        }
        out += tmp;
        shown++;
    }
    return out;
}

void fmt_arg(std::string& out, unsigned char kind, void* v) {
    char tmp[96];
    switch (kind) {
    case 1:
        snprintf(tmp, sizeof(tmp), "%d", static_cast<int>(reinterpret_cast<intptr_t>(v)));
        out += tmp;
        break;
    case 2:
        out += (reinterpret_cast<uintptr_t>(v) & 0xff) ? "true" : "false";
        break;
    case 3: {
        std::string s = il2::to_utf8(v);
        if (s.size() > 40) s.resize(40);
        out += "'" + s + "'";
        break;
    }
    default:
        snprintf(tmp, sizeof(tmp), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v)));
        out += tmp;
    }
}

// ---- recording --------------------------------------------------------------
void spy_record_inner(int idx, void* self, void* a, void* b) {
    Slot& s = g_slots[idx];
    s.hits++;
    ULONGLONG now = GetTickCount64();

    // per-method flood guard: a method called more than 6x/second for 3 seconds is "noise"
    if (s.muted) return;
    if (now - s.winStart > 1000) { s.winStart = now; s.winCount = 0; }
    if (++s.winCount > 6) {
        if (s.winCount == 7 && ++s.hotWindows >= 3) {
            s.muted = true;
            char ml[160];
            snprintf(ml, sizeof(ml), "(muted noisy method %s.%s)", s.cls, s.name);
            push_line(ml);
        }
        return;
    }

    static int lastIdx = -1;
    static void* lastSelf = nullptr;
    static void* lastA = nullptr;
    static void* lastB = nullptr;
    static ULONGLONG lastT = 0;
    if (idx == lastIdx && self == lastSelf && a == lastA && b == lastB && now - lastT < 250) return;
    lastIdx = idx; lastSelf = self; lastA = a; lastB = b; lastT = now;

    static ULONGLONG winStart = 0;
    static int winCount = 0;
    if (now - winStart > 1000) { winStart = now; winCount = 0; }
    if (++winCount > 40) return;      // never flood

    std::string args;
    void* av[2] = { a, b };
    for (int i = 0; i < s.argc && i < 2; i++) {
        if (i) args += ", ";
        fmt_arg(args, s.kind[i], av[i]);
    }

    void* rk = il2::object_get_class(self);
    const char* rn = rk ? il2::class_get_name(rk) : nullptr;
    std::string extra = describe_fields(self, s.isInteract);

    char line[520];
    snprintf(line, sizeof(line), "[%6.1f] %s.%s(%s)  obj=%s@%p%s",
             static_cast<double>(now - g_t0) / 1000.0, s.cls, s.name, args.c_str(),
             rn ? rn : "?", self, extra.c_str());
    push_line(line);
}

void spy_record(int idx, void* self, void* a, void* b) {
    if (g.ownCall.load()) return;          // calls made by the mod itself are not "player actions"
    __try { spy_record_inner(idx, self, a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- generic detours: slot I forwards 4 integer registers to the original ---
using Fn4 = void* (*)(void*, void*, void*, void*);

template <int I>
void* Detour(void* self, void* a, void* b, void* c) {
    if (g.spyOn.load(std::memory_order_relaxed)) spy_record(I, self, a, b);
    return reinterpret_cast<Fn4>(g_slots[I].orig)(self, a, b, c);
}

template <int... Is>
auto make_table(std::integer_sequence<int, Is...>) {
    return std::array<Fn4, sizeof...(Is)>{ { &Detour<Is>... } };
}

const auto kDetours = make_table(std::make_integer_sequence<int, kMaxSlots>{});

// ---- install ----------------------------------------------------------------
bool return_ok(const std::string& r) {
    return r == "System.Void" || r == "System.Boolean" || r == "System.Int32" || r == "System.String";
}

bool param_ok(const std::string& t) {
    return t != "System.Single" && t != "System.Double";   // floats travel in xmm registers
}

void install_hooks() {
    if (g_installed) return;
    g_installed = true;
    g_t0 = GetTickCount64();

    void* image = il2::find_image("Assembly-CSharp.dll");
    if (!image) { logf("spy: Assembly-CSharp not found"); return; }

    static const char* kNamed[] = {
        "OnFryerButtonPressed", "LoadOven", "ToggleOvenDoor", "TopKnobControl", "BottomKnobControl",
        "PourStrawberry", "PourBlueberry", "SelectSodaVariety", "RequestIceFill", "RequestSodaFill",
        "ProcessIngredient", "FinishBurger", "OwnerRequestItem", "LoadCup", "SlideCup", "Toggle",
        "ClearCurrentOrder", "AddFoodItemByName", "SendSubmitOrder"
    };

    // pass 1 hooks EVERY (simple) method of these classes: hand / items / appliances / bag / board
    static const char* kClasses[] = {
        "GenericObject", "Hand", "Usable", "Meat", "MeatCookingVisual", "CookingZone",
        "AssemblyBoard", "AssemblyBoardTrigger", "AssemblyBoardHelper",
        "DrinkMachine", "DrinkMachineTrigger", "DrinkMachineButton", "ScytheCupDispenser", "ShakeMachine",
        "Fryer", "FryerBasket", "FryerControl", "FryerTrigger",
        "Oven", "OvenTray", "OvenTrigger", "BakedGood", "BakedGoodsFoodSources", "BakedGoodsDumpStation",
        "BaggingSystem", "BaggedOrder", "IngredientBoxGrab", "SauceDispenser", "CashRegister"
    };
    static const char* kSkipNames[] = {
        "Update", "LateUpdate", "FixedUpdate", "OnGUI", "Awake", "Start", "OnEnable", "OnDisable", "OnDestroy",
        "ToString", "Equals", "GetHashCode", "RegisterSyncVars", "RegisterSyncVar", "SubscribeToSyncVars",
        "UnsubscribeFromSyncVars", "InitializeFromCurrentState", "OnSpawned", "OnDespawned", "OnValidate"
    };

    std::set<void*> seen;
    int failed = 0, nInteract = 0, nOther = 0;
    size_t n = il2::image_get_class_count(image);
    for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n && g_nSlots < kMaxSlots; i++) {
        void* k = il2::image_get_class(image, i);
        if (!k) continue;
        const char* cn = il2::class_get_name(k);
        const char* ns = il2::class_get_namespace(k);
        if (!cn || cn[0] == '<') continue;
        if (ns && strncmp(ns, "PurrNet.CodeGen", 15) == 0) continue;
        const bool kit = kitchenish(cn);
        bool inList = false;
        for (const char* c : kClasses) if (strcmp(cn, c) == 0) { inList = true; break; }
        if (pass == 1 && !inList) continue;

        void* it = nullptr;
        while (void* m = il2::class_get_methods(k, &it)) {
            if (g_nSlots >= kMaxSlots) break;
            const char* mn = il2::method_get_name(m);
            if (!mn) continue;
            int pc = static_cast<int>(il2::method_get_param_count(m));
            if (pc > 2) continue;

            uint32_t iflags = 0;
            uint32_t fl = il2::method_get_flags(m, &iflags);
            if (fl & 0x10) continue;       // static
            if (fl & 0x400) continue;      // abstract

            std::string p[2];
            for (int q = 0; q < pc; q++) p[q] = type_name(il2::method_get_param(m, static_cast<uint32_t>(q)));

            bool take = false, isInteract = false;
            if (pass == 1) {
                take = true;
                if (mn[0] == '.' || strncmp(mn, "get_", 4) == 0 || strncmp(mn, "set_", 4) == 0 ||
                    strncmp(mn, "add_", 4) == 0 || strncmp(mn, "remove_", 7) == 0 ||
                    strstr(mn, "HandleRPC") || strstr(mn, "OnReceivedRpc") || strstr(mn, "CodeGen")) take = false;
                for (const char* sk : kSkipNames) if (strcmp(mn, sk) == 0) { take = false; break; }
            }
            else if (strcmp(mn, "Interact") == 0 && pc == 1 && p[0] == "PurrNet.PlayerID") { take = true; isInteract = true; }
            else if (strcmp(cn, "Usable") == 0 && strcmp(mn, "Use") == 0) take = true;
            else if (kit) {
                if (strstr(mn, "_Original_")) take = true;
                else for (const char* nm : kNamed) if (strcmp(mn, nm) == 0) { take = true; break; }
            }
            if (!take) continue;

            if (!return_ok(type_name(il2::method_get_return_type(m)))) continue;
            bool okp = true;
            for (int q = 0; q < pc; q++) if (!param_ok(p[q])) okp = false;
            if (!okp) continue;

            void* target = il2::native(m);
            if (!target) continue;
            if (!seen.insert(target).second) continue;

            Slot& s = g_slots[g_nSlots];
            snprintf(s.cls, sizeof(s.cls), "%s", cn);
            snprintf(s.name, sizeof(s.name), "%s", mn);
            s.argc = pc;
            for (int q = 0; q < pc; q++) s.kind[q] = kind_of(p[q]);
            s.isInteract = isInteract;

            MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(kDetours[g_nSlots]), &s.orig);
            if (st == MH_OK) st = MH_EnableHook(target);
            if (st != MH_OK) {
                if (failed++ < 6) logf("spy: hook %s.%s failed: %s", cn, mn, MH_StatusToString(st));
                continue;
            }
            g_nSlots++;
            if (isInteract) nInteract++; else nOther++;
        }
    }
    g.spyHooks = g_nSlots;
    logf("spy: installed %d hooks (%d Interact, %d other), %d failed", g_nSlots, nInteract, nOther, failed);
}

}  // namespace

// ============================================================================
void spy_note(const char* fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!g.spyOn.load()) { logf("mod: %s", msg); return; }
    char line[440];
    snprintf(line, sizeof(line), "MOD >> %s", msg);
    push_line(line);
}

void spy_mark(const char* label) {
    char line[200];
    snprintf(line, sizeof(line), "==== MARK: %s ====", label ? label : "?");
    push_line(line);
}

void spy_tick() {
    if (g.spyClear.exchange(0)) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_lines.clear();
        g_version++;
    }
    if (g.spyInstall.exchange(0) && !g_installed) install_hooks();
}

size_t spy_tail(std::vector<std::string>& out, size_t maxLines) {
    std::lock_guard<std::mutex> lk(g_mtx);
    size_t start = g_lines.size() > maxLines ? g_lines.size() - maxLines : 0;
    out.clear();
    for (size_t i = start; i < g_lines.size(); i++) {
        std::string l = g_lines[i];
        if (l.size() > 128) { l.resize(125); l += "..."; }
        out.push_back(l);
    }
    return out.size();
}

int spy_version() { return g_version.load(); }
