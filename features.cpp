#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>

// ============================================================================
//  Everything here runs on the GAME'S MAIN THREAD, from a detour on
//  OrderManager.LateUpdate (called every frame). All game classes/fields/methods
//  are looked up by name through the il2cpp API; nothing is hard-coded.
// ============================================================================

namespace {

// ---- looked-up data -------------------------------------------------------
void* k_player = nullptr;      void* f_playerInstance = nullptr;
int o_cur = -1, o_max = -1, o_drain = -1, o_walk = -1, o_run = -1;

void* k_om = nullptr;  void* f_omInstance = nullptr;
int o_pending = -1;
void* m_getRegisters = nullptr;

void* k_reg = nullptr;
int o_windowType = -1, o_pendingTrunc = -1;
void* m_clear = nullptr; void* m_add = nullptr; void* m_submit = nullptr;

int o_pcoOrder = -1, o_pcoType = -1, o_pcoTimedOut = -1;   // PendingCustomerOrder
int o_coItems = -1;                                        // CustomerOrder.OrderItems
int o_recipeName = -1;                                     // Recipe.recipeName
int o_listItems = 0x10, o_listSize = 0x18;                 // List<T> (fallback values)

void* m_setLock = nullptr;  void* m_setVisible = nullptr;  // UnityEngine.Cursor
void* m_setTimeScale = nullptr;                            // UnityEngine.Time

// ---- hook trampolines -----------------------------------------------------
using FnLateUpdate = void (*)(void* self, void* mi);
FnLateUpdate o_lateUpdate = nullptr;
void* m_lateUpdateMI = nullptr;

using FnMouseLook = uint64_t (*)(void* mi);
FnMouseLook o_mouseLook = nullptr;

// ---- player state ---------------------------------------------------------
struct Orig { float walk, run; };
std::map<void*, Orig> s_orig;      // original speeds per player instance
bool s_speedApplied = false;
bool s_timeApplied = false;
bool s_cursorApplied = false;

// ---- auto-accept state ----------------------------------------------------
struct RegState { void* pending = nullptr; int attempts = 0; ULONGLONG last = 0; std::string text; };
int o_availRecipes = -1;
int o_levelVariants = -1;
std::map<void*, RegState> s_reg;
ULONGLONG s_lastAcceptScan = 0;
void* s_image = nullptr;
int s_logBudget = 300;              // limit diagnostic lines

inline void* rd_ptr(void* base, int off) { return *reinterpret_cast<void**>(static_cast<char*>(base) + off); }
inline int   rd_int(void* base, int off) { return *reinterpret_cast<int*>(static_cast<char*>(base) + off); }
inline float& rf(void* base, int off)    { return *reinterpret_cast<float*>(static_cast<char*>(base) + off); }

// ---------------------------------------------------------------------------
//  Player tweaks
// ---------------------------------------------------------------------------
// ---- jumps -----------------------------------------------------------------
// ECM2.Character is not in the class dump, so its jump members are looked up by name at runtime.
int field_off_deep(void* k, const char* name) {
    for (void* c = k; c; c = il2::class_get_parent(c)) {
        int o = il2::field_off(c, name);
        if (o >= 0) return o;
    }
    return -1;
}
int o_fps = -1, o_defJump = -1;
int o_jImpulse = -1, o_jMax = -1, o_jCount = -1;
bool s_jumpInit = false;
struct JOrig { float def; float impulse; int maxCount; };
std::map<void*, JOrig> s_jorig;
bool s_jumpApplied = false;

void jump_init(void* p) {
    s_jumpInit = true;
    void* kp = il2::object_get_class(p);
    o_fps = field_off_deep(kp, "fpsChar");
    o_defJump = field_off_deep(kp, "defaultJumpPower");
    void* kc = nullptr;
    if (o_fps >= 0) {
        void* ch = rd_ptr(p, o_fps);
        if (ch) kc = il2::object_get_class(ch);
    }
    if (kc) {
        static const char* const imp[] = { "_jumpImpulse", "jumpImpulse", "m_JumpImpulse" };
        static const char* const mx[] = { "_jumpMaxCount", "jumpMaxCount", "m_JumpMaxCount" };
        static const char* const cnt[] = { "_jumpCount", "jumpCount", "m_JumpCount" };
        for (auto n : imp) if (o_jImpulse < 0) o_jImpulse = field_off_deep(kc, n);
        for (auto n : mx) if (o_jMax < 0) o_jMax = field_off_deep(kc, n);
        for (auto n : cnt) if (o_jCount < 0) o_jCount = field_off_deep(kc, n);
        logf("jump: character class '%s' impulse=0x%x maxCount=0x%x count=0x%x; player defaultJumpPower=0x%x",
             il2::class_get_name(kc), o_jImpulse, o_jMax, o_jCount, o_defJump);
        for (void* c = kc; c; c = il2::class_get_parent(c)) {          // diagnostics: every jump-ish member
            void* it = nullptr; void* f;
            while ((f = il2::class_get_fields(c, &it)) != nullptr) {
                const char* n = il2::field_get_name(f);
                if (n && (strstr(n, "ump") || strstr(n, "rounded")))
                    logf("jump:   %s.%s @0x%x", il2::class_get_name(c), n, (unsigned)il2::field_get_offset(f));
            }
        }
    } else logf("jump: fpsChar object not available (fpsChar=0x%x), using defaultJumpPower only", o_fps);
}

void jump_tick(void* p) {
    if (!s_jumpInit) jump_init(p);
    void* ch = (o_fps >= 0) ? rd_ptr(p, o_fps) : nullptr;
    const bool want = g.jumpOn.load() || g.infJump.load();
    if (want) {
        auto it = s_jorig.find(p);
        if (it == s_jorig.end()) {
            JOrig o{ 0, 0, 0 };
            if (o_defJump >= 0) o.def = rf(p, o_defJump);
            if (ch && o_jImpulse >= 0) o.impulse = rf(ch, o_jImpulse);
            if (ch && o_jMax >= 0) o.maxCount = rd_int(ch, o_jMax);
            it = s_jorig.emplace(p, o).first;
        }
        float m = g.jumpOn.load() ? static_cast<float>(g.jumpPct.load()) / 100.0f : 1.0f;
        if (m < 0.5f) m = 0.5f;
        if (m > 5.0f) m = 5.0f;
        if (o_defJump >= 0) rf(p, o_defJump) = it->second.def * m;
        if (ch && o_jImpulse >= 0) rf(ch, o_jImpulse) = it->second.impulse * m;
        if (ch && o_jMax >= 0) *reinterpret_cast<int*>(static_cast<char*>(ch) + o_jMax) = g.infJump.load() ? 9999 : it->second.maxCount;
        if (ch && o_jCount >= 0 && g.infJump.load()) *reinterpret_cast<int*>(static_cast<char*>(ch) + o_jCount) = 0;
        s_jumpApplied = true;
    } else if (s_jumpApplied) {
        for (auto& kv : s_jorig) {
            void* pl = kv.first;
            void* c = (o_fps >= 0) ? rd_ptr(pl, o_fps) : nullptr;
            if (o_defJump >= 0) rf(pl, o_defJump) = kv.second.def;
            if (c && o_jImpulse >= 0) rf(c, o_jImpulse) = kv.second.impulse;
            if (c && o_jMax >= 0) *reinterpret_cast<int*>(static_cast<char*>(c) + o_jMax) = kv.second.maxCount;
        }
        s_jorig.clear();
        s_jumpApplied = false;
    }
}

void player_tick() {
    void* p = nullptr;
    if (f_playerInstance) il2::field_static_get_value(f_playerInstance, &p);
    if (!p || o_cur < 0) { g.playerFound = 0; return; }
    g.playerFound = 1;

    static void* s_lastPlayer = nullptr;
    if (p != s_lastPlayer) {            // player object was recreated: old pointers are stale
        s_orig.clear();
        s_jorig.clear();
        s_jumpApplied = false;
        s_lastPlayer = p;
    }

    if (g.infStamina.load()) {
        rf(p, o_cur) = rf(p, o_max);
        rf(p, o_drain) = 0.0f;
    }

    if (g.speedOn.load()) {
        auto it = s_orig.find(p);
        if (it == s_orig.end()) {
            it = s_orig.emplace(p, Orig{ rf(p, o_walk), rf(p, o_run) }).first;
        }
        float m = static_cast<float>(g.speedPct.load()) / 100.0f;
        if (m < 0.5f) m = 0.5f;
        if (m > 4.0f) m = 4.0f;
        rf(p, o_walk) = it->second.walk * m;
        rf(p, o_run) = it->second.run * m;
        s_speedApplied = true;
    } else if (s_speedApplied) {
        for (auto& kv : s_orig) {
            rf(kv.first, o_walk) = kv.second.walk;
            rf(kv.first, o_run) = kv.second.run;
        }
        s_orig.clear();
        s_speedApplied = false;
    }
    jump_tick(p);
}

// ---------------------------------------------------------------------------
//  Time scale / cursor
// ---------------------------------------------------------------------------
void world_tick() {
    if (m_setTimeScale) {
        auto fn = reinterpret_cast<void (*)(float, void*)>(il2::native(m_setTimeScale));
        if (g.timeOn.load()) {
            float t = static_cast<float>(g.timePct.load()) / 100.0f;
            fn(t, m_setTimeScale);
            s_timeApplied = true;
        } else if (s_timeApplied) {
            fn(1.0f, m_setTimeScale);
            s_timeApplied = false;
        }
    }
}

void cursor_tick() {
    if (!m_setLock || !m_setVisible) return;
    auto lock = reinterpret_cast<void (*)(int, void*)>(il2::native(m_setLock));
    auto vis = reinterpret_cast<void (*)(bool, void*)>(il2::native(m_setVisible));
    if (g.menuOpen.load()) {
        lock(0, m_setLock);        // CursorLockMode.None
        vis(true, m_setVisible);
        s_cursorApplied = true;
    } else if (s_cursorApplied) {
        lock(1, m_setLock);        // CursorLockMode.Locked
        vis(false, m_setVisible);
        s_cursorApplied = false;
    }
}

// ---------------------------------------------------------------------------
//  Auto-accept orders: do on the register what the player would do:
//  clear -> add each item by name -> submit.
// ---------------------------------------------------------------------------
void accept_register(void* reg, void* pco, RegState& st, ULONGLONG now) {
    void* order = rd_ptr(pco, o_pcoOrder);
    if (!order) return;
    void* items = rd_ptr(order, o_coItems);
    if (!items) return;
    int count = rd_int(items, o_listSize);
    void* arr = rd_ptr(items, o_listItems);
    if (!arr || count <= 0 || count > 40) return;

    if (st.pending != pco) { st.pending = pco; st.attempts = 0; st.last = 0; }
    if (st.attempts >= 3) return;
    ULONGLONG gap = static_cast<ULONGLONG>(g.acceptDelayMs.load());
    if (gap < 1500) gap = 1500;                       // do not hammer the server
    if (st.last && now - st.last < gap) return;

    auto clearF = reinterpret_cast<void (*)(void*, void*)>(il2::native(m_clear));
    auto addF = reinterpret_cast<void (*)(void*, void*, void*)>(il2::native(m_add));
    auto submitF = reinterpret_cast<void (*)(void*, void*)>(il2::native(m_submit));

    g.acceptMode = 1;
    g.ownCall = 1;
    clearF(reg, m_clear);
    for (int i = 0; i < count; i++) {
        void* recipe = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
        if (!recipe) continue;
        void* name = rd_ptr(recipe, o_recipeName);
        if (!name) continue;
        if (s_logBudget > 0) { s_logBudget--; logf("auto-accept: add '%s'", il2::to_utf8(name).c_str()); }
        addF(reg, name, m_add);
    }
    submitF(reg, m_submit);
    g.ownCall = 0;
    st.attempts++;
    st.last = now;
    g.acceptedCount++;
    if (s_logBudget > 0) { s_logBudget--; logf("auto-accept: submitted (%d items, attempt %d)", count, st.attempts); }
}

// ---------------------------------------------------------------------------
//  CLIENT mode: pendingOrders exists only on the server, so we read the text the
//  register shows to the player ("Flower Crown\nJazzy Java") and enter those items.
// ---------------------------------------------------------------------------
std::vector<std::string> split_items(const std::string& t) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(" \t");
        size_t b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (char c : t) { if (c == '\n' || c == '\r' || c == '|') flush(); else cur += c; }
    flush();
    return out;
}

