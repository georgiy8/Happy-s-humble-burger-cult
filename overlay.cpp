#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <MinHook.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <set>

// Menu drawn with Unity's own IMGUI (UnityEngine.GUI), from a detour on
// PurrNet.ScytheNetworkStats.OnGUI - a method that the game calls every frame
// on the main thread. No D3D hook, no external UI library.

namespace {

struct Rect { float x, y, w, h; };

void* m_box = nullptr; void* m_label = nullptr; void* m_button = nullptr;
void* m_toggle = nullptr; void* m_slider = nullptr;

using FnOnGUI = void (*)(void*, void*);
FnOnGUI o_onGUI = nullptr;

int s_tab = 0;
int s_calls = 0;
int s_exceptions = 0;

// ---- thin invoke helpers (value types by pointer, strings as the object pointer) ----
void* invoke(void* m, void** args) {
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m, nullptr, args, &exc);
    if (exc) { s_exceptions++; return nullptr; }
    return r;
}

void box(Rect r, const char* text) {
    void* s = il2::string_new(text);
    void* a[] = { &r, s };
    invoke(m_box, a);
}
void label(Rect r, const char* text) {
    void* s = il2::string_new(text);
    void* a[] = { &r, s };
    invoke(m_label, a);
}
bool button(Rect r, const char* text) {
    void* s = il2::string_new(text);
    void* a[] = { &r, s };
    void* res = invoke(m_button, a);
    return res && *(static_cast<char*>(res) + 0x10) != 0;     // boxed bool
}
bool toggle(Rect r, bool v, const char* text) {
    void* s = il2::string_new(text);
    unsigned char b = v ? 1 : 0;
    void* a[] = { &r, &b, s };
    void* res = invoke(m_toggle, a);
    return res ? (*(static_cast<char*>(res) + 0x10) != 0) : v;
}
float slider(Rect r, float v, float lo, float hi) {
    void* a[] = { &r, &v, &lo, &hi };
    void* res = invoke(m_slider, a);
    return res ? *reinterpret_cast<float*>(static_cast<char*>(res) + 0x10) : v;
}

bool cook_patty_name_ui(int i, char* out, int cap) { return cook_patty_name(i, out, cap); }

// ---- one UI pass ----
void draw_ui() {
    const bool wide = (s_tab >= 3);
    const float X = 40, Y = 40, W = wide ? 900.0f : 560.0f, H = wide ? ((s_tab == 5 || s_tab == 3) ? 720.0f : 580.0f) : 430.0f;
    {
        char title[96], kn[40];
        vk_name(g.bind[0].load(), kn, sizeof(kn));
        snprintf(title, sizeof(title), "HHBC mod   [%s] - hide", kn);
        box({ X, Y, W, H }, title);
    }

    const char* tabs[] = { "Player", "Orders", "World", "Kitchen", "Debug", "Lab", "Keys" };
    for (int i = 0; i < 7; i++) {
        char t[32];
        snprintf(t, sizeof(t), i == s_tab ? "[ %s ]" : "%s", tabs[i]);
        if (button({ X + 15, Y + 40 + 42.0f * i, 130, 36 }, t)) s_tab = i;
    }

    const float cx = X + 165, cw = W - 185;
    char buf[160];
    float y = Y + 40;

    if (s_tab == 0) {
        g.infStamina = toggle({ cx, y, cw, 28 }, g.infStamina.load() != 0, "Infinite stamina"); y += 40;
        g.speedOn = toggle({ cx, y, cw, 28 }, g.speedOn.load() != 0, "Speed multiplier"); y += 34;
        snprintf(buf, sizeof(buf), "Multiplier: x%.2f", g.speedPct.load() / 100.0f);
        label({ cx, y, cw, 24 }, buf); y += 26;
        float v = slider({ cx, y + 6, cw, 20 }, static_cast<float>(g.speedPct.load()), 50.0f, 400.0f);
        g.speedPct = static_cast<int>(v + 0.5f);
        y += 40;
        g.infJump = toggle({ cx, y, cw, 28 }, g.infJump.load() != 0, "Infinite jump"); y += 34;
        g.jumpOn = toggle({ cx, y, cw, 28 }, g.jumpOn.load() != 0, "Jump height multiplier"); y += 34;
        snprintf(buf, sizeof(buf), "Jump: x%.2f", g.jumpPct.load() / 100.0f);
        label({ cx, y, cw, 24 }, buf); y += 26;
        g.jumpPct = static_cast<int>(slider({ cx, y + 6, cw, 20 }, static_cast<float>(g.jumpPct.load()), 50.0f, 500.0f) + 0.5f);
        y += 40;
        label({ cx, y, cw, 24 }, g.playerFound.load() ? "Player: found" : "Player: not found (start a game)");
    } else if (s_tab == 1) {
        g.autoAccept = toggle({ cx, y, cw, 28 }, g.autoAccept.load() != 0, "Auto-accept orders at the register"); y += 40;
        snprintf(buf, sizeof(buf), "Delay between attempts: %d ms", g.acceptDelayMs.load());
        label({ cx, y, cw, 24 }, buf); y += 26;
        float v = slider({ cx, y + 6, cw, 20 }, static_cast<float>(g.acceptDelayMs.load()), 1500.0f, 8000.0f);
        g.acceptDelayMs = static_cast<int>(v + 0.5f);
        y += 40;
        snprintf(buf, sizeof(buf), "Orders submitted: %d", g.acceptedCount.load());
        label({ cx, y, cw, 24 }, buf); y += 28;
        {
            int mode = g.acceptMode.load();
            snprintf(buf, sizeof(buf), "Last mode: %s", mode == 1 ? "HOST / solo (reads the server's order list)"
                                                         : mode == 2 ? "CLIENT (reads the order text on the register)" : "none yet");
            label({ cx, y, cw, 24 }, buf); y += 28;
        }
        label({ cx, y, cw, 90 }, "Enters the customer's order on the register and presses submit,\nthe same way you would. Two modes, chosen automatically:\n  host/solo - exact order from the server list\n  client - order text shown on the register (names must match a recipe)");
    } else if (s_tab == 2) {
        g.timeOn = toggle({ cx, y, cw, 28 }, g.timeOn.load() != 0, "Custom game speed (Time.timeScale)"); y += 34;
        snprintf(buf, sizeof(buf), "Game speed: x%.2f", g.timePct.load() / 100.0f);
        label({ cx, y, cw, 24 }, buf); y += 26;
        float v = slider({ cx, y + 6, cw, 20 }, static_cast<float>(g.timePct.load()), 10.0f, 300.0f);
        g.timePct = static_cast<int>(v + 0.5f);
        y += 40;
        if (button({ cx, y, cw, 32 }, "FULL DUMP (classes, kitchen, recipes, world) -> game folder")) g.requestDump = 1;
        y += 38;
        label({ cx, y, cw, 90 }, "Do it INSIDE a shift (recipes + stations must exist).\nWrites: hhbc_details.txt, hhbc_kitchen.txt,\nhhbc_recipes.txt, hhbc_world.txt, and logs to hhbc_mod.log.\nThe game may stutter for a second.");
    } else if (s_tab == 3) {
        g.labVisible = 1;
        label({ cx, y, cw, 22 }, "AUTO-COOK. Every click starts a list (job); lists run in parallel. Spy ON shows each step (MOD >> ...)."); y += 26;
        label({ cx, y, cw, 22 }, "BURGERS - patties (raw types found in the ingredient boxes)"); y += 24;
        {
            char nm[96];
            int n = 0;
            for (int i = 0; i < 8; i++) {
                if (!cook_patty_name_ui(i, nm, sizeof(nm))) break;
                char t[140];
                snprintf(t, sizeof(t), "Auto-cook: %s", nm);
                const float colW = (cw - 6) / 2;
                if (button({ cx + (i % 2) * (colW + 6), y + (i / 2) * 28.0f, colW, 26 }, t)) { char c[120]; snprintf(c, sizeof(c), "patty:%s", nm); cook_request(c); }
                n = i + 1;
            }
            if (!n) { label({ cx, y, cw, 24 }, "(no raw patty boxes yet - start a shift)"); n = 2; }
            y += ((n + 1) / 2) * 28.0f + 4;
        }
        {
            bool b = toggle({ cx, y, cw / 2 - 4, 26 }, g.dstBoard.load() != 0, "Done patty -> assembly board");
            bool inv = toggle({ cx + cw / 2, y, cw / 2, 26 }, g.dstInv.load() != 0, "Done patty -> inventory");
            if (b != (g.dstBoard.load() != 0)) { g.dstBoard = b ? 1 : 0; if (b) g.dstInv = 0; }
            if (inv != (g.dstInv.load() != 0)) { g.dstInv = inv ? 1 : 0; if (inv) g.dstBoard = 0; }
            y += 32;
        }
        label({ cx, y, cw, 22 }, "SIDES AND BAKED GOODS"); y += 24;
        {
            const float w3 = (cw - 12) / 3;
            if (button({ cx, y, w3, 28 }, "Cook cookies + pies")) cook_request("trays");
            if (button({ cx + w3 + 6, y, w3, 28 }, "Cook french fries")) cook_request("fries");
            if (button({ cx + 2 * (w3 + 6), y, w3, 28 }, "Cook nuggets")) cook_request("nuggets");
            y += 32;
            const float w4 = (cw - 18) / 4;
            if (button({ cx, y, w4, 28 }, "Cookie -> bag")) cook_request("bag:cookie");
            if (button({ cx + (w4 + 6), y, w4, 28 }, "Pie -> bag")) cook_request("bag:pie");
            if (button({ cx + 2 * (w4 + 6), y, w4, 28 }, "Fries -> bag")) cook_request("bag:fries");
            if (button({ cx + 3 * (w4 + 6), y, w4, 28 }, "Nuggets -> bag")) cook_request("bag:nuggets");
            y += 36;
        }
        label({ cx, y, cw, 22 }, "DRINKS (cup, ice, soda, then into the bag)"); y += 24;
        {
            static const char* kD[6] = { "Bump Cola", "Swimmers Choice", "Greap", "Jazzy Java", "Blob Aqua", "Agent Orange" };
            const float w3 = (cw - 12) / 3;
            for (int i = 0; i < 6; i++) {
                char t[60], c[80];
                snprintf(t, sizeof(t), "%s", kD[i]);
                if (button({ cx + (i % 3) * (w3 + 6), y + (i / 3) * 28.0f, w3, 26 }, t)) { snprintf(c, sizeof(c), "soda:%s", kD[i]); cook_request(c); }
            }
            y += 58;
            if (button({ cx, y, cw / 2 - 4, 28 }, "Soda of the accepted order")) cook_request("soda:order");
            if (button({ cx + cw / 2, y, cw / 2, 28 }, "CANCEL ALL JOBS")) cook_request("cancel");
            y += 36;
        }
        {
            const float w3 = (cw - 12) / 3;
            if (button({ cx, y, w3, 28 }, "Shake: Strawberry")) cook_request("shake:strawberry");
            if (button({ cx + w3 + 6, y, w3, 28 }, "Shake: Blueberry")) cook_request("shake:blueberry");
            if (button({ cx + 2 * (w3 + 6), y, w3, 28 }, "Shake of the accepted order")) cook_request("shake:order");
            y += 34;
        }
        {
            static std::string jobs;
            static int tk = 0;
            if ((tk++ % 6) == 0) {
                jobs.clear();
                char ln[240];
                for (int i = 0; i < 12 && cook_job_line(i, ln, sizeof(ln)); i++) { jobs += ln; jobs += '\n'; }
                if (jobs.empty()) jobs = "(no active lists)";
            }
            label({ cx, y, cw, H - (y - Y) - 40 }, jobs.c_str());
        }
    } else if (s_tab == 4) {
        bool on = toggle({ cx, y, cw, 28 }, g.spyOn.load() != 0, "Spy ON  (log my interactions / appliance calls)");
        if (on) g.spyInstall = 1;
        g.spyOn = on ? 1 : 0;
        y += 34;
        if (button({ cx, y, 120, 28 }, "Clear")) g.spyClear = 1;
        snprintf(buf, sizeof(buf), "hooks: %d   events: %d   (all lines also go to hhbc_mod.log)", g.spyHooks.load(), g.spyEvents.load());
        label({ cx + 130, y + 3, cw - 130, 24 }, buf);
        y += 34;
        {
            static const char* kMarks[] = { "grill", "board", "drink", "fryer", "oven", "bag" };
            for (int i = 0; i < 6; i++) {
                char t[32];
                snprintf(t, sizeof(t), "MARK: %s", kMarks[i]);
                if (button({ cx + 118.0f * i, y, 112, 26 }, t)) spy_mark(kMarks[i]);
            }
        }
        y += 34;
        static std::vector<std::string> lines;
        static std::string joined;
        static int lastVer = -1;
        int ver = spy_version();
        if (ver != lastVer) {
            lastVer = ver;
            spy_tail(lines, 22);
            joined.clear();
            for (auto& l : lines) { joined += l; joined += '\n'; }
            if (joined.empty()) joined = "(console empty - turn Spy ON, then press buttons / grab items in the game)";
        }
        label({ cx, y, cw, H - (y - Y) - 40 }, joined.c_str());
    } else if (s_tab == 5) {
        g.labVisible = 1;
        label({ cx, y, cw, 62 }, "LAB: automatic tools. Turn Spy ON (Debug tab) to see every action in the log.\nBETA CLOSEOUT does the ACTIVE after-shift duties: thermostats, rat traps, appliances off, trash, kitchen trash, grease, napkins, air filters, restock boxes, fires (experimental)."); y += 66;
        if (button({ cx, y, cw - 150, 34 }, "BETA CLOSEOUT: do the current after-shift duties")) cook_request("closeout");
        if (button({ cx + cw - 144, y, 144, 34 }, "CANCEL JOBS")) cook_request("cancel");
        y += 40;
        {
            char jl[220];
            int shown = 0;
            for (int i = 0; i < 8 && cook_job_line(i, jl, sizeof(jl)); i++) { label({ cx, y, cw, 20 }, jl); y += 20; shown++; }
            if (!shown) { label({ cx, y, cw, 20 }, "(no jobs running)"); y += 20; }
            y += 6;
        }
        static std::vector<std::string> tail;
        static std::string joined;
        static int tick = 0;
        if ((tick++ % 10) == 0) {
            log_tail(tail, 6);
            joined.clear();
            for (auto& l : tail) { size_t p = l.find("] "); joined += (p == std::string::npos ? l : l.substr(p + 2)); joined += '\n'; }
        }
        label({ cx, y, cw, H - (y - Y) - 40 }, joined.c_str());
    } else {
        label({ cx, y, cw, 40 }, "KEY BINDS: click a row, then press the key you want (keyboard or extra mouse button).\nEsc while waiting = clear the bind (menu key goes back to Insert). Saved to hhbc_binds.ini."); y += 46;
        const int listen = g.bindListen.load();
        for (int i = 0; i < State::kBindCount; i++) {
            label({ cx, y + 3, cw - 200, 24 }, bind_action_name(i));
            char t[64], nm[40];
            if (listen == i) snprintf(t, sizeof(t), "press a key...  (Esc = clear)");
            else {
                int vk = g.bind[i].load();
                if (vk > 0) { vk_name(vk, nm, sizeof(nm)); snprintf(t, sizeof(t), "[ %s ]", nm); }
                else snprintf(t, sizeof(t), "none");
            }
            if (button({ cx + cw - 190, y, 190, 26 }, t)) g.bindListen = (listen == i) ? -1 : i;
            y += 30;
        }
    }

    snprintf(buf, sizeof(buf), "ticks: order=%d look=%d | OnGUI calls=%d exc=%d",
             g.tickOrder.load(), g.tickLook.load(), s_calls, s_exceptions);
    label({ X + 15, Y + H - 28, W - 30, 22 }, buf);
}

