/* Small always-on-top Win32 window with toggles (no CRT, no windows.h).
 * Insert = show/hide.  Hotkeys work even when the mouse is locked by the game:
 *   F6 infinite stamina, F7 speed boost, F8 speed -0.25, F9 speed +0.25 */
#ifndef UI_H
#define UI_H
#include "mod.h"

typedef unsigned long long UPTR;
typedef long long LPR;
typedef struct {
    unsigned int cbSize, style;
    LPR (*proc)(void *, unsigned int, UPTR, LPR);
    int cbCls, cbWnd;
    void *inst, *icon, *cursor, *brush;
    const char *menu, *cls;
    void *iconSm;
} WNDCLASSEXA_;
typedef struct {
    void *hwnd;
    unsigned int message;
    UPTR wParam;
    LPR lParam;
    unsigned int time;
    long px, py;
    unsigned int priv;
} MSG_;

_Static_assert(sizeof(WNDCLASSEXA_) == 80, "WNDCLASSEXA size on x64");
_Static_assert(sizeof(MSG_) == 48, "MSG size on x64");

#define U32 __declspec(dllimport)
U32 unsigned short RegisterClassExA(const WNDCLASSEXA_ *);
U32 void *CreateWindowExA(unsigned long, const char *, const char *, unsigned long, int, int, int, int,
                          void *, void *, void *, void *);
U32 LPR DefWindowProcA(void *, unsigned int, UPTR, LPR);
U32 int ShowWindow(void *, int);
U32 int PeekMessageA(MSG_ *, void *, unsigned int, unsigned int, unsigned int);
U32 int TranslateMessage(const MSG_ *);
U32 LPR DispatchMessageA(const MSG_ *);
U32 short GetAsyncKeyState(int);
U32 LPR SendMessageA(void *, unsigned int, UPTR, LPR);
U32 int SetWindowTextA(void *, const char *);
U32 void *LoadCursorA(void *, const char *);
U32 int IsWindowVisible(void *);
U32 int SetForegroundWindow(void *);
U32 void *GetStockObject(int);

static Mod *g_ui_mod;
static void *g_hwnd, *g_h_stam, *g_h_speed, *g_h_label, *g_h_status;

static void fmt_speed(char *d, int pct) {
    const char *a = "Speed multiplier: x";
    int i = 0;
    while (*a) d[i++] = *a++;
    d[i++] = (char)('0' + pct / 100);
    d[i++] = '.';
    d[i++] = (char)('0' + (pct / 10) % 10);
    d[i++] = (char)('0' + pct % 10);
    d[i] = 0;
}
static void refresh_label(void) {
    char t[40];
    fmt_speed(t, g_ui_mod->speed_pct);
    SetWindowTextA(g_h_label, t);
}
static void set_check(void *h, int on) { SendMessageA(h, 0xF1 /*BM_SETCHECK*/, on ? 1 : 0, 0); }
static void bump_speed(int d) {
    int v = g_ui_mod->speed_pct + d;
    if (v < 50) v = 50;
    if (v > 400) v = 400;
    g_ui_mod->speed_pct = v;
    refresh_label();
}

static LPR wndproc(void *h, unsigned int msg, UPTR w, LPR l) {
    if (msg == 0x111 /*WM_COMMAND*/) {
        int id = (int)(w & 0xFFFF);
        if (id == 101) g_ui_mod->inf_stamina = (SendMessageA((void *)l, 0xF0 /*BM_GETCHECK*/, 0, 0) == 1);
        else if (id == 102) g_ui_mod->speed_on = (SendMessageA((void *)l, 0xF0, 0, 0) == 1);
        else if (id == 103) bump_speed(-25);
        else if (id == 104) bump_speed(25);
        return 0;
    }
    if (msg == 0x10 /*WM_CLOSE*/) { ShowWindow(h, 0); return 0; }
    return DefWindowProcA(h, msg, w, l);
}