bool recipe_known(void* om, const std::string& name) {
    if (o_availRecipes < 0) return true;
    void* ar = rd_ptr(om, o_availRecipes);
    if (!ar) return true;
    int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(ar) + 0x18));
    if (n <= 0 || n > 200) return true;
    for (int i = 0; i < n; i++) {
        void* rc = *reinterpret_cast<void**>(static_cast<char*>(ar) + 0x20 + 8 * i);
        if (rc && il2::to_utf8(rd_ptr(rc, o_recipeName)) == name) return true;
    }
    // orders with removed ingredients ("Meat Heater (No BBQ)") live in levelVariantRecipes (RecipeGroup.recipes)
    if (o_levelVariants >= 0) {
        void* grp = rd_ptr(om, o_levelVariants);
        if (grp) {
            int oRec = il2::field_off(il2::object_get_class(grp), "recipes");
            void* list = oRec >= 0 ? rd_ptr(grp, oRec) : nullptr;
            int vn = list ? rd_int(list, o_listSize) : 0;
            void* varr = list ? rd_ptr(list, o_listItems) : nullptr;
            for (int i = 0; varr && i < vn && i < 300; i++) {
                void* rc = *reinterpret_cast<void**>(static_cast<char*>(varr) + 0x20 + 8 * i);
                if (rc && il2::to_utf8(rd_ptr(rc, o_recipeName)) == name) return true;
            }
        }
    }
    return false;
}