// =============================== BOOK UI ===============================
// Paper-book menu: the page picture and the button sheet are embedded in the DLL as resources (hhbc_ui.rc),
// turned into Unity textures with ImageConversion.LoadImage and drawn with GUI.DrawTextureWithTexCoords.
// Everything is laid out in the book's own 1536x1024 pixels and scaled to the window with GUI.matrix.
// If anything fails (resource, module, method) the classic grey menu is used instead.
struct Tex { void* obj = nullptr; int w = 0, h = 0; };
enum { T_BOOK, T_NL, T_NM, T_NR, T_HL, T_HM, T_HR, T_PL, T_PM, T_PR, T_SBEIGE, T_SRCAP, T_SRMID, T_SKNOB, T_TAB, T_TLOBBY, T_TDRIVE, T_TPICKUP, T_COUNT };
Tex s_t[T_COUNT];
int s_drawKind = 0;
void* m_draw = nullptr;
void* m_guiColor = nullptr;
bool s_bookTried = false, s_bookOk = false;
void *m_setMatrix = nullptr, *m_setColor = nullptr;
void *m_getSkin = nullptr, *m_skinLabel = nullptr, *m_setFont = nullptr, *m_setAlign = nullptr;
void *m_styleNone = nullptr, *m_evCur = nullptr, *m_evMouse = nullptr, *m_scrW = nullptr, *m_scrH = nullptr, *m_texW = nullptr;
void* s_labelStyle = nullptr;
float s_mx = -1, s_my = -1;
int s_btab = 3;

void* invoke_on(void* m, void* obj, void** args) {
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m, obj, args, &exc);
    if (exc) { s_exceptions++; return nullptr; }
    return r;
}

bool load_resource(int id, std::vector<unsigned char>& out) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&load_resource), &self) || !self) return false;
    HRSRC r = FindResourceA(self, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10));
    if (!r) return false;
    HGLOBAL h = LoadResource(self, r);
    DWORD n = SizeofResource(self, r);
    const void* p = h ? LockResource(h) : nullptr;
    if (!p || !n) return false;
    out.assign(static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + n);
    return true;
}