static void *mk(const char *cls, const char *text, unsigned long style, int x, int y, int w, int h, int id, void *inst) {
    void *c = CreateWindowExA(0, cls, text, 0x50000000 /*WS_CHILD|WS_VISIBLE*/ | style, x, y, w, h, g_hwnd,
                              (void *)(UPTR)id, inst, 0);
    if (c) SendMessageA(c, 0x30 /*WM_SETFONT*/, (UPTR)GetStockObject(17 /*DEFAULT_GUI_FONT*/), 1);
    return c;
}

static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static DWORD ui_thread(void *p) {
    WNDCLASSEXA_ wc;
    MSG_ msg;
    void *inst = GetModuleHandleA(0);
    int loops = 0, prev_ins = 0, prev[4] = {0, 0, 0, 0};
    const char *last_status = 0;
    char t[40];
    unsigned char *z = (unsigned char *)&wc;
    unsigned long i;
    (void)p;
    for (i = 0; i < sizeof(wc); i++) z[i] = 0;
    wc.cbSize = sizeof(wc);
    wc.proc = wndproc;
    wc.inst = inst;
    wc.cursor = LoadCursorA(0, (const char *)(UPTR)32512 /*IDC_ARROW*/);
    wc.brush = (void *)(UPTR)16; /* COLOR_BTNFACE + 1 */
    wc.cls = "HhbcScoutWnd";
    if (!RegisterClassExA(&wc)) return 1;
    g_hwnd = CreateWindowExA(0x88 /*TOPMOST|TOOLWINDOW*/, wc.cls, "HHBC mod  [Insert]", 0x00C80000 /*caption+sysmenu*/,
                             60, 60, 310, 250, 0, 0, inst, 0);
    if (!g_hwnd) return 2;
    g_h_stam = mk("BUTTON", "Infinite stamina  (F6)", 0x3 /*AUTOCHECKBOX*/, 16, 14, 270, 22, 101, inst);
    g_h_speed = mk("BUTTON", "Speed boost  (F7)", 0x3, 16, 44, 270, 22, 102, inst);
    fmt_speed(t, g_ui_mod->speed_pct);
    g_h_label = mk("STATIC", t, 0, 16, 80, 160, 20, 105, inst);
    mk("BUTTON", "-", 0, 180, 76, 34, 26, 103, inst);
    mk("BUTTON", "+", 0, 220, 76, 34, 26, 104, inst);
    g_h_status = mk("STATIC", "Player: waiting", 0, 16, 118, 270, 20, 106, inst);
    mk("STATIC", "Insert shows/hides this window. F8/F9 change speed. Use windowed or borderless mode.",
       0, 16, 148, 270, 44, 107, inst);

    for (;;) {
        int ins = key_down(0x2D);
        int k[4], j;
        k[0] = key_down(0x75); k[1] = key_down(0x76); k[2] = key_down(0x77); k[3] = key_down(0x78);
        while (PeekMessageA(&msg, 0, 0, 0, 1)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (ins && !prev_ins) {
            if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, 0);
            else { ShowWindow(g_hwnd, 1); SetForegroundWindow(g_hwnd); }
        }
        prev_ins = ins;
        if (k[0] && !prev[0]) { g_ui_mod->inf_stamina = !g_ui_mod->inf_stamina; set_check(g_h_stam, g_ui_mod->inf_stamina); }
        if (k[1] && !prev[1]) { g_ui_mod->speed_on = !g_ui_mod->speed_on; set_check(g_h_speed, g_ui_mod->speed_on); }
        if (k[2] && !prev[2]) bump_speed(-25);
        if (k[3] && !prev[3]) bump_speed(25);
        for (j = 0; j < 4; j++) prev[j] = k[j];
        if ((++loops % 10) == 0 && g_ui_mod->status != last_status) {
            last_status = g_ui_mod->status;
            SetWindowTextA(g_h_status, last_status);
        }
        Sleep(25);
    }
}
#endif