void accept_from_text(void* om, void* reg, const std::string& text, RegState& st, ULONGLONG now) {
    std::vector<std::string> items = split_items(text);
    if (items.empty() || items.size() > 40) return;

    if (st.text != text) { st.text = text; st.attempts = 0; st.last = 0; }
    if (st.attempts >= 3) return;
    ULONGLONG gap = static_cast<ULONGLONG>(g.acceptDelayMs.load());
    if (gap < 1500) gap = 1500;
    if (st.last && now - st.last < gap) return;

    for (const std::string& n : items) {
        if (!recipe_known(om, n)) {
            st.attempts = 3;                       // do not retry this text
            if (s_logBudget > 0) { s_logBudget--; logf("auto-accept(client): unknown item '%s' - order skipped", n.c_str()); }
            return;
        }
    }

    auto clearF = reinterpret_cast<void (*)(void*, void*)>(il2::native(m_clear));
    auto addF = reinterpret_cast<void (*)(void*, void*, void*)>(il2::native(m_add));
    auto submitF = reinterpret_cast<void (*)(void*, void*)>(il2::native(m_submit));

    g.acceptMode = 2;
    g.ownCall = 1;
    clearF(reg, m_clear);
    for (const std::string& n : items) {
        if (s_logBudget > 0) { s_logBudget--; logf("auto-accept(client): add '%s'", n.c_str()); }
        addF(reg, il2::string_new(n.c_str()), m_add);
    }
    submitF(reg, m_submit);
    g.ownCall = 0;
    st.attempts++;
    st.last = now;
    g.acceptedCount++;
    if (s_logBudget > 0) { s_logBudget--; logf("auto-accept(client): submitted (%d items, attempt %d)", static_cast<int>(items.size()), st.attempts); }
}