bool make_tex(int resId, Tex& t) {
    std::vector<unsigned char> png;
    if (!load_resource(resId, png)) { logf("book: resource %d not found in the DLL", resId); return false; }
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* ic = il2::find_image("UnityEngine.ImageConversionModule.dll");
    void* cor = il2::find_image("mscorlib.dll");
    void* kTex2D = il2::find_class(core, "UnityEngine", "Texture2D");
    void* kTex = il2::find_class(core, "UnityEngine", "Texture");
    void* kObj = il2::find_class(core, "UnityEngine", "Object");
    void* kIC = il2::find_class(ic, "UnityEngine", "ImageConversion");
    void* kByte = il2::find_class(cor, "System", "Byte");
    if (!kTex2D || !kTex || !kIC || !kByte || !il2::array_new) { logf("book: Texture2D / ImageConversion / Byte / il2cpp_array_new missing"); return false; }
    const char* lp[] = { "UnityEngine.Texture2D", "System.Byte[]" };
    void* mLoad = il2::find_method_sig(kIC, "LoadImage", lp, 2);
    void* mCtor = il2::find_method(kTex2D, ".ctor", 2);
    if (!mLoad || !mCtor) return false;
    void* arr = il2::array_new(kByte, png.size());
    if (!arr) return false;
    memcpy(static_cast<char*>(arr) + 0x20, png.data(), png.size());
    void* obj = il2::object_new(kTex2D);
    int two = 2;
    void* ca[] = { &two, &two };
    if (!obj) return false;
    void* exc = nullptr;
    il2::runtime_invoke(mCtor, obj, ca, &exc);
    if (exc) { logf("book: Texture2D ctor threw"); return false; }
    void* la[] = { obj, arr };
    void* ok = invoke_on(mLoad, nullptr, la);
    if (!ok || !*(static_cast<char*>(ok) + 0x10)) { logf("book: LoadImage failed"); return false; }
    if (il2::gchandle_new) il2::gchandle_new(obj, 1);
    if (kObj) { void* mh = il2::find_method(kObj, "set_hideFlags", 1); int hf = 61; void* ha[] = { &hf }; if (mh) invoke_on(mh, obj, ha); }
    { void* mw = il2::find_method(kTex, "set_wrapMode", 1); int wm = 1; void* wa[] = { &wm }; if (mw) invoke_on(mw, obj, wa); }
    m_texW = il2::find_method(kTex, "get_width", 0);
    void* mh2 = il2::find_method(kTex, "get_height", 0);
    void* w = m_texW ? invoke_on(m_texW, obj, nullptr) : nullptr;
    void* h = mh2 ? invoke_on(mh2, obj, nullptr) : nullptr;
    t.obj = obj;
    t.w = w ? *reinterpret_cast<int*>(static_cast<char*>(w) + 0x10) : 0;
    t.h = h ? *reinterpret_cast<int*>(static_cast<char*>(h) + 0x10) : 0;
    logf("book: texture %d loaded %dx%d", resId, t.w, t.h);
    return t.w > 0 && t.h > 0;
}

void log_gui_methods(void* klass, const char* cname) {
    void* it = nullptr;
    int n = 0;
    while (void* m = il2::class_get_methods(klass, &it)) {
        const char* nm = il2::method_get_name(m);
        if (!nm) continue;
        std::string line = std::string(cname) + "." + nm + "(";
        int pc = static_cast<int>(il2::method_get_param_count(m));
        for (int i = 0; i < pc; i++) {
            char* tn = il2::type_get_name(il2::method_get_param(m, static_cast<uint32_t>(i)));
            if (i) line += ", ";
            line += tn ? tn : "?";
            if (tn && il2::il2_free) il2::il2_free(tn);
        }
        line += ")";
        logf("book-diag: %s", line.c_str());
        if (++n > 400) break;
    }
}

bool book_init() {
    s_bookTried = true;
    void* guiImg = il2::find_image("UnityEngine.IMGUIModule.dll");
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* kGUI = il2::find_class(guiImg, "UnityEngine", "GUI");
    void* kSkin = il2::find_class(guiImg, "UnityEngine", "GUISkin");
    void* kStyle = il2::find_class(guiImg, "UnityEngine", "GUIStyle");
    void* kEvent = il2::find_class(guiImg, "UnityEngine", "Event");
    void* kScreen = il2::find_class(core, "UnityEngine", "Screen");
    if (!kGUI || !kSkin || !kStyle || !kEvent || !kScreen) { logf("book: IMGUI classes missing"); return false; }
    // The game strips unused engine methods, so only some overloads exist: take the first one that does.
    {
        const char* d2[] = { "UnityEngine.Rect", "UnityEngine.Texture" };
        const char* d3[] = { "UnityEngine.Rect", "UnityEngine.Texture", "UnityEngine.ScaleMode" };
        const char* d4[] = { "UnityEngine.Rect", "UnityEngine.Texture", "UnityEngine.ScaleMode", "System.Boolean" };
        if ((m_draw = il2::find_method_sig(kGUI, "DrawTexture", d2, 2))) s_drawKind = 2;
        else if ((m_draw = il2::find_method_sig(kGUI, "DrawTexture", d3, 3))) s_drawKind = 3;
        else if ((m_draw = il2::find_method_sig(kGUI, "DrawTexture", d4, 4))) s_drawKind = 4;
    }
    m_setMatrix = il2::find_method(kGUI, "set_matrix", 1);
    m_setColor = il2::find_method(kGUI, "set_contentColor", 1);
    m_guiColor = il2::find_method(kGUI, "set_color", 1);
    if (!m_guiColor) m_guiColor = il2::find_method(kGUI, "set_backgroundColor", 1);
    m_getSkin = il2::find_method(kGUI, "get_skin", 0);
    m_skinLabel = il2::find_method(kSkin, "get_label", 0);
    m_setFont = il2::find_method(kStyle, "set_fontSize", 1);
    m_setAlign = il2::find_method(kStyle, "set_alignment", 1);
    m_evCur = il2::find_method(kEvent, "get_current", 0);
    m_evMouse = il2::find_method(kEvent, "get_mousePosition", 0);
    m_scrW = il2::find_method(kScreen, "get_width", 0);
    m_scrH = il2::find_method(kScreen, "get_height", 0);
    if (!s_drawKind || !m_guiColor || !m_setMatrix || !m_setColor || !m_getSkin || !m_skinLabel || !m_setFont || !m_setAlign ||
        !m_evCur || !m_evMouse || !m_scrW || !m_scrH) { logf("book: some IMGUI methods are missing (draw kind %d, guiColor %p) - classic menu", s_drawKind, m_guiColor); log_gui_methods(kGUI, "GUI"); return false; }
    for (int i = 0; i < T_COUNT; i++) if (!make_tex(101 + i, s_t[i])) return false;
    logf("book: menu textures ready");
    return true;
}

bool book_ready() {
    static int check = 0;
    if (!s_bookTried) s_bookOk = book_init();
    else if (s_bookOk && (++check % 240) == 0) {                // textures may be destroyed by a scene change: rebuild then
        void* exc = nullptr;
        void* r = (m_texW && s_t[T_BOOK].obj) ? il2::runtime_invoke(m_texW, s_t[T_BOOK].obj, nullptr, &exc) : nullptr;
        if (exc || !r) { logf("book: texture lost - reloading"); s_bookOk = book_init(); }
    }
    return s_bookOk;
}

int s_fs = -1, s_al = -1;
float s_col[4] = { -1, 0, 0, 0 };
void set_style(int fs, int al) {
    if (!s_labelStyle) return;
    if (fs != s_fs) { void* a[] = { &fs }; invoke_on(m_setFont, s_labelStyle, a); s_fs = fs; }
    if (al != s_al) { void* a[] = { &al }; invoke_on(m_setAlign, s_labelStyle, a); s_al = al; }
}
void set_col(float r, float g_, float b) {
    if (s_col[0] == r && s_col[1] == g_ && s_col[2] == b) return;
    float c[4] = { r, g_, b, 1.0f };
    void* a[] = { c };
    invoke_on(m_setColor, nullptr, a);
    s_col[0] = r; s_col[1] = g_; s_col[2] = b;
}

void draw_piece(Rect dst, int idx) {
    if (dst.w <= 0.5f || dst.h <= 0.5f) return;
    const Tex& t = s_t[idx];
    int sm = 0;                                                // ScaleMode.StretchToFill
    unsigned char ab = 1;
    if (s_drawKind == 2) { void* a[] = { &dst, t.obj }; invoke_on(m_draw, nullptr, a); }
    else if (s_drawKind == 3) { void* a[] = { &dst, t.obj, &sm }; invoke_on(m_draw, nullptr, a); }
    else { void* a[] = { &dst, t.obj, &sm, &ab }; invoke_on(m_draw, nullptr, a); }
}
bool inside(Rect r) { return s_mx >= r.x && s_mx <= r.x + r.w && s_my >= r.y && s_my <= r.y + r.h; }
void gui_tint(float r, float g_, float b, float a) { float c4[4] = { r, g_, b, a }; void* args[] = { c4 }; invoke_on(m_guiColor, nullptr, args); }
void gui_alpha(float a) { gui_tint(1, 1, 1, a); }

void draw_btn(Rect r, int st) {                              // 0 normal, 1 hover, 2 dark (on / selected)
    const int base = st == 1 ? T_HL : (st == 2 ? T_PL : T_NL);
    const float k = r.h / 182.0f, dl = 60.0f * k, dr = 120.0f * k;
    draw_piece({ r.x, r.y, dl, r.h }, base);
    draw_piece({ r.x + dl, r.y, r.w - dl - dr, r.h }, base + 1);
    draw_piece({ r.x + r.w - dr, r.y, dr, r.h }, base + 2);
}
// the real GUI.Button / slider are drawn fully transparent (GUI.color alpha 0): they only catch the mouse
bool click_area(Rect r) {
    gui_alpha(0.0f);
    bool res = button(r, "");
    gui_alpha(1.0f);
    return res;
}
void blabel(Rect r, const char* text, int fs, int al, float cr, float cg, float cb) {
    set_style(fs, al); set_col(cr, cg, cb);
    label(r, text);
}
void ink(Rect r, const char* text, int fs = 16) { blabel(r, text, fs, 0, 0.13f, 0.12f, 0.32f); }          // upper-left, blue ink
bool bbtn(Rect r, const char* text, int forced = -1) {
    draw_btn(r, forced >= 0 ? forced : (inside(r) ? 1 : 0));
    blabel({ r.x + 10, r.y, r.w - 52, r.h }, text, 15, 3, 1.0f, 0.96f, 0.9f);                                 // middle-left, white
    return click_area(r);
}
bool btgl(Rect r, const char* text, bool v) {
    draw_btn(r, v ? 2 : (inside(r) ? 1 : 0));
    char t[200]; snprintf(t, sizeof(t), v ? "[X]  %s" : "[   ]  %s", text);
    blabel({ r.x + 10, r.y, r.w - 52, r.h }, t, 15, 3, 1.0f, 0.96f, 0.9f);
    return click_area(r) ? !v : v;
}
float bslider(Rect r, float v, float lo, float hi) {
    gui_alpha(0.0f);
    v = slider(r, v, lo, hi);
    gui_alpha(1.0f);
    float f = (v - lo) / (hi - lo); f = f < 0 ? 0 : (f > 1 ? 1 : f);
    const float k = r.h / 130.0f, th = 84.0f * k, cy = r.y + r.h * 0.5f, capW = 32.0f * k;
    draw_piece({ r.x, cy - th * 0.5f, r.w, th }, T_SBEIGE);
    draw_piece({ r.x, cy - th * 0.5f, capW, th }, T_SRCAP);
    float redW = f * r.w - capW; if (redW > 0) draw_piece({ r.x + capW, cy - th * 0.5f, redW, th }, T_SRMID);
    const float kw = 115.0f * k;
    draw_piece({ r.x + f * r.w - kw * 0.5f, r.y, kw, r.h }, T_SKNOB);
    return v;
}

