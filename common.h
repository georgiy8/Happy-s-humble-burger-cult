#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Shared state: written by the menu (render thread), read by the hooks on the game thread.
struct State {
    std::atomic<int> infStamina{0};
    std::atomic<int> speedOn{0};
    std::atomic<int> speedPct{150};      // 50..400
    std::atomic<int> timeOn{0};
    std::atomic<int> timePct{100};       // 10..300
    std::atomic<int> autoAccept{0};
    std::atomic<int> acceptDelayMs{1500};
    std::atomic<int> menuOpen{0};
    std::atomic<int> requestDump{0};
    std::atomic<int> playerFound{0};
    std::atomic<int> playerHookOk{0};
    std::atomic<int> orderHookOk{0};
    std::atomic<int> lookHookOk{0};
    std::atomic<int> overlayOk{0};
    std::atomic<int> acceptedCount{0};
    std::atomic<int> ready{0};
    std::atomic<int> tickOrder{0};
    std::atomic<int> tickLook{0};
    std::atomic<int> hostOk{0};
    std::atomic<int> spyOn{0};         // debug console: log player actions
    std::atomic<int> spyInstall{0};    // request: install spy hooks (once)
    std::atomic<int> spyClear{0};
    std::atomic<int> spyHooks{0};
    std::atomic<int> spyEvents{0};
    std::atomic<int> labCmd{0};        // lab experiment requested from the menu (1..5)
    std::atomic<int> labBoxIdx{0};
    std::atomic<int> labVisible{0};
    std::atomic<int> alphaDrive{0};    // Alpha also builds drive-thru orders (default: normal orders only)
    std::atomic<int> infJump{0};       // infinite (air) jumps
    std::atomic<int> jumpOn{0};        // jump height multiplier
    std::atomic<int> jumpPct{150};     // 50..500
    std::atomic<int> stockAuto{0};     // Alpha: cook missing sides after each order
    std::atomic<int> fullbright{0};    // Visual tab
    std::atomic<int> espEnemies{0};
    std::atomic<int> espObjects{0};
    std::atomic<int> dstBoard{0};      // finished patties -> assembly board
    std::atomic<int> dstInv{1};        // finished patties -> inventory (default)    // set by the menu while the Lab tab is drawn     // which ingredient box the lab uses
    std::atomic<int> acceptMode{0};
    // key binds: virtual-key code per action, 0 = unbound
    static constexpr int kBindCount = 14;
    std::atomic<int> bind[kBindCount];
    std::atomic<int> bindListen{-1};   // action currently waiting for a key (-1 = none)
    std::atomic<int> bindsDirty{0};    // 0 none yet, 1 host/solo (pendingOrders), 2 client (register text)
    std::atomic<int> ownCall{0};       // 1 while the mod itself calls game methods (not logged)
};
extern State g;
extern std::string g_gameDir;   // folder of the game exe, with trailing backslash

void log_open();
void logf(const char* fmt, ...);
void log_tail(std::vector<std::string>& out, size_t maxLines);

// features.cpp
bool features_init(void* gameImage);   // look up classes/fields/methods by name
bool features_hook();                  // MinHook hooks on game methods
// spy.cpp  (debug console)
void spy_tick();                                              // main thread: install hooks / clear on request
size_t spy_tail(std::vector<std::string>& out, size_t maxLines);
int spy_version();
void spy_mark(const char* label);                              // writes a ==== MARK ==== line into the log
// lab.cpp (experiments for auto-cook, main thread)
void lab_tick();
bool lab_box_name(int i, char* out, int cap);
// cook jobs (lab.cpp)
void cook_request(const char* cmd);                 // e.g. "patty:Raw Beef Patty", "fries", "nuggets", "trays", "bag:pie", "soda:Blob Aqua", "soda:order", "cancel"
void features_register_orders(std::vector<void*>& regs, std::vector<std::vector<std::string>>& orders);
bool features_recipe_info(const char* name, int& kind, std::vector<std::string>& ingIds);
bool cook_order_busy(void* key);                    // true while ALPHA is building this accepted order
bool cook_job_line(int i, char* out, int cap);      // status of job i (false past the end)
bool cook_patty_name(int i, char* out, int cap);    // raw patty types found in ingredient boxes
void spy_note(const char* fmt, ...);                // line from the mod itself into the Spy console / log
struct AcceptedOrder { void* key = nullptr; int id = 0; int type = 0; float t = 0.0f; std::vector<std::string> items; };
void features_accepted_orders(std::vector<AcceptedOrder>& out);   // OrderManager.activeOrders = orders that were ACCEPTED, oldest first
void features_order_items(std::vector<std::string>& out);   // item names currently shown on the registers
const char* bind_action_name(int i);
void vk_name(int vk, char* out, int cap);
void binds_init();
// overlay.cpp
bool overlay_install();
void overlay_ensure_host();            // main thread: creates the GameObject that receives OnGUI                // Unity IMGUI menu drawn from a hook on PurrNet.ScytheNetworkStats.OnGUI
// dump.cpp
void run_dump(void* gameImage);