void accept_tick(void* om) {
    if (!g.autoAccept.load() || !m_getRegisters || o_pending < 0) return;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastAcceptScan < 250) return;
    s_lastAcceptScan = now;

    void* pendList = rd_ptr(om, o_pending);
    int pcount = pendList ? rd_int(pendList, o_listSize) : 0;
    void* parr = pendList ? rd_ptr(pendList, o_listItems) : nullptr;
    if (!parr || pcount < 0 || pcount > 64) pcount = 0;      // client: list is empty -> text mode below

    auto getRegs = reinterpret_cast<void* (*)(void*, void*)>(il2::native(m_getRegisters));
    void* regs = getRegs(om, m_getRegisters);
    if (!regs) return;
    int rn = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(regs) + 0x18));
    if (rn <= 0 || rn > 16) return;

    for (int r = 0; r < rn; r++) {
        void* reg = *reinterpret_cast<void**>(static_cast<char*>(regs) + 0x20 + 8 * r);
        if (!reg) continue;
        // a customer is standing at this register only if it has a pending order text
        void* trunc = rd_ptr(reg, o_pendingTrunc);
        std::string text = trunc ? il2::to_utf8(trunc) : std::string();
        if (text.empty()) { s_reg.erase(reg); continue; }
        int wt = rd_int(reg, o_windowType);
        bool done = false;
        for (int i = 0; i < pcount && !done; i++) {
            void* pco = *reinterpret_cast<void**>(static_cast<char*>(parr) + 0x20 + 8 * i);
            if (!pco) continue;
            if (rd_int(pco, o_pcoType) != wt) continue;
            if (*(static_cast<char*>(pco) + o_pcoTimedOut)) continue;
            accept_register(reg, pco, s_reg[reg], now);
            done = true;
        }
        if (!done) accept_from_text(om, reg, text, s_reg[reg], now);
    }
}