// =============================== VISUAL (fullbright + ESP) ===============================
// Everything is looked up by name; if a method is stripped from the game the feature just stays off (see the log).
// ESP draws small text labels with IMGUI at the screen position of each object (Camera.WorldToScreenPoint).
struct EspClass { const char* name; bool enemy; void* klass; int deadOff; bool deadSync; bool resolved; };
EspClass s_esp[] = {
    { "CorruptedChef", true }, { "Cultist", true }, { "ViolentCultist", true }, { "Mechanoid", true },
    { "KhyvinDemonAI", true }, { "JoyAsset", true }, { "HappyAssetAI", true }, { "NullPhantom", true }, { "ParagonCleaner", true },
    { "RatTrap", false }, { "GreaseBucket", false }, { "RestockBox", false }, { "CloseoutTrashBag", false },
    { "KitchenTrashItem", false }, { "NapkinBox", false }, { "AirFilterItem", false }, { "FireExtinguisher", false },
};
struct EspTarget { void* obj; int cls; };
std::vector<EspTarget> s_espList;
ULONGLONG s_espScan = 0, s_fbT = 0;
bool s_visTried = false, s_visOk = false;
void *m_camMain = nullptr, *m_w2s = nullptr, *m_cGetTr = nullptr, *m_trGetPos = nullptr, *m_objFind = nullptr, *m_evType = nullptr;
void *m_rsSetMode = nullptr, *m_rsGetMode = nullptr, *m_rsSetLight = nullptr, *m_rsGetLight = nullptr, *m_rsSetFog = nullptr, *m_rsGetFog = nullptr;
bool s_fbActive = false; int s_fbMode = 0; float s_fbLight[4] = { 0.5f, 0.5f, 0.5f, 1 }; bool s_fbFog = false;

bool vis_init() {
    s_visTried = true;
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* guiImg = il2::find_image("UnityEngine.IMGUIModule.dll");
    void* kCam = il2::find_class(core, "UnityEngine", "Camera");
    void* kComp = il2::find_class(core, "UnityEngine", "Component");
    void* kTr = il2::find_class(core, "UnityEngine", "Transform");
    void* kObj = il2::find_class(core, "UnityEngine", "Object");
    void* kRS = il2::find_class(core, "UnityEngine", "RenderSettings");
    void* kEvent = il2::find_class(guiImg, "UnityEngine", "Event");
    if (kCam) { m_camMain = il2::find_method(kCam, "get_main", 0); m_w2s = il2::find_method(kCam, "WorldToScreenPoint", 1); }
    m_cGetTr = il2::find_method(kComp, "get_transform", 0);
    m_trGetPos = il2::find_method(kTr, "get_position", 0);
    const char* tp[] = { "System.Type" };
    m_objFind = il2::find_method_sig(kObj, "FindObjectsOfType", tp, 1);
    m_evType = il2::find_method(kEvent, "get_type", 0);
    if (kRS) {
        m_rsSetMode = il2::find_method(kRS, "set_ambientMode", 1); m_rsGetMode = il2::find_method(kRS, "get_ambientMode", 0);
        m_rsSetLight = il2::find_method(kRS, "set_ambientLight", 1); m_rsGetLight = il2::find_method(kRS, "get_ambientLight", 0);
        m_rsSetFog = il2::find_method(kRS, "set_fog", 1); m_rsGetFog = il2::find_method(kRS, "get_fog", 0);
    }
    logf("visual: Camera.main %s, WorldToScreenPoint %s, FindObjectsOfType %s, Event.type %s, RenderSettings ambient %s / fog %s",
         m_camMain ? "ok" : "MISSING", m_w2s ? "ok" : "MISSING", m_objFind ? "ok" : "MISSING", m_evType ? "ok" : "MISSING",
         (m_rsSetMode && m_rsSetLight) ? "ok" : "MISSING", m_rsSetFog ? "ok" : "MISSING");
    void* cs = il2::find_image("Assembly-CSharp.dll");
    for (auto& e : s_esp) {
        e.klass = il2::find_class(cs, "", e.name);
        e.deadOff = -1; e.deadSync = false;
        if (e.klass && e.enemy) {
            void* f = il2::find_field(e.klass, "dead");
            if (f) {
                e.deadOff = static_cast<int>(il2::field_get_offset(f));
                char* tn = il2::type_get_name(il2::field_get_type(f));
                e.deadSync = tn && strstr(tn, "SyncVar");
                if (tn && il2::il2_free) il2::il2_free(tn);
            }
        }
        if (!e.klass) logf("visual: class %s not found (skipped)", e.name);
    }
    return m_camMain && m_w2s && m_cGetTr && m_trGetPos && m_objFind && m_evType;
}

bool vis_pos(void* comp, float out[3]) {
    void* tr = invoke_on(m_cGetTr, comp, nullptr);
    if (!tr) return false;
    void* b = invoke_on(m_trGetPos, tr, nullptr);
    if (!b) return false;
    const float* v = reinterpret_cast<const float*>(static_cast<char*>(b) + 0x10);
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return true;
}

bool vis_dead(const EspClass& e, void* o) {
    if (e.deadOff < 0) return false;
    char* p = static_cast<char*>(o) + e.deadOff;
    if (e.deadSync) {
        void* sv = *reinterpret_cast<void**>(p);
        return sv && *(static_cast<char*>(sv) + 0x30) != 0;
    }
    return *p != 0;
}

void fullbright_apply(bool want) {
    if (!m_rsSetMode || !m_rsSetLight) return;
    if (want) {
        if (!s_fbActive) {                                             // remember the game's own values once
            if (m_rsGetMode) { void* r = invoke_on(m_rsGetMode, nullptr, nullptr); if (r) s_fbMode = *reinterpret_cast<int*>(static_cast<char*>(r) + 0x10); }
            if (m_rsGetLight) { void* r = invoke_on(m_rsGetLight, nullptr, nullptr); if (r) memcpy(s_fbLight, static_cast<char*>(r) + 0x10, 16); }
            if (m_rsGetFog) { void* r = invoke_on(m_rsGetFog, nullptr, nullptr); if (r) s_fbFog = *(static_cast<char*>(r) + 0x10) != 0; }
            s_fbActive = true;
            logf("visual: fullbright ON (saved ambient mode %d)", s_fbMode);
        }
        int mode = 3; float white[4] = { 1, 1, 1, 1 }; unsigned char fog = 0;
        { void* a[] = { &mode }; invoke_on(m_rsSetMode, nullptr, a); }
        { void* a[] = { white }; invoke_on(m_rsSetLight, nullptr, a); }
        if (m_rsSetFog) { void* a[] = { &fog }; invoke_on(m_rsSetFog, nullptr, a); }
    } else if (s_fbActive) {
        { int mode = s_fbMode; void* a[] = { &mode }; invoke_on(m_rsSetMode, nullptr, a); }
        { void* a[] = { s_fbLight }; invoke_on(m_rsSetLight, nullptr, a); }
        if (m_rsSetFog) { unsigned char fog = s_fbFog ? 1 : 0; void* a[] = { &fog }; invoke_on(m_rsSetFog, nullptr, a); }
        s_fbActive = false;
        logf("visual: fullbright OFF (lighting restored)");
    }
}

void visual_frame() {
    const bool fb = g.fullbright.load() != 0, en = g.espEnemies.load() != 0, ob = g.espObjects.load() != 0;
    if (!fb && !en && !ob && !s_fbActive) return;
    if (!s_visTried) s_visOk = vis_init();
    if (!s_visOk) return;
    const ULONGLONG now = GetTickCount64();
    if (fb != s_fbActive || (fb && now - s_fbT > 300)) { s_fbT = now; fullbright_apply(fb); }
    if (!en && !ob) return;

    void* ev = invoke_on(m_evCur, nullptr, nullptr);
    void* et = ev ? invoke_on(m_evType, ev, nullptr) : nullptr;
    if (!et || *reinterpret_cast<int*>(static_cast<char*>(et) + 0x10) != 7) return;      // EventType.Repaint only

    void* cam = invoke_on(m_camMain, nullptr, nullptr);
    if (!cam) return;
    if (now - s_espScan > 1000) {                                      // the object list is rebuilt once a second
        s_espScan = now; s_espList.clear();
        for (int c = 0; c < static_cast<int>(sizeof(s_esp) / sizeof(s_esp[0])); c++) {
            const EspClass& e = s_esp[c];
            if (!e.klass || (e.enemy ? !en : !ob)) continue;
            void* typeObj = il2::type_get_object(il2::class_get_type(e.klass));
            void* args[] = { typeObj };
            void* arr = invoke_on(m_objFind, nullptr, args);
            if (!arr) continue;
            int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18));
            for (int i = 0; i < n && i < 128 && s_espList.size() < 400; i++) {
                void* o = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
                if (o) s_espList.push_back({ o, c });
            }
        }
    }
    if (s_espList.empty() || !m_getSkin || !m_skinLabel) return;

    void* sh = invoke_on(m_scrH, nullptr, nullptr);
    const float H = sh ? static_cast<float>(*reinterpret_cast<int*>(static_cast<char*>(sh) + 0x10)) : 1080.0f;
    float cp[3] = { 0, 0, 0 };
    if (!vis_pos(cam, cp)) return;
    s_labelStyle = invoke_on(m_skinLabel, invoke_on(m_getSkin, nullptr, nullptr), nullptr);
    if (!s_labelStyle) return;
    s_fs = s_al = -1; s_col[0] = -1;
    for (const EspTarget& t : s_espList) {
        const EspClass& e = s_esp[t.cls];
        if (!t.obj || vis_dead(e, t.obj)) continue;
        float p[3];
        if (!vis_pos(t.obj, p)) continue;
        float v[3] = { p[0], p[1], p[2] };
        void* a[] = { v };
        void* sp = invoke_on(m_w2s, cam, a);
        if (!sp) continue;
        const float* s = reinterpret_cast<const float*>(static_cast<char*>(sp) + 0x10);
        if (s[2] <= 0.0f) continue;                                    // behind the camera
        const float dx = p[0] - cp[0], dy = p[1] - cp[1], dz = p[2] - cp[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s  %dm", e.name, static_cast<int>(d));
        if (e.enemy) { set_style(15, 4); set_col(1.0f, 0.25f, 0.2f); }
        else { set_style(13, 4); set_col(1.0f, 0.9f, 0.3f); }
        label({ s[0] - 110.0f, H - s[1] - 11.0f, 220.0f, 22.0f }, txt);
    }
    set_style(0, 0); set_col(1.0f, 1.0f, 1.0f);
}