// ---------------------------------------------------------------------------
//  client-probe: logs the text shown on each register (to study the order
//  format when playing as a client). Rate-limited, logs only on change.
// ---------------------------------------------------------------------------
void client_probe(void* om) {
    if (!m_getRegisters || o_pendingTrunc < 0) return;
    static ULONGLONG last = 0;
    static int budget = 150;
    if (budget <= 0) return;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<void*, std::string> seen;

    void* pl = o_pending >= 0 ? rd_ptr(om, o_pending) : nullptr;
    int pcount = pl ? rd_int(pl, o_listSize) : -1;

    auto getRegs = reinterpret_cast<void* (*)(void*, void*)>(il2::native(m_getRegisters));
    void* regs = getRegs(om, m_getRegisters);
    if (!regs) return;
    int rn = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(regs) + 0x18));
    if (rn <= 0 || rn > 16) return;
    for (int r = 0; r < rn; r++) {
        void* reg = *reinterpret_cast<void**>(static_cast<char*>(regs) + 0x20 + 8 * r);
        if (!reg) continue;
        std::string t = il2::to_utf8(rd_ptr(reg, o_pendingTrunc));
        if (t.empty()) { seen.erase(reg); continue; }
        if (seen[reg] == t) continue;
        seen[reg] = t;
        for (char& c : t) if (c == '\n' || c == '\r') c = '|';
        budget--;
        logf("client-probe: reg#%d windowType=%d pendingCount=%d text='%s'",
             r, rd_int(reg, o_windowType), pcount, t.c_str());
    }
}

// ---------------------------------------------------------------------------
//  SEH wrappers (no C++ objects with destructors in these functions)
// ---------------------------------------------------------------------------
int s_faults = 0;
void guarded_tick(void* om) {
    __try {
        if (g.ready.load()) overlay_ensure_host();
        if (!om && f_omInstance) il2::field_static_get_value(f_omInstance, &om);
        player_tick();
        world_tick();
        cursor_tick();
        spy_tick();
        lab_tick();
        if (g.requestDump.exchange(0) && s_image) run_dump(s_image);   // main thread: Unity objects may be touched
        if (om) { accept_tick(om); client_probe(om); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g.ownCall = 0;
        if (s_faults++ < 5) logf("SEH exception in tick (code %08lx) - feature skipped this frame", GetExceptionCode());
    }
}

bool our_window_focused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---- key binds -------------------------------------------------------------
// 0 menu, 1 stamina, 2 speed, 3 game speed, 4 auto-accept, 5 spy, 6..10 lab steps.
// Only the menu key has a default (Insert). Everything else starts unbound; while
// rebinding, Esc clears the bind (the menu key falls back to Insert).
const char* const kBindNames[State::kBindCount] = {
    "Open / close menu", "Infinite stamina (toggle)", "Speed multiplier (toggle)", "Game speed (toggle)",
    "Auto-accept orders (toggle)", "Spy (toggle)", "Lab 1: take item", "Lab 2: drop on grill",
    "Lab 3: toggle grill", "Lab 4: cooked -> board", "Lab 5: report",
    "Alpha cooking (whole order)", "Cancel all kitchen jobs", "Beta closeout (after-shift duties)" };

std::string binds_path() { return g_gameDir + "hhbc_binds.ini"; }

void binds_save() {
    FILE* f = nullptr;
    if (fopen_s(&f, binds_path().c_str(), "w") != 0 || !f) return;
    for (int i = 0; i < State::kBindCount; i++) fprintf(f, "%d=%d\n", i, g.bind[i].load());
    fclose(f);
}

void binds_load() {
    FILE* f = nullptr;
    if (fopen_s(&f, binds_path().c_str(), "r") != 0 || !f) return;
    int i = 0, v = 0;
    while (fscanf_s(f, "%d=%d\n", &i, &v) == 2) {
        if (i >= 0 && i < State::kBindCount && v >= 0 && v < 256) g.bind[i] = v;
    }
    fclose(f);
}

void run_bind(int i) {
    switch (i) {
    case 0: g.menuOpen = g.menuOpen.load() ? 0 : 1; logf("menu %s", g.menuOpen.load() ? "opened" : "closed"); break;
    case 1: g.infStamina = g.infStamina.load() ? 0 : 1; break;
    case 2: g.speedOn = g.speedOn.load() ? 0 : 1; break;
    case 3: g.timeOn = g.timeOn.load() ? 0 : 1; break;
    case 4: g.autoAccept = g.autoAccept.load() ? 0 : 1; break;
    case 5: { int on = g.spyOn.load() ? 0 : 1; if (on) g.spyInstall = 1; g.spyOn = on; break; }
    case 11: cook_request("alpha"); break;
    case 12: cook_request("cancel"); break;
    case 13: cook_request("closeout"); break;
    default: g.labCmd = i - 5; break;                 // 6..10 -> lab 1..5
    }
}

void hotkeys() {
    static bool prev[256] = {};
    static bool loaded = false;
    if (!loaded) { loaded = true; binds_load(); }
    const bool focused = our_window_focused();
    bool now[256];
    for (int vk = 1; vk < 256; vk++) now[vk] = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
    now[0] = false;

    int listen = g.bindListen.load();
    if (listen >= 0) {
        for (int vk = 3; vk < 256; vk++) {            // 1/2 = left/right mouse: used for clicking the menu
            if (!now[vk] || prev[vk]) continue;
            if (vk == VK_ESCAPE) g.bind[listen] = (listen == 0) ? VK_INSERT : 0;
            else g.bind[listen] = vk;
            g.bindListen = -1;
            binds_save();
            char nm[40];
            vk_name(g.bind[listen].load(), nm, sizeof(nm));
            logf("bind '%s' = %s", kBindNames[listen], nm);
            break;
        }
    } else {
        for (int i = 0; i < State::kBindCount; i++) {
            int vk = g.bind[i].load();
            if (vk > 0 && vk < 256 && now[vk] && !prev[vk]) run_bind(i);
        }
    }
    for (int vk = 0; vk < 256; vk++) prev[vk] = now[vk];
}

void Hook_LateUpdate(void* self, void* mi) {
    o_lateUpdate(self, mi);
    if (g.tickOrder++ == 0) logf("tick source OrderManager.LateUpdate is alive");
    hotkeys();
    guarded_tick(self);
}

uint64_t Hook_MouseLook(void* mi) {
    uint64_t r = o_mouseLook(mi);
    if (g.tickLook++ == 0) logf("tick source GameInput.get_MouseLook is alive");
    hotkeys();
    guarded_tick(nullptr);
    return g.menuOpen.load() ? 0 : r;    // Vector2(0,0): camera does not turn while the menu is open
}

}  // namespace