void guarded_visual() {
    __try { visual_frame(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s_exceptions++; }
}

struct Pg { float x, w; };
const Pg kL = { 62, 665 }, kR = { 815, 665 };
const float kTop = 165;
// Bookmarks sit like in a real book: full width, partly overlapping, the one under the mouse slides out on top.
// kBookTabs is indexed by PAGE id (the s_btab branches in draw_book). kTabOrder is the left-to-right order of the bookmarks:
// easy things first, "Debug / Keys" always LAST. To add a page: add its name to kBookTabs, put its id into kTabOrder before the last one.
const char* kBookTabs[] = { "Player", "Orders", "World", "Kitchen", "Lab", "Visual", "Debug / Keys" };
const int kTabOrder[] = { 0, 1, 5, 2, 4, 3, 6 };
const int kTabN = static_cast<int>(sizeof(kTabOrder) / sizeof(kTabOrder[0]));
// ---- accepted orders hung as tickets (right page of the Orders bookmark); click = ALPHA cooks that order ----
int s_ordPage = 0;
void draw_tickets(float rx, float rw) {
    static std::vector<AcceptedOrder> acc;
    static int tk = 0;
    if ((tk++ % 10) == 0) { acc.clear(); features_accepted_orders(acc); }
    const int n = static_cast<int>(acc.size());
    const int pages = n > 0 ? (n + 3) / 4 : 1;
    if (s_ordPage >= pages) s_ordPage = pages - 1;
    if (s_ordPage < 0) s_ordPage = 0;
    if (n == 0) {
        ink({ rx, kTop, rw, 160 }, "No accepted orders right now.\nAccept them at the register (or switch on Auto-accept):\nthey hang here as tickets. Click a ticket and ALPHA\ncooks exactly that order.", 17);
        return;
    }
    const float cs = 290.0f, k = cs / 512.0f;
    static const char* const kType[] = { "LOBBY", "DRIVE-THRU", "PICKUP", "OTHER", "-", "EVIL" };
    for (int slot = 0; slot < 4; slot++) {
        const int i = s_ordPage * 4 + slot;
        if (i >= n) break;
        const AcceptedOrder& a = acc[i];
        const float cx = rx + 20 + (slot % 2) * 325.0f;
        float cy = kTop + (slot / 2) * 295.0f;
        const bool busy = cook_order_busy(a.key);
        const Rect body = { cx + 22 * k * 1.0f, cy + 56 * k, 440 * k, cs - 56 * k };
        const bool hov = !busy && inside(body);
        if (hov) cy -= 6;
        const int tex = a.type == 1 ? T_TDRIVE : (a.type == 2 ? T_TPICKUP : T_TLOBBY);
        if (busy) gui_tint(0.55f, 0.55f, 0.58f, 1.0f);              // being cooked: the whole ticket is darkened
        else if (a.type == 5) gui_tint(0.55f, 0.35f, 0.35f, 1.0f);  // evil orders: darker
        draw_piece({ cx, cy, cs, cs }, tex);
        if (busy || a.type == 5) gui_alpha(1.0f);
        char hd[80];
        snprintf(hd, sizeof(hd), "#%d  %s", a.id, a.type >= 0 && a.type < 6 ? kType[a.type] : "?");
        blabel({ cx + 70 * k, cy + 130 * k, 375 * k, 50 * k }, hd, 15, 3, 1.0f, 0.96f, 0.9f);
        {   // text on ALL 7 ruled lines; long names wrap to the next line, a smaller font is used when the order is big
            std::vector<std::string> lines;
            int fs = 14;
            for (int pass = 0; pass < 2; pass++) {
                lines.clear();
                const size_t maxc = pass == 0 ? 30 : 36;
                fs = pass == 0 ? 14 : 12;
                for (const std::string& it : a.items) {
                    std::string t = it;
                    bool first = true;
                    while (!t.empty()) {
                        const size_t lim = first ? maxc : maxc - 2;
                        if (t.size() <= lim) { lines.push_back((first ? "" : "  ") + t); break; }
                        size_t cut = t.rfind(' ', lim);
                        if (cut == std::string::npos || cut < lim / 2) cut = lim;
                        lines.push_back((first ? "" : "  ") + t.substr(0, cut));
                        t = t.substr(cut);
                        while (!t.empty() && t[0] == ' ') t.erase(0, 1);
                        first = false;
                    }
                }
                if (lines.size() <= 7) break;
            }
            if (lines.size() > 7) { const size_t more = lines.size() - 6; lines.resize(6); lines.push_back("+ " + std::to_string(more) + " more lines"); }
            static std::set<int> s_logged;
            if (s_logged.insert(a.id).second) { std::string all; for (auto& x : a.items) all += x + " | "; logf("ticket #%d type %d: %d item(s): %s", a.id, a.type, static_cast<int>(a.items.size()), all.c_str()); }
            for (size_t li = 0; li < lines.size(); li++) {
                const float ly = cy + (222 + 46 * static_cast<int>(li) - 38) * k;
                blabel({ cx + 74 * k, ly, 372 * k, 40 * k }, lines[li].c_str(), fs, 3, 0.09f, 0.08f, 0.07f);
            }
        }
        if (busy) blabel({ cx, cy + 150, cs, 50 }, "COOKING...", 22, 4, 1.0f, 0.93f, 0.8f);
        else if (click_area(body)) {
            char cmd[40]; snprintf(cmd, sizeof(cmd), "alpha:%d", a.id);
            cook_request(cmd);
        }
    }
    if (pages > 1) {
        char pg[60]; snprintf(pg, sizeof(pg), "page %d / %d", s_ordPage + 1, pages);
        if (bbtn({ rx + 20, kTop + 600, 170, 40 }, "< previous")) s_ordPage = (s_ordPage + pages - 1) % pages;
        blabel({ rx + 200, kTop + 600, 250, 40 }, pg, 16, 4, 0.13f, 0.12f, 0.32f);
        if (bbtn({ rx + 460, kTop + 600, 170, 40 }, "next >")) s_ordPage = (s_ordPage + 1) % pages;
    }
    blabel({ rx + 20, kTop + 650, rw - 20, 30 }, "Click a ticket: ALPHA cooks that order.", 15, 0, 0.13f, 0.12f, 0.32f);
}

float s_tabX[16], s_tabStep[16], s_tabW = 216.0f;       // by slot; step = visible width when the next one lies on top
int s_hovSlot = -1;
void layout_tabs() {
    const float lx0 = 62, lx1 = 722, rx0 = 812, rx1 = 1486;
    const int nL = (kTabN + 1) / 2, nR = kTabN - nL;
    const float w = 216.0f; s_tabW = w;
    for (int i = 0; i < kTabN; i++) {
        const bool left = i < nL; const int n = left ? nL : nR, k = left ? i : i - nL;
        const float x0 = left ? lx0 : rx0, x1 = left ? lx1 : rx1;
        float step = n > 1 ? (x1 - x0 - w) / (n - 1) : w; if (step > w + 6.0f) step = w + 6.0f;
        s_tabX[i] = x0 + k * step;
        s_tabStep[i] = (k == n - 1) ? w : (step < w ? step : w);
    }
}

void draw_book() {
    if (!s_bookOk) return;
    // window -> book scale
    void* sw = invoke_on(m_scrW, nullptr, nullptr);
    void* sh = invoke_on(m_scrH, nullptr, nullptr);
    float W = sw ? static_cast<float>(*reinterpret_cast<int*>(static_cast<char*>(sw) + 0x10)) : 1536.0f;
    float H = sh ? static_cast<float>(*reinterpret_cast<int*>(static_cast<char*>(sh) + 0x10)) : 1024.0f;
    float sc = (W - 16) / 1536.0f, sy = (H - 16) / 1024.0f;
    if (sy < sc) sc = sy;
    if (sc > 1.3f) sc = 1.3f;
    if (sc < 0.3f) sc = 0.3f;
    float m[16] = { sc, 0, 0, 0,  0, sc, 0, 0,  0, 0, 1, 0,  (W - 1536.0f * sc) * 0.5f, (H - 1024.0f * sc) * 0.5f, 0, 1 };
    { void* a[] = { m }; invoke_on(m_setMatrix, nullptr, a); }
    s_labelStyle = invoke_on(m_skinLabel, invoke_on(m_getSkin, nullptr, nullptr), nullptr);
    s_fs = s_al = -1; s_col[0] = -1;
    if (!s_labelStyle) { float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }; void* a[] = { id }; invoke_on(m_setMatrix, nullptr, a); return; }
    {
        void* ev = invoke_on(m_evCur, nullptr, nullptr);
        void* mp = ev ? invoke_on(m_evMouse, ev, nullptr) : nullptr;
        if (mp) { s_mx = *reinterpret_cast<float*>(static_cast<char*>(mp) + 0x10); s_my = *reinterpret_cast<float*>(static_cast<char*>(mp) + 0x14); }
    }

    // ---- bookmarks: they sit BEHIND the page, only the upper part sticks out above the page edge (y = 63) ----
    layout_tabs();
    const float tabW = s_tabW, tabH = 31.0f;
    auto slotTy = [&](int i) { return kTabOrder[i] == s_btab ? 33.0f : (i == s_hovSlot ? 38.0f : 44.0f); };
    {   // which slot is under the mouse: keep the popped one while the mouse is on it, else take the strip that is visible there
        int hov = -1;
        if (s_hovSlot >= 0 && s_hovSlot < kTabN && inside({ s_tabX[s_hovSlot], slotTy(s_hovSlot), tabW, 64.0f - slotTy(s_hovSlot) })) hov = s_hovSlot;
        else for (int i = kTabN - 1; i >= 0; i--) if (inside({ s_tabX[i], 38.0f, s_tabStep[i], 26.0f })) { hov = i; break; }
        s_hovSlot = hov;
    }
    int selSlot = 0; for (int i = 0; i < kTabN; i++) if (kTabOrder[i] == s_btab) selSlot = i;
    // z-order: left to right (a tab lies over the one on its left), only the hovered tab slides out on top of everything.
    // The selected tab is just raised: it does NOT cover its right neighbour, so the neighbour's text stays visible.
    int order[16], on = 0;
    for (int i = 0; i < kTabN; i++) if (i != s_hovSlot) order[on++] = i;
    if (s_hovSlot >= 0) order[on++] = s_hovSlot;
    for (int q = 0; q < on; q++) {
        const int i = order[q];
        const float c = (i == selSlot) ? 1.0f : (i == s_hovSlot ? 0.93f : 0.72f);
        gui_tint(c, c, c, 1.0f);
        draw_piece({ s_tabX[i], slotTy(i), tabW, tabH }, T_TAB);
    }
    gui_tint(1.0f, 1.0f, 1.0f, 1.0f);
    draw_piece({ 0, 0, 1536, 1024 }, T_BOOK);
    int clicked = -1;
    for (int i = 0; i < kTabN; i++) {
        const bool sel = (i == selSlot);
        const float ty = slotTy(i);
        // only the hovered tab hides the labels of the tabs that lie under it (to its right)
        if (s_hovSlot >= 0 && i > s_hovSlot && s_tabX[i] < s_tabX[s_hovSlot] + tabW) continue;
        const float lw0 = (i == s_hovSlot) ? tabW : s_tabStep[i];
        const Rect vis = { s_tabX[i], ty, lw0, 64.0f - ty };
        const char* nm = kBookTabs[kTabOrder[i]];
        if (sel) blabel({ vis.x, vis.y + 1, vis.w, vis.h - 2 }, nm, lw0 < 150.0f ? 16 : 18, 4, 1.0f, 0.88f, 0.4f);
        else blabel({ vis.x, vis.y, vis.w, vis.h - 1 }, nm, lw0 < 150.0f ? 12 : 14, 4, 1.0f, 0.93f, 0.88f);
        if (click_area(vis)) clicked = i;
    }
    if (clicked >= 0) s_btab = kTabOrder[clicked];
    // ---- titles on the red bands ----
    {
        char title[120], kn[40], st[160];
        vk_name(g.bind[0].load(), kn, sizeof(kn));
        snprintf(title, sizeof(title), "HHBC mod  -  %s        [%s] hide", kBookTabs[s_btab], kn);
        blabel({ 60, 84, 560, 48 }, title, 20, 3, 1.0f, 0.95f, 0.88f);
        static int jobsN = 0, tk0 = 0;
        if ((tk0++ % 10) == 0) { char ln[240]; int n = 0; while (n < 12 && cook_job_line(n, ln, sizeof(ln))) n++; jobsN = n; }
        snprintf(st, sizeof(st), "Spy %s    jobs: %d    player: %s", g.spyOn.load() ? "ON" : "off", jobsN, g.playerFound.load() ? "found" : "not found");
        blabel({ 1000, 84, 480, 48 }, st, 18, 5, 1.0f, 0.95f, 0.88f);
    }

    char buf[200];
    const float lx = kL.x, lw = kL.w, rx = kR.x, rw = kR.w;
    float y = kTop;

    if (s_btab == 0) {
        g.infStamina = btgl({ lx, y, lw, 40 }, "Infinite stamina", g.infStamina.load() != 0); y += 52;
        g.speedOn = btgl({ lx, y, lw, 40 }, "Speed multiplier", g.speedOn.load() != 0); y += 52;
        snprintf(buf, sizeof(buf), "Multiplier: x%.2f", g.speedPct.load() / 100.0f);
        ink({ lx, y, lw, 26 }, buf, 17); y += 30;
        g.speedPct = static_cast<int>(bslider({ lx, y, lw, 34 }, static_cast<float>(g.speedPct.load()), 50.0f, 400.0f) + 0.5f); y += 50;
        g.infJump = btgl({ lx, y, lw, 40 }, "Infinite jump", g.infJump.load() != 0) ? 1 : 0; y += 52;
        g.jumpOn = btgl({ lx, y, lw, 40 }, "Jump height multiplier", g.jumpOn.load() != 0) ? 1 : 0; y += 52;
        snprintf(buf, sizeof(buf), "Jump: x%.2f", g.jumpPct.load() / 100.0f);
        ink({ lx, y, lw, 26 }, buf, 17); y += 30;
        g.jumpPct = static_cast<int>(bslider({ lx, y, lw, 34 }, static_cast<float>(g.jumpPct.load()), 50.0f, 500.0f) + 0.5f); y += 50;
        ink({ rx, kTop, rw, 160 }, g.playerFound.load() ? "Player: found" : "Player: not found (start a game)");
        ink({ rx, kTop + 40, rw, 200 }, "Insert (or your own key) opens and closes this book.\nKey binds: last bookmark, Debug / Keys.\nAfter a game update: open Debug / Keys and check the log\nif something stops working.");
    } else if (s_btab == 1) {
        g.autoAccept = btgl({ lx, y, lw, 40 }, "Auto-accept orders at the register", g.autoAccept.load() != 0); y += 52;
        snprintf(buf, sizeof(buf), "Delay between attempts: %d ms", g.acceptDelayMs.load());
        ink({ lx, y, lw, 26 }, buf, 17); y += 30;
        g.acceptDelayMs = static_cast<int>(bslider({ lx, y, lw, 34 }, static_cast<float>(g.acceptDelayMs.load()), 1500.0f, 8000.0f) + 0.5f); y += 50;
        snprintf(buf, sizeof(buf), "Orders submitted: %d", g.acceptedCount.load());
        ink({ lx, y, lw, 26 }, buf, 17); y += 32;
        { int mode = g.acceptMode.load();
          snprintf(buf, sizeof(buf), "Last mode: %s", mode == 1 ? "HOST / solo (server's order list)" : mode == 2 ? "CLIENT (text on the register)" : "none yet");
          ink({ lx, y, lw, 26 }, buf, 17); }
        draw_tickets(rx, rw);
    } else if (s_btab == 2) {
        g.timeOn = btgl({ lx, y, lw, 40 }, "Custom game speed (Time.timeScale)", g.timeOn.load() != 0); y += 52;
        snprintf(buf, sizeof(buf), "Game speed: x%.2f", g.timePct.load() / 100.0f);
        ink({ lx, y, lw, 26 }, buf, 17); y += 30;
        g.timePct = static_cast<int>(bslider({ lx, y, lw, 34 }, static_cast<float>(g.timePct.load()), 10.0f, 300.0f) + 0.5f);
        if (bbtn({ rx, kTop, rw, 44 }, "FULL DUMP (classes, kitchen, recipes, world)")) g.requestDump = 1;
        ink({ rx, kTop + 60, rw, 220 }, "Do it INSIDE a shift (recipes and stations must exist).\nWrites hhbc_details.txt, hhbc_kitchen.txt, hhbc_recipes.txt,\nhhbc_world.txt into the game folder and logs to hhbc_mod.log.\nThe game may stutter for a second.");
    } else if (s_btab == 3) {
        g.labVisible = 1;
        ink({ lx, y, lw, 24 }, "PATTIES (raw types from the ingredient boxes)", 17); y += 28;
        {
            char nm[96]; int n = 0;
            const float cw2 = (lw - 8) / 2;
            for (int i = 0; i < 8; i++) {
                if (!cook_patty_name_ui(i, nm, sizeof(nm))) break;
                char t[140]; snprintf(t, sizeof(t), "Cook: %s", nm);
                if (bbtn({ lx + (i % 2) * (cw2 + 8), y + (i / 2) * 42.0f, cw2, 36 }, t)) { char c[120]; snprintf(c, sizeof(c), "patty:%s", nm); cook_request(c); }
                n = i + 1;
            }
            if (!n) { ink({ lx, y, lw, 24 }, "(no raw patty boxes yet - start a shift)"); n = 2; }
            y += ((n + 1) / 2) * 42.0f + 6;
        }
        {
            const float cw2 = (lw - 8) / 2;
            bool b = btgl({ lx, y, cw2, 36 }, "Patty -> board", g.dstBoard.load() != 0);
            bool inv = btgl({ lx + cw2 + 8, y, cw2, 36 }, "Patty -> inventory", g.dstInv.load() != 0);
            if (b != (g.dstBoard.load() != 0)) { g.dstBoard = b ? 1 : 0; if (b) g.dstInv = 0; }
            if (inv != (g.dstInv.load() != 0)) { g.dstInv = inv ? 1 : 0; if (inv) g.dstBoard = 0; }
            y += 48;
        }
        ink({ lx, y, lw, 24 }, "SIDES AND BAKED GOODS", 17); y += 28;
        {
            const float w3 = (lw - 16) / 3;
            if (bbtn({ lx, y, w3, 36 }, "Cookies + pies")) cook_request("trays");
            if (bbtn({ lx + w3 + 8, y, w3, 36 }, "French fries")) cook_request("fries");
            if (bbtn({ lx + 2 * (w3 + 8), y, w3, 36 }, "Nuggets")) cook_request("nuggets");
            y += 44;
            const float w4 = (lw - 24) / 4;
            if (bbtn({ lx, y, w4, 34 }, "Cookie > bag")) cook_request("bag:cookie");
            if (bbtn({ lx + (w4 + 8), y, w4, 34 }, "Pie > bag")) cook_request("bag:pie");
            if (bbtn({ lx + 2 * (w4 + 8), y, w4, 34 }, "Fries > bag")) cook_request("bag:fries");
            if (bbtn({ lx + 3 * (w4 + 8), y, w4, 34 }, "Nuggets > bag")) cook_request("bag:nuggets");
        }
        // right page: drinks, shakes, alpha, jobs
        float ry = kTop;
        ink({ rx, ry, rw, 24 }, "DRINKS (cup, ice, soda, then into the bag)", 17); ry += 28;
        {
            static const char* kD[6] = { "Bump Cola", "Swimmers Choice", "Greap", "Jazzy Java", "Blob Aqua", "Agent Orange" };
            const float w3 = (rw - 16) / 3;
            for (int i = 0; i < 6; i++) {
                char c[80];
                if (bbtn({ rx + (i % 3) * (w3 + 8), ry + (i / 3) * 42.0f, w3, 36 }, kD[i])) { snprintf(c, sizeof(c), "soda:%s", kD[i]); cook_request(c); }
            }
            ry += 92;
            const float w2 = (rw - 8) / 2;
            if (bbtn({ rx, ry, w2, 36 }, "Soda of the accepted order")) cook_request("soda:order");
            if (bbtn({ rx + w2 + 8, ry, w2, 36 }, "CANCEL ALL JOBS")) cook_request("cancel");
            ry += 48;
            const float w3b = (rw - 16) / 3;
            if (bbtn({ rx, ry, w3b, 36 }, "Shake: Strawberry")) cook_request("shake:strawberry");
            if (bbtn({ rx + w3b + 8, ry, w3b, 36 }, "Shake: Blueberry")) cook_request("shake:blueberry");
            if (bbtn({ rx + 2 * (w3b + 8), ry, w3b, 36 }, "Shake of the order")) cook_request("shake:order");
            ry += 52;
        }
        {
            static std::string jobs; static int tk = 0;
            if ((tk++ % 6) == 0) {
                jobs.clear(); char ln[240];
                for (int i = 0; i < 12 && cook_job_line(i, ln, sizeof(ln)); i++) { jobs += ln; jobs += '\n'; }
                if (jobs.empty()) jobs = "(no active lists)";
            }
            ink({ rx, ry, rw, 880 - ry }, jobs.c_str(), 15);
        }
    } else if (s_btab == 4) {
        g.labVisible = 1;
        ink({ lx, y, lw, 90 }, "LAB: automatic tools. Spy ON (Debug / Keys) writes every step to the log.\nBETA CLOSEOUT does the ACTIVE after-shift duties: thermostats, rat traps,\nappliances off, trash, grease, napkins, filters, restock boxes, fires (experimental).", 16); y += 104;
        if (bbtn({ lx, y, lw, 54 }, "BETA CLOSEOUT: do the current after-shift duties")) cook_request("closeout"); y += 66;
        if (bbtn({ lx, y, lw, 44 }, "CANCEL ALL JOBS")) cook_request("cancel"); y += 56;
        if (bbtn({ lx, y, lw, 44 }, "STOCK: cook the missing fries / nuggets / cookies / pies")) cook_request("stock"); y += 56;
        g.stockAuto = 0; g.alphaDrive = 0;      // switches removed from the menu: ALPHA is started from the tickets (Orders bookmark) and does not restock by itself
        float ry = kTop;
        ink({ rx, ry, rw, 24 }, "JOBS", 17); ry += 30;
        {
            char jl[220]; int shown = 0;
            for (int i = 0; i < 8 && cook_job_line(i, jl, sizeof(jl)); i++) { ink({ rx, ry, rw, 40 }, jl, 15); ry += 40; shown++; }
            if (!shown) { ink({ rx, ry, rw, 24 }, "(no jobs running)", 15); ry += 30; }
            ry += 10;
        }
        ink({ rx, ry, rw, 24 }, "LOG", 17); ry += 28;
        static std::vector<std::string> tail; static std::string joined; static int tick = 0;
        if ((tick++ % 10) == 0) {
            log_tail(tail, 8); joined.clear();
            for (auto& l : tail) { size_t p = l.find("] "); joined += (p == std::string::npos ? l : l.substr(p + 2)); joined += '\n'; }
        }
        ink({ rx, ry, rw, 880 - ry }, joined.c_str(), 14);
    } else if (s_btab == 5) {
        ink({ lx, y, lw, 70 }, "VISUAL: look-through helpers. They only change how things are DRAWN on your screen.\nFullbright removes the darkness / fog. ESP writes the name and distance next to the thing.", 16); y += 84;
        g.fullbright = btgl({ lx, y, lw, 44 }, "Fullbright (bright ambient light, no fog)", g.fullbright.load() != 0) ? 1 : 0; y += 56;
        g.espEnemies = btgl({ lx, y, lw, 44 }, "ESP: enemies (cultists, chefs, mechanoids, demons...)", g.espEnemies.load() != 0) ? 1 : 0; y += 56;
        g.espObjects = btgl({ lx, y, lw, 44 }, "ESP: objects (rat traps, grease, boxes, trash, napkins, filters)", g.espObjects.load() != 0) ? 1 : 0; y += 56;
        float ry = kTop;
        ink({ rx, ry, rw, 300 }, "Notes:\n- Enemies are found by their game class names and hidden once their 'dead' flag is set.\n- Labels are drawn also when this menu is closed.\n- The list is refreshed once a second.\n- Switching Fullbright off restores the game's own lighting.\n- If something does not work, the log has a line starting with 'visual:'.", 16);
    } else {
        bool on = btgl({ lx, y, lw, 40 }, "Spy ON (log my interactions / appliance calls)", g.spyOn.load() != 0);
        if (on) g.spyInstall = 1;
        g.spyOn = on ? 1 : 0;
        y += 50;
        if (bbtn({ lx, y, 130, 34 }, "Clear")) g.spyClear = 1;
        snprintf(buf, sizeof(buf), "hooks: %d   events: %d   (also in hhbc_mod.log)", g.spyHooks.load(), g.spyEvents.load());
        ink({ lx + 144, y + 6, lw - 144, 24 }, buf, 15);
        y += 46;
        {
            static const char* kMarks[] = { "grill", "board", "drink", "fryer", "oven", "bag" };
            const float w3 = (lw - 16) / 3;
            for (int i = 0; i < 6; i++) {
                char t[32]; snprintf(t, sizeof(t), "MARK: %s", kMarks[i]);
                if (bbtn({ lx + (i % 3) * (w3 + 8), y + (i / 3) * 40.0f, w3, 34 }, t)) spy_mark(kMarks[i]);
            }
            y += 88;
        }
        static std::vector<std::string> lines; static std::string joined; static int lastVer = -1;
        int ver = spy_version();
        if (ver != lastVer) {
            lastVer = ver; spy_tail(lines, 12); joined.clear();
            for (auto& l : lines) { joined += l; joined += '\n'; }
            if (joined.empty()) joined = "(console empty - turn Spy ON, then press buttons / grab items in the game)";
        }
        ink({ lx, y, lw, 650 - y }, joined.c_str(), 13);
        // right page: key binds
        float ry = kTop;
        ink({ rx, ry, rw, 62 }, "KEY BINDS: click a row, then press the key (keyboard or extra mouse button).\nEsc while waiting = clear the bind. Saved to hhbc_binds.ini.", 15); ry += 68;
        const int listen = g.bindListen.load();
        for (int i = 0; i < State::kBindCount; i++) {
            ink({ rx, ry + 6, rw - 250, 26 }, bind_action_name(i), 15);
            char t[64], nm[40];
            if (listen == i) snprintf(t, sizeof(t), "press a key...");
            else {
                int vk = g.bind[i].load();
                if (vk > 0) { vk_name(vk, nm, sizeof(nm)); snprintf(t, sizeof(t), "[ %s ]", nm); } else snprintf(t, sizeof(t), "none");
            }
            if (bbtn({ rx + rw - 240, ry, 240, 34 }, t, listen == i ? 2 : -1)) g.bindListen = (listen == i) ? -1 : i;
            ry += 38;
        }
    }

    snprintf(buf, sizeof(buf), "ticks: order=%d look=%d | OnGUI calls=%d exc=%d", g.tickOrder.load(), g.tickLook.load(), s_calls, s_exceptions);
    blabel({ 815, 912, 560, 22 }, buf, 13, 3, 1.0f, 0.95f, 0.88f);

    set_style(0, 0); set_col(1.0f, 1.0f, 1.0f);                // give the style and the colour back to the game
    float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    void* a[] = { id }; invoke_on(m_setMatrix, nullptr, a);
}

void guarded_draw() {
    __try { if (book_ready()) draw_book(); else draw_ui(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s_exceptions++; }
}

// ---- our own OnGUI host --------------------------------------------------
// The game's ScytheNetworkStats component is not active, so its OnGUI never runs.
// We create a GameObject with that component ourselves (OnGUI is hooked, and the
// component's lifecycle methods are turned into no-ops for OUR instance only).
void* g_host = nullptr;
bool g_creating = false;
int g_hostTries = 0;
ULONGLONG g_hostLast = 0;

using FnLife = void (*)(void*, void*);
FnLife o_awake = nullptr, o_start = nullptr, o_update = nullptr, o_destroy = nullptr, o_validate = nullptr;

void Hook_Awake(void* self, void* mi)    { if (g_creating || (g_host && self == g_host)) return; o_awake(self, mi); }
void Hook_Start(void* self, void* mi)    { if (g_host && self == g_host) return; o_start(self, mi); }
void Hook_Update(void* self, void* mi)   { if (g_host && self == g_host) return; o_update(self, mi); }
void Hook_Destroy(void* self, void* mi)  { if (g_host && self == g_host) return; o_destroy(self, mi); }
void Hook_Validate(void* self, void* mi) { if (g_creating || (g_host && self == g_host)) return; o_validate(self, mi); }

void Hook_OnGUI(void* self, void* mi) {
    if (g_host && self == g_host) {
        s_calls++;
        if (s_calls == 1) logf("OnGUI of our host is being called - menu drawing is alive");
        if (g.menuOpen.load()) guarded_draw();
        guarded_visual();
        return;                               // our host never runs the original stats drawing
    }
    o_onGUI(self, mi);
}

bool hook_one(void* klass, const char* name, void* detour, void** orig) {
    void* mi = il2::find_method(klass, name, 0);
    if (!mi) return false;
    void* target = il2::native(mi);
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) logf("hook %s failed: %s", name, MH_StatusToString(st));
    return st == MH_OK;
}