// ============================================================================
bool features_init(void* image) {
    s_image = image;
    binds_init();
    k_player = il2::find_class(image, "", "ScytheNetworkPlayer");
    if (k_player) {
        f_playerInstance = il2::find_field(k_player, "instance");
        o_cur = il2::field_off(k_player, "currentSprintAbility");
        o_max = il2::field_off(k_player, "maxSprintAbility");
        o_drain = il2::field_off(k_player, "sprintDrainRate");
        o_walk = il2::field_off(k_player, "defaultMaxWalkSpeed");
        o_run = il2::field_off(k_player, "defaultMaxRunSpeed");
        if (o_max < 0 || o_drain < 0 || o_walk < 0 || o_run < 0) o_cur = -1;
    } else logf("class ScytheNetworkPlayer not found");

    k_om = il2::find_class(image, "", "OrderManager");
    if (k_om) {
        f_omInstance = il2::find_field(k_om, "instance");
        o_pending = il2::field_off(k_om, "pendingOrders");
        o_availRecipes = il2::field_off(k_om, "availableRecipes");
        o_levelVariants = il2::field_off(k_om, "levelVariantRecipes");
        m_getRegisters = il2::find_method(k_om, "GetCachedRegisters", 0);
        m_lateUpdateMI = il2::find_method(k_om, "LateUpdate", 0);
    } else logf("class OrderManager not found");

    k_reg = il2::find_class(image, "", "CashRegister");
    if (k_reg) {
        o_windowType = il2::field_off(k_reg, "windowType");
        o_pendingTrunc = il2::field_off(k_reg, "_pendingCustomerTruncOrder");
        m_clear = il2::find_method(k_reg, "ClearCurrentOrder", 0);
        m_add = il2::find_method(k_reg, "AddFoodItemByName", 1);
        m_submit = il2::find_method(k_reg, "SendSubmitOrder", 0);
    } else logf("class CashRegister not found");

    void* kPco = il2::find_class(image, "", "PendingCustomerOrder");
    o_pcoOrder = il2::field_off(kPco, "order");
    o_pcoType = il2::field_off(kPco, "orderType");
    o_pcoTimedOut = il2::field_off(kPco, "hasTimedOut");
    void* kCo = il2::find_class(image, "", "CustomerOrder");
    o_coItems = il2::field_off(kCo, "OrderItems");
    void* kRecipe = il2::find_class(image, "", "Recipe");
    o_recipeName = il2::field_off(kRecipe, "recipeName");

    // List<T> layout from mscorlib
    void* mscorlib = il2::find_image("mscorlib.dll");
    void* kList = il2::find_class(mscorlib, "System.Collections.Generic", "List`1");
    if (kList) {
        int a = il2::field_off(kList, "_items");
        int b = il2::field_off(kList, "_size");
        if (a > 0 && b > 0) { o_listItems = a; o_listSize = b; }
    }
    logf("List<T> layout: _items=0x%x _size=0x%x", o_listItems, o_listSize);

    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* kCursor = il2::find_class(core, "UnityEngine", "Cursor");
    void* kTime = il2::find_class(core, "UnityEngine", "Time");
    if (kCursor) {
        const char* lp[] = { "UnityEngine.CursorLockMode" };
        const char* vp[] = { "System.Boolean" };
        m_setLock = il2::find_method_sig(kCursor, "set_lockState", lp, 1);
        m_setVisible = il2::find_method_sig(kCursor, "set_visible", vp, 1);
    }
    if (kTime) m_setTimeScale = il2::find_method(kTime, "set_timeScale", 1);

    logf("init: player=%d speedOffsets=%d orderMgr=%d register=%d accept(clear/add/submit)=%d/%d/%d cursor=%d/%d time=%d",
         k_player != nullptr, o_cur >= 0, k_om != nullptr, k_reg != nullptr,
         m_clear != nullptr, m_add != nullptr, m_submit != nullptr,
         m_setLock != nullptr, m_setVisible != nullptr, m_setTimeScale != nullptr);
    return o_cur >= 0;
}