void* s_statsClass = nullptr;

void* create_host_inner() {
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    void* kGO = il2::find_class(core, "UnityEngine", "GameObject");
    void* kObj = il2::find_class(core, "UnityEngine", "Object");
    if (!kGO || !s_statsClass) return nullptr;
    const char* sp[] = { "System.String" };
    const char* tp[] = { "System.Type" };
    const char* op[] = { "UnityEngine.Object" };
    void* ctor = il2::find_method_sig(kGO, ".ctor", sp, 1);
    void* addc = il2::find_method_sig(kGO, "AddComponent", tp, 1);
    void* dont = il2::find_method_sig(kObj, "DontDestroyOnLoad", op, 1);
    if (!ctor || !addc) return nullptr;

    void* exc = nullptr;
    void* go = il2::object_new(kGO);
    void* name = il2::string_new("HHBC_Host");
    void* ca[] = { name };
    il2::runtime_invoke(ctor, go, ca, &exc);
    if (exc) { logf("GameObject ctor threw"); return nullptr; }

    void* typeObj = il2::type_get_object(il2::class_get_type(s_statsClass));
    void* aa[] = { typeObj };
    exc = nullptr;
    void* comp = il2::runtime_invoke(addc, go, aa, &exc);
    if (exc || !comp) { logf("AddComponent threw or returned null"); return nullptr; }
    if (dont) { void* da[] = { go }; exc = nullptr; il2::runtime_invoke(dont, nullptr, da, &exc); }
    return comp;
}

void* create_host_seh() {
    __try { return create_host_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

}  // namespace

bool overlay_install() {
    void* guiImg = il2::find_image("UnityEngine.IMGUIModule.dll");
    void* guiCls = il2::find_class(guiImg, "UnityEngine", "GUI");
    if (!guiCls) { logf("UnityEngine.GUI not found"); return false; }

    const char* box_p[] = { "UnityEngine.Rect", "System.String" };
    const char* tog_p[] = { "UnityEngine.Rect", "System.Boolean", "System.String" };
    const char* sld_p[] = { "UnityEngine.Rect", "System.Single", "System.Single", "System.Single" };
    m_box = il2::find_method_sig(guiCls, "Box", box_p, 2);
    m_label = il2::find_method_sig(guiCls, "Label", box_p, 2);
    m_button = il2::find_method_sig(guiCls, "Button", box_p, 2);
    m_toggle = il2::find_method_sig(guiCls, "Toggle", tog_p, 3);
    m_slider = il2::find_method_sig(guiCls, "HorizontalSlider", sld_p, 4);
    if (!m_box || !m_label || !m_button || !m_toggle || !m_slider) { logf("IMGUI methods missing"); return false; }

    void* cs = il2::find_image("Assembly-CSharp.dll");
    void* stats = il2::find_class(cs, "PurrNet", "ScytheNetworkStats");
    s_statsClass = stats;
    void* onGui = il2::find_method(stats, "OnGUI", 0);
    if (!onGui) return false;
    hook_one(stats, "Awake", reinterpret_cast<void*>(&Hook_Awake), reinterpret_cast<void**>(&o_awake));
    hook_one(stats, "Start", reinterpret_cast<void*>(&Hook_Start), reinterpret_cast<void**>(&o_start));
    hook_one(stats, "Update", reinterpret_cast<void*>(&Hook_Update), reinterpret_cast<void**>(&o_update));
    hook_one(stats, "OnDestroy", reinterpret_cast<void*>(&Hook_Destroy), reinterpret_cast<void**>(&o_destroy));
    hook_one(stats, "OnValidate", reinterpret_cast<void*>(&Hook_Validate), reinterpret_cast<void**>(&o_validate));
    void* target = il2::native(onGui);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&Hook_OnGUI), reinterpret_cast<void**>(&o_onGUI));
    if (s == MH_OK) s = MH_EnableHook(target);
    logf("hook ScytheNetworkStats.OnGUI: %s", MH_StatusToString(s));
    g.overlayOk = (s == MH_OK);
    return s == MH_OK;
}

void overlay_ensure_host() {
    if (g_host || !g.overlayOk.load() || g_hostTries >= 5) return;
    ULONGLONG now = GetTickCount64();
    if (g_hostLast && now - g_hostLast < 3000) return;
    g_hostLast = now;
    g_hostTries++;
    g_creating = true;
    void* h = create_host_seh();
    g_creating = false;
    if (h) { g_host = h; g.hostOk = 1; logf("OnGUI host created (try %d)", g_hostTries); }
    else logf("OnGUI host creation failed (try %d)", g_hostTries);
}

void vk_name(int vk, char* out, int cap) {
    const char* fixed = nullptr;
    switch (vk) {
    case 0: fixed = "none"; break;
    case VK_INSERT: fixed = "Insert"; break;
    case 0x04: fixed = "Mouse3 (middle)"; break;
    case 0x05: fixed = "Mouse4"; break;
    case 0x06: fixed = "Mouse5"; break;
    case 0x2E: fixed = "Delete"; break;
    case 0x24: fixed = "Home"; break;
    case 0x23: fixed = "End"; break;
    case 0x21: fixed = "PageUp"; break;
    case 0x22: fixed = "PageDown"; break;
    case 0x25: fixed = "Left"; break;
    case 0x26: fixed = "Up"; break;
    case 0x27: fixed = "Right"; break;
    case 0x28: fixed = "Down"; break;
    case 0x20: fixed = "Space"; break;
    case 0x09: fixed = "Tab"; break;
    case 0x0D: fixed = "Enter"; break;
    case 0xA0: case 0x10: fixed = "Shift"; break;
    case 0xA1: fixed = "Right Shift"; break;
    case 0xA2: case 0x11: fixed = "Ctrl"; break;
    case 0xA3: fixed = "Right Ctrl"; break;
    case 0xA4: case 0x12: fixed = "Alt"; break;
    case 0xA5: fixed = "Right Alt"; break;
    default: break;
    }
    if (fixed) { snprintf(out, static_cast<size_t>(cap), "%s", fixed); return; }
    if (vk >= 0x30 && vk <= 0x39) { snprintf(out, static_cast<size_t>(cap), "%c", vk); return; }
    if (vk >= 0x41 && vk <= 0x5A) { snprintf(out, static_cast<size_t>(cap), "%c", vk); return; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(out, static_cast<size_t>(cap), "F%d", vk - 0x6F); return; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(out, static_cast<size_t>(cap), "Num%d", vk - 0x60); return; }
    snprintf(out, static_cast<size_t>(cap), "key 0x%02X", vk);
}