bool features_hook() {
    void* image = il2::find_image("Assembly-CSharp.dll");
    if (m_lateUpdateMI) {
        void* target = il2::native(m_lateUpdateMI);
        MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_LateUpdate), reinterpret_cast<void**>(&o_lateUpdate));
        if (s == MH_OK) s = MH_EnableHook(target);
        g.orderHookOk = (s == MH_OK);
        logf("hook OrderManager.LateUpdate: %s", MH_StatusToString(s));
    }
    void* kInput = il2::find_class(image, "", "GameInput");
    void* mLook = il2::find_method(kInput, "get_MouseLook", 0);
    if (mLook) {
        void* target = il2::native(mLook);
        MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_MouseLook), reinterpret_cast<void**>(&o_mouseLook));
        if (s == MH_OK) s = MH_EnableHook(target);
        g.lookHookOk = (s == MH_OK);
        logf("hook GameInput.get_MouseLook: %s", MH_StatusToString(s));
    }
    return g.orderHookOk.load() != 0;
}


const char* bind_action_name(int i) { return (i >= 0 && i < State::kBindCount) ? kBindNames[i] : "?"; }

void binds_init() { g.bind[0] = VK_INSERT; }

void features_order_items(std::vector<std::string>& out) {
    void* om = nullptr;
    if (f_omInstance) il2::field_static_get_value(f_omInstance, &om);
    if (!om || !m_getRegisters) return;
    auto getRegs = reinterpret_cast<void* (*)(void*, void*)>(il2::native(m_getRegisters));
    void* regs = getRegs(om, m_getRegisters);
    if (!regs) return;
    int rn = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(regs) + 0x18));
    if (rn <= 0 || rn > 16) return;
    for (int r = 0; r < rn; r++) {
        void* reg = *reinterpret_cast<void**>(static_cast<char*>(regs) + 0x20 + 8 * r);
        if (!reg) continue;
        void* trunc = rd_ptr(reg, o_pendingTrunc);
        if (!trunc) continue;
        for (auto& n : split_items(il2::to_utf8(trunc))) out.push_back(n);
    }
}

// Items shown on each register (one entry per register that has a customer) - used by "alpha cooking".
void features_register_orders(std::vector<void*>& regs, std::vector<std::vector<std::string>>& orders) {
    void* om = nullptr;
    if (f_omInstance) il2::field_static_get_value(f_omInstance, &om);
    if (!om || !m_getRegisters) return;
    auto getRegs = reinterpret_cast<void* (*)(void*, void*)>(il2::native(m_getRegisters));
    void* arr = getRegs(om, m_getRegisters);
    if (!arr) return;
    int rn = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18));
    if (rn <= 0 || rn > 16) return;
    for (int r = 0; r < rn; r++) {
        void* reg = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * r);
        if (!reg) continue;
        void* trunc = rd_ptr(reg, o_pendingTrunc);
        if (!trunc) continue;
        std::vector<std::string> it = split_items(il2::to_utf8(trunc));
        if (it.empty()) continue;
        regs.push_back(reg);
        orders.push_back(it);
    }
}

// Orders that were already ACCEPTED at a register (OrderManager.activeOrders), oldest first.
void features_accepted_orders(std::vector<AcceptedOrder>& out) {
    out.clear();
    void* om = nullptr;
    if (f_omInstance) il2::field_static_get_value(f_omInstance, &om);
    if (!om || !k_om) return;
    static int oAct = -2, oId = -2, oType = -2, oTime = -2;
    if (oAct == -2) {
        oAct = il2::field_off(k_om, "activeOrders");
        void* kCo = il2::find_class(il2::find_image("Assembly-CSharp.dll"), "", "CustomerOrder");
        oId = il2::field_off(kCo, "Id");
        oType = il2::field_off(kCo, "orderType");
        oTime = il2::field_off(kCo, "timeActivated");
    }
    if (oAct < 0 || o_coItems < 0 || o_recipeName < 0) return;
    void* list = rd_ptr(om, oAct);
    if (!list) return;
    int n = rd_int(list, o_listSize);
    void* arr = rd_ptr(list, o_listItems);
    if (!arr || n <= 0 || n > 64) return;
    for (int i = 0; i < n; i++) {
        void* co = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
        if (!co) continue;
        AcceptedOrder ao;
        ao.key = co;
        ao.id = oId >= 0 ? rd_int(co, oId) : i;
        ao.type = oType >= 0 ? rd_int(co, oType) : 0;
        ao.t = oTime >= 0 ? *reinterpret_cast<float*>(static_cast<char*>(co) + oTime) : 0.0f;
        void* items = rd_ptr(co, o_coItems);
        if (!items) continue;
        int cnt = rd_int(items, o_listSize);
        void* ia = rd_ptr(items, o_listItems);
        if (!ia || cnt <= 0 || cnt > 40) continue;
        for (int k = 0; k < cnt; k++) {
            void* rc = *reinterpret_cast<void**>(static_cast<char*>(ia) + 0x20 + 8 * k);
            if (!rc) continue;
            std::string nm = il2::to_utf8(rd_ptr(rc, o_recipeName));
            if (!nm.empty()) ao.items.push_back(nm);
        }
        if (!ao.items.empty()) out.push_back(ao);
    }
    std::stable_sort(out.begin(), out.end(), [](const AcceptedOrder& a, const AcceptedOrder& b) { return a.t != b.t ? a.t < b.t : a.id < b.id; });
}

// Recipe lookup by name (also the "(No X)" variants). kind: 1 sandwich, 2 drink, 3 side, 4 baked good, 0 unknown.
// ingIds = ingredientID of every ingredient in the recipe's own order.
bool features_recipe_info(const char* name, int& kind, std::vector<std::string>& ingIds) {
    kind = 0; ingIds.clear();
    void* om = nullptr;
    if (f_omInstance) il2::field_static_get_value(f_omInstance, &om);
    if (!om || o_recipeName < 0) return false;
    void* found = nullptr;
    auto scan = [&](void* arr, int n) {
        for (int i = 0; arr && i < n && !found; i++) {
            void* rc = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
            if (rc && il2::to_utf8(rd_ptr(rc, o_recipeName)) == name) found = rc;
        }
    };
    if (o_availRecipes >= 0) {
        void* ar = rd_ptr(om, o_availRecipes);
        if (ar) { int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(ar) + 0x18)); if (n > 0 && n < 200) scan(ar, n); }
    }
    if (!found && o_levelVariants >= 0) {
        void* grp = rd_ptr(om, o_levelVariants);
        if (grp) {
            int oRec = il2::field_off(il2::object_get_class(grp), "recipes");
            void* list = oRec >= 0 ? rd_ptr(grp, oRec) : nullptr;
            int vn = list ? rd_int(list, o_listSize) : 0;
            void* varr = list ? rd_ptr(list, o_listItems) : nullptr;
            if (varr && vn > 0 && vn < 400) scan(varr, vn);
        }
    }
    if (!found) return false;
    void* rk = il2::object_get_class(found);
    int oS = il2::field_off(rk, "isSandwich"), oD = il2::field_off(rk, "isDrink"), oSi = il2::field_off(rk, "isSide"), oB = il2::field_off(rk, "isBakedGood");
    int oI = il2::field_off(rk, "ingredients");
    auto flag = [&](int o) { return o >= 0 && *(static_cast<char*>(found) + o) != 0; };
    kind = flag(oS) ? 1 : flag(oD) ? 2 : flag(oSi) ? 3 : flag(oB) ? 4 : 0;
    void* list = oI >= 0 ? rd_ptr(found, oI) : nullptr;
    int n = list ? rd_int(list, o_listSize) : 0;
    void* arr = list ? rd_ptr(list, o_listItems) : nullptr;
    for (int i = 0; arr && i < n && i < 40; i++) {
        void* ing = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
        if (!ing) continue;
        int oid = il2::field_off(il2::object_get_class(ing), "ingredientID");
        void* s = oid >= 0 ? rd_ptr(ing, oid) : nullptr;
        ingIds.push_back(s ? il2::to_utf8(s) : std::string());
    }
    return true;
}
