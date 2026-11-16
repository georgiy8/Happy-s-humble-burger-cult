#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <map>
#include <set>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
//  LAB: small, logged experiments for the future auto-cook.
//  Every step is what a player does, done by the mod with the player's own ID:
//    1  take an item from an ingredient box   (IngredientBoxGrab.Interact)
//    2  drop the held item and move it onto the grill trigger
//    3  toggle the grill                       (CookingZone.Toggle)
//    4  pick a cooked patty off the grill (Meat.Interact), drop it, move it onto
//       the nearest assembly board
//    5  report what the mod sees
//  Everything runs on the main thread from the per-frame tick. Results go to
//  hhbc_mod.log (turn Spy ON as well to see the game's own reaction).
// ============================================================================

namespace {

struct Step { ULONGLONG at; int kind; void* obj; };
std::vector<Step> s_queue;

bool s_ready = false;
void* k_player = nullptr; void* k_box = nullptr; void* k_generic = nullptr;
void* k_zone = nullptr; void* k_meat = nullptr; void* k_board = nullptr;
void* f_playerInst = nullptr;
int o_playerID = -1, o_inv = -1, o_tracked = -1;
int o_cur = -1, o_cooked = -1, o_burnt = -1, o_cookTime = -1, o_cookedTime = -1, o_burntTime = -1;
int o_listItems = 0x10, o_listSize = 0x18;

void* m_boxInteract = nullptr; void* m_meatInteract = nullptr; void* m_localDrop = nullptr;
void* m_zoneToggle = nullptr; void* m_getName = nullptr; void* m_getComponent = nullptr;
void* m_getTransform = nullptr; void* m_getPosition = nullptr; void* m_setPosition = nullptr;
void* m_findObjects = nullptr;
void* m_localTeleport = nullptr; void* m_getTime = nullptr; void* m_rbSetVel = nullptr;
void* k_rigidbody = nullptr;
int o_toSpawn = -1, o_lastThrow = -1;

std::mutex s_boxMtx;
std::vector<std::string> s_boxNames, s_patties;
ULONGLONG s_boxLast = 0;

inline void* rd_ptr(void* b, int off) { return *reinterpret_cast<void**>(static_cast<char*>(b) + off); }
inline int rd_int(void* b, int off) { return *reinterpret_cast<int*>(static_cast<char*>(b) + off); }
inline float rd_flt(void* b, int off) { return *reinterpret_cast<float*>(static_cast<char*>(b) + off); }

std::string uname(void* o) {
    if (!o || !m_getName) return std::string();
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m_getName, o, nullptr, &exc);
    return (exc || !r) ? std::string() : il2::to_utf8(r);
}

bool pos_of(void* comp, float out[3]) {
    if (!comp || !m_getTransform || !m_getPosition) return false;
    void* exc = nullptr;
    void* tr = il2::runtime_invoke(m_getTransform, comp, nullptr, &exc);
    if (exc || !tr) return false;
    void* box = il2::runtime_invoke(m_getPosition, tr, nullptr, &exc);
    if (exc || !box) return false;
    const float* v = reinterpret_cast<const float*>(static_cast<char*>(box) + 0x10);
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return true;
}

bool set_pos(void* comp, const float p[3]) {
    if (!comp || !m_getTransform || !m_setPosition) return false;
    void* exc = nullptr;
    void* tr = il2::runtime_invoke(m_getTransform, comp, nullptr, &exc);
    if (exc || !tr) return false;
    float v[3] = { p[0], p[1], p[2] };
    void* args[] = { v };
    il2::runtime_invoke(m_setPosition, tr, args, &exc);
    return exc == nullptr;
}

float dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;   // squared
}

std::vector<void*> find_objects(void* klass) {
    std::vector<void*> out;
    if (!klass || !m_findObjects) return out;
    void* typeObj = il2::type_get_object(il2::class_get_type(klass));
    void* args[] = { typeObj };
    void* exc = nullptr;
    void* arr = il2::runtime_invoke(m_findObjects, nullptr, args, &exc);
    if (exc || !arr) return out;
    int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18));
    for (int i = 0; i < n && i < 256; i++) {
        void* o = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
        if (o) out.push_back(o);
    }
    return out;
}

void* component_of(void* comp, void* klass) {
    if (!comp || !klass || !m_getComponent) return nullptr;
    void* typeObj = il2::type_get_object(il2::class_get_type(klass));
    void* args[] = { typeObj };
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m_getComponent, comp, args, &exc);
    return exc ? nullptr : r;
}

void* local_player() {
    void* p = nullptr;
    if (f_playerInst) il2::field_static_get_value(f_playerInst, &p);
    return p;
}

// PlayerID (16 bytes) of the local player: the SyncVar<PlayerID> playerID on ScytheNetworkPlayer
bool get_pid(unsigned char out[16]) {
    void* pl = local_player();
    if (!pl || o_playerID < 0) return false;
    void* sv = rd_ptr(pl, o_playerID);
    if (!sv) return false;
    for (void* kk = il2::object_get_class(sv); kk; kk = il2::class_get_parent(kk)) {
        void* it = nullptr;
        while (void* f = il2::class_get_fields(kk, &it)) {
            if (il2::field_get_flags(f) & 0x10) continue;
            char* tn = il2::type_get_name(il2::field_get_type(f));
            bool match = tn && strcmp(tn, "PurrNet.PlayerID") == 0;
            if (tn && il2::il2_free) il2::il2_free(tn);
            if (match) {
                memcpy(out, static_cast<char*>(sv) + il2::field_get_offset(f), 16);
                return true;
            }
        }
    }
    logf("lab: PlayerID field not found inside SyncVar<PlayerID> (class '%s')", il2::class_get_name(il2::object_get_class(sv)));
    return false;
}

std::vector<void*> held_items() {
    std::vector<void*> out;
    void* pl = local_player();
    if (!pl || o_inv < 0) return out;
    void* arr = rd_ptr(pl, o_inv);
    if (!arr) return out;
    int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18));
    for (int i = 0; i < n && i < 16; i++) {
        void* o = *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i);
        if (o) out.push_back(o);
    }
    return out;
}

// 0 = raw, 1 = cooked, 2 = burnt, -1 unknown
int meat_state(void* meat) {
    if (o_cur < 0) return -1;
    void* cur = rd_ptr(meat, o_cur);
    if (cur && cur == rd_ptr(meat, o_burnt)) return 2;
    if (cur && cur == rd_ptr(meat, o_cooked)) return 1;
    return 0;
}

void resolve() {
    if (s_ready) return;
    s_ready = true;
    void* cs = il2::find_image("Assembly-CSharp.dll");
    void* core = il2::find_image("UnityEngine.CoreModule.dll");
    k_player = il2::find_class(cs, "", "ScytheNetworkPlayer");
    k_box = il2::find_class(cs, "", "IngredientBoxGrab");
    k_generic = il2::find_class(cs, "", "GenericObject");
    k_zone = il2::find_class(cs, "", "CookingZone");
    k_meat = il2::find_class(cs, "", "Meat");
    k_board = il2::find_class(cs, "", "AssemblyBoard");
    f_playerInst = il2::find_field(k_player, "instance");
    o_playerID = il2::field_off(k_player, "playerID");
    o_inv = il2::field_off(k_player, "gameObjectsInInventory");
    o_tracked = il2::field_off(k_zone, "trackedFoodItems");
    o_toSpawn = il2::field_off(k_box, "toSpawn");
    o_lastThrow = il2::field_off(k_generic, "lastLocalThrowTime");
    o_cur = il2::field_off(k_meat, "currentIngredient");
    o_cooked = il2::field_off(k_meat, "cookedIngredient");
    o_burnt = il2::field_off(k_meat, "burntIngredient");
    o_cookTime = il2::field_off(k_meat, "cookTime");
    o_cookedTime = il2::field_off(k_meat, "cookedTime");
    o_burntTime = il2::field_off(k_meat, "burntTime");

    const char* pidp[] = { "PurrNet.PlayerID" };
    m_boxInteract = il2::find_method_sig(k_box, "Interact", pidp, 1);
    m_meatInteract = il2::find_method_sig(k_meat, "Interact", pidp, 1);
    m_localDrop = il2::find_method(k_generic, "LocalDrop", 0);
    m_zoneToggle = il2::find_method(k_zone, "Toggle", 0);
    m_localTeleport = il2::find_method(k_generic, "LocalTeleport", 1);

    void* kObj = il2::find_class(core, "UnityEngine", "Object");
    void* kComp = il2::find_class(core, "UnityEngine", "Component");
    void* kTr = il2::find_class(core, "UnityEngine", "Transform");
    m_getName = il2::find_method(kObj, "get_name", 0);
    m_getTransform = il2::find_method(kComp, "get_transform", 0);
    m_getPosition = il2::find_method(kTr, "get_position", 0);
    m_setPosition = il2::find_method(kTr, "set_position", 1);
    const char* tp[] = { "System.Type" };
    m_findObjects = il2::find_method_sig(kObj, "FindObjectsOfType", tp, 1);
    m_getComponent = il2::find_method_sig(kComp, "GetComponent", tp, 1);

    {
        void* kTime = il2::find_class(core, "UnityEngine", "Time");
        m_getTime = il2::find_method(kTime, "get_time", 0);
        void* phys = il2::find_image("UnityEngine.PhysicsModule.dll");
        k_rigidbody = il2::find_class(phys, "UnityEngine", "Rigidbody");
        m_rbSetVel = il2::find_method(k_rigidbody, "set_linearVelocity", 1);
        if (!m_rbSetVel) m_rbSetVel = il2::find_method(k_rigidbody, "set_velocity", 1);
    }
    void* ms = il2::find_image("mscorlib.dll");
    void* kList = il2::find_class(ms, "System.Collections.Generic", "List`1");
    if (kList) {
        int a = il2::field_off(kList, "_items");
        int b = il2::field_off(kList, "_size");
        if (a > 0 && b > 0) { o_listItems = a; o_listSize = b; }
    }
    logf("lab: LocalTeleport=%d Time.time=%d Rigidbody.velocity=%d lastLocalThrowTime off=0x%x", m_localTeleport != nullptr, m_getTime != nullptr, m_rbSetVel != nullptr, o_lastThrow);
    logf("lab: resolved (player=%d box=%d generic=%d zone=%d meat=%d board=%d, playerID off=0x%x inv off=0x%x)",
         k_player != nullptr, k_box != nullptr, k_generic != nullptr, k_zone != nullptr, k_meat != nullptr, k_board != nullptr,
         o_playerID, o_inv);
}

bool invoke_pid(void* method, void* obj, const unsigned char pid[16]) {
    alignas(16) unsigned char buf[16];
    memcpy(buf, pid, 16);
    void* args[] = { buf };
    void* exc = nullptr;
    il2::runtime_invoke(method, obj, args, &exc);
    return exc == nullptr;
}

std::vector<void*> boxes_sorted() {
    std::vector<void*> boxes = find_objects(k_box);
    std::vector<std::pair<std::string, void*>> tmp;
    for (void* b : boxes) tmp.push_back({ uname(b), b });
    std::sort(tmp.begin(), tmp.end(), [](const std::pair<std::string, void*>& a, const std::pair<std::string, void*>& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second; });
    std::vector<void*> out;
    for (auto& p : tmp) out.push_back(p.second);
    return out;
}

void refresh_boxes() {
    std::vector<void*> boxes = boxes_sorted();
    std::vector<std::string> names, patties;
    for (void* b : boxes) {
        std::string n = uname(b);
        void* ts = o_toSpawn >= 0 ? rd_ptr(b, o_toSpawn) : nullptr;
        std::string sp = uname(ts);
        size_t p = sp.find("(Clone)");
        if (p != std::string::npos) sp.erase(p);
        if (!sp.empty() && sp != n) n += "  ->  " + sp;
        {
            std::string l = sp;
            for (auto& c : l) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (l.find("raw") != std::string::npos && l.find("patty") != std::string::npos && std::find(patties.begin(), patties.end(), sp) == patties.end()) patties.push_back(sp);
        }
        names.push_back(n);
    }
    std::lock_guard<std::mutex> lk(s_boxMtx);
    s_boxNames.swap(names);
    s_patties.swap(patties);
}

// ---- hotbar --------------------------------------------------------------------
// The player's hotbar is ScytheNetworkPlayer.gameObjectsInInventory (GenericObject[]), the item in the
// hand is ScytheNetworkPlayer.heldObject / activeSlotIndex. GenericObject.LocalDrop() only lets go of the
// object physically; it does NOT always free the hotbar slot ("phantom" item that stays in the inventory
// although it lies on the grill). So after every throw the item is removed from the hotbar explicitly.
int Of(const char* cls, const char* f);
void* Mt(const char* cls, const char* name, int argc);

int inv_index(void* gen) {
    void* pl = local_player();
    if (!pl || o_inv < 0 || !gen) return -1;
    void* arr = rd_ptr(pl, o_inv);
    if (!arr) return -1;
    int n = static_cast<int>(*reinterpret_cast<uint64_t*>(static_cast<char*>(arr) + 0x18));
    for (int i = 0; i < n && i < 64; i++)
        if (*reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i) == gen) return i;
    return -1;
}

// before the drop: a STASHED item cannot be dropped, make it the active one (SwitchToSlot)
void hotbar_activate(void* gen, const char* tag) {
    void* pl = local_player();
    int idx = inv_index(gen);
    if (!pl || idx < 0) return;
    int offHeld = Of("ScytheNetworkPlayer", "heldObject");
    int offAct = Of("ScytheNetworkPlayer", "activeSlotIndex");
    if (offHeld < 0 || rd_ptr(pl, offHeld) == gen) return;                    // already in the hand
    if (offAct >= 0 && rd_int(pl, offAct) == idx) return;                     // do not toggle the active slot
    void* sw = Mt("ScytheNetworkPlayer", "SwitchToSlot", 1);
    if (!sw) return;
    int v = idx;
    void* a[] = { &v };
    void* exc = nullptr;
    il2::runtime_invoke(sw, pl, a, &exc);
    logf("%s: item was stashed in slot %d -> SwitchToSlot(%d) %s, in the hand now: %d", tag, idx, idx, exc ? "THREW" : "ok",
         rd_ptr(pl, offHeld) == gen ? 1 : 0);
}

// after the drop: make sure the hotbar slot is free
bool hotbar_remove(void* gen, const char* tag) {
    if (!gen || inv_index(gen) < 0) return true;
    void* pl = local_player();
    void* exc = nullptr;
    void* rm = Mt("GenericObject", "RemoveFromHotbar", 0);
    if (rm) { il2::runtime_invoke(rm, gen, nullptr, &exc); }
    if (inv_index(gen) < 0) { logf("%s: hotbar slot freed with GenericObject.RemoveFromHotbar()", tag); return true; }
    void* nt = Mt("ScytheNetworkPlayer", "NotifyHotbarItemRemoved", 1);
    if (nt && pl) { void* a[] = { gen }; exc = nullptr; il2::runtime_invoke(nt, pl, a, &exc); }
    if (inv_index(gen) < 0) { logf("%s: hotbar slot freed with NotifyHotbarItemRemoved()", tag); return true; }
    // last resort: clear the slot ourselves and refresh the HUD
    int idx = inv_index(gen);
    void* arr = (pl && o_inv >= 0) ? rd_ptr(pl, o_inv) : nullptr;
    if (arr && idx >= 0) *reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * idx) = nullptr;
    int offHeld = Of("ScytheNetworkPlayer", "heldObject");
    if (pl && offHeld >= 0 && rd_ptr(pl, offHeld) == gen) *reinterpret_cast<void**>(static_cast<char*>(pl) + offHeld) = nullptr;
    void* rf = Mt("ScytheNetworkPlayer", "RefreshHotbarUI", 0);
    if (rf && pl) { exc = nullptr; il2::runtime_invoke(rf, pl, nullptr, &exc); }
    logf("%s: hotbar slot %d cleared by force (the game's own removal did not free it)", tag, idx);
    return inv_index(gen) < 0;
}

// "Throw" the way the game sees it: drop through the game's own LocalDrop, free the hotbar slot,
// put the item above the target with the game's LocalTeleport, give it a downward velocity so the
// physics triggers fire like for a real throw, and stamp lastLocalThrowTime.
void throw_to(void* item, const float target[3], const char* tag) {
    void* gen = component_of(item, k_generic);
    if (!gen) gen = item;
    hotbar_activate(gen, tag);
    void* exc = nullptr;
    il2::runtime_invoke(m_localDrop, gen, nullptr, &exc);
    if (exc) logf("%s: LocalDrop threw", tag);
    hotbar_remove(gen, tag);
    bool ok = false;
    if (m_localTeleport) {
        float v[3] = { target[0], target[1], target[2] };
        void* a[] = { v };
        exc = nullptr;
        il2::runtime_invoke(m_localTeleport, gen, a, &exc);
        ok = exc == nullptr;
        if (exc) logf("%s: LocalTeleport threw", tag);
    }
    if (!ok) ok = set_pos(item, target);
    if (o_lastThrow >= 0 && m_getTime) {
        void* t = il2::runtime_invoke(m_getTime, nullptr, nullptr, &exc);
        if (t) *reinterpret_cast<float*>(static_cast<char*>(gen) + o_lastThrow) = *reinterpret_cast<float*>(static_cast<char*>(t) + 0x10);
    }
    void* rb = k_rigidbody ? component_of(item, k_rigidbody) : nullptr;
    if (rb && m_rbSetVel) {
        float vel[3] = { 0.0f, -2.5f, 0.0f };
        void* a[] = { vel };
        exc = nullptr;
        il2::runtime_invoke(m_rbSetVel, rb, a, &exc);
        if (exc) logf("%s: set velocity threw", tag);
    } else logf("%s: no Rigidbody/velocity setter (rb=%d)", tag, rb != nullptr);
    logf("%s: threw '%s' to (%.2f, %.2f, %.2f) teleport=%s, still in hotbar=%d", tag, uname(item).c_str(),
         static_cast<double>(target[0]), static_cast<double>(target[1]), static_cast<double>(target[2]), ok ? "ok" : "FAILED", inv_index(gen) >= 0 ? 1 : 0);
}

// ---- commands ---------------------------------------------------------------
void cmd_take() {
    unsigned char pid[16];
    if (!get_pid(pid)) { logf("lab[1]: no local PlayerID (are you in a game?)"); return; }
    std::vector<void*> boxes = boxes_sorted();
    if (boxes.empty()) { logf("lab[1]: no IngredientBoxGrab found"); return; }
    int idx = g.labBoxIdx.load();
    idx = ((idx % static_cast<int>(boxes.size())) + static_cast<int>(boxes.size())) % static_cast<int>(boxes.size());
    void* box = boxes[static_cast<size_t>(idx)];
    float p[3] = {};
    pos_of(box, p);
    logf("lab[1]: Interact on box #%d/%d '%s' at (%.2f, %.2f, %.2f)", idx, static_cast<int>(boxes.size()), uname(box).c_str(),
         static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]));
    bool ok = invoke_pid(m_boxInteract, box, pid);
    logf("lab[1]: call %s; held items now: %d", ok ? "ok" : "THREW an exception", static_cast<int>(held_items().size()));
    s_queue.push_back({ GetTickCount64() + 600, 10, nullptr });   // report after 0.6 s
}

void cmd_drop_grill() {
    std::vector<void*> held = held_items();
    std::vector<void*> zones = find_objects(k_zone);
    if (zones.empty()) { logf("lab[2]: no CookingZone"); return; }
    void* item = nullptr;
    for (void* h : held) if (component_of(h, k_meat)) { item = h; break; }
    if (!item) { logf("lab[2]: no raw meat in the inventory (take 'Raw ...' patty first); held=%d", static_cast<int>(held.size())); return; }
    float zp[3] = {};
    pos_of(zones[0], zp);
    float target[3] = { zp[0], zp[1] + 0.35f, zp[2] };
    throw_to(item, target, "lab[2]");
    s_queue.push_back({ GetTickCount64() + 1200, 11, zones[0] });
}

void cmd_toggle_grill() {
    std::vector<void*> zones = find_objects(k_zone);
    if (zones.empty()) { logf("lab[3]: no CookingZone"); return; }
    void* exc = nullptr;
    il2::runtime_invoke(m_zoneToggle, zones[0], nullptr, &exc);
    logf("lab[3]: CookingZone.Toggle() %s", exc ? "THREW" : "called");
}

void cmd_cooked_to_board() {
    unsigned char pid[16];
    if (!get_pid(pid)) { logf("lab[4]: no local PlayerID"); return; }
    std::vector<void*> zones = find_objects(k_zone);
    if (zones.empty()) { logf("lab[4]: no CookingZone"); return; }
    float zp[3] = {};
    pos_of(zones[0], zp);
    std::vector<void*> meats = find_objects(k_meat);
    void* pick = nullptr;
    for (void* m : meats) {
        float mp[3] = {};
        if (!pos_of(m, mp) || dist(mp, zp) > 9.0f) continue;      // within 3 m of the grill
        int st = meat_state(m);
        logf("lab[4]:   near grill: '%s' state=%d cookTime=%.1f (cooked at %.0f, burnt at %.0f)", uname(m).c_str(), st,
             static_cast<double>(o_cookTime >= 0 ? rd_flt(m, o_cookTime) : -1.0f),
             static_cast<double>(o_cookedTime >= 0 ? rd_flt(m, o_cookedTime) : -1.0f),
             static_cast<double>(o_burntTime >= 0 ? rd_flt(m, o_burntTime) : -1.0f));
        if (st == 1 && !pick) pick = m;
    }
    if (!pick) { logf("lab[4]: no COOKED patty near the grill"); return; }
    logf("lab[4]: picking up '%s' via Meat.Interact", uname(pick).c_str());
    bool ok = invoke_pid(m_meatInteract, pick, pid);
    logf("lab[4]: Interact %s", ok ? "ok" : "THREW");
    s_queue.push_back({ GetTickCount64() + 500, 12, pick });      // phase B
}

void phase_board(void* meat) {
    void* pl = local_player();
    float pp[3] = {};
    pos_of(pl, pp);
    std::vector<void*> boards = find_objects(k_board);
    void* best = nullptr;
    float bd = 1e30f;
    for (void* b : boards) {
        float bp[3] = {};
        if (!pos_of(b, bp)) continue;
        float d = dist(bp, pp);
        if (d < bd) { bd = d; best = b; }
    }
    if (!best) { logf("lab[4]: no AssemblyBoard found"); return; }
    float bp[3] = {};
    pos_of(best, bp);
    float target[3] = { bp[0], bp[1] + 0.425f, bp[2] };
    throw_to(meat, target, "lab[4]");
    s_queue.push_back({ GetTickCount64() + 1000, 13, meat });
}

void cmd_report() {
    unsigned char pid[16] = {};
    bool havePid = get_pid(pid);
    logf("lab[5]: PlayerID %s", havePid ? "found" : "NOT found");
    if (havePid) {
        char hex[40] = {};
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", pid[i]);
        logf("lab[5]: PlayerID bytes %s", hex);
    }
    std::vector<void*> held = held_items();
    logf("lab[5]: inventory items: %d", static_cast<int>(held.size()));
    for (void* h : held) logf("lab[5]:   held '%s'", uname(h).c_str());
    std::vector<void*> zones = find_objects(k_zone);
    if (!zones.empty()) {
        void* tl = o_tracked >= 0 ? rd_ptr(zones[0], o_tracked) : nullptr;
        logf("lab[5]: grill tracks %d item(s)", tl ? rd_int(tl, o_listSize) : -1);
    }
    for (void* m : find_objects(k_meat)) {
        float mp[3] = {};
        pos_of(m, mp);
        logf("lab[5]:   meat '%s' state=%d at (%.2f, %.2f, %.2f)", uname(m).c_str(), meat_state(m),
             static_cast<double>(mp[0]), static_cast<double>(mp[1]), static_cast<double>(mp[2]));
    }
}

// ============================================================================
//  COOK JOBS: every job is a small state machine ("list") that does what the
//  player does: press E on stations (Interact with the local PlayerID), throw
//  items into triggers (grill, fryer, oven, bag, drink machine), wait for the
//  game's own "cooked" state, then collect the result. Several jobs run at the
//  same time (many patties on the grill, a tray in each oven...).
//  Every action is written to the Spy console / hhbc_mod.log ("MOD >> ...").
// ============================================================================
struct Job {
    int id = 0, kind = 0, step = 0, n = 0, tries = 0, mask = 0;
    std::string label, param, status;
    ULONGLONG t = 0, next = 0, started = 0, finished = 0;
    void* item = nullptr; void* a = nullptr; void* b = nullptr;
    std::vector<void*> snap;
    float a2 = 0.0f;
    ULONGLONG t2 = 0, t3 = 0;
    int dest = -1;                        // patty: -1 = menu toggles, 0 = board, 1 = inventory
    void* board = nullptr;                // alpha / board jobs: the assembly board in use
    std::vector<int> kids;                // alpha: sub-jobs
    std::vector<std::string> plan;        // alpha: tasks
    std::vector<std::pair<int, std::string>> chain;   // alpha: (job id to wait for, bag param)
    int kid = 0;                          // alpha: job currently waited for
    int n2 = 0;
    bool burnt = false;                   // patty: wait until it is BURNT (backrooms recipes), not cooked
    bool hold = false;                    // patty: keep it cooked on the grill until the alpha plan asks for it
    bool done = false;
    float anc[3] = { 0.0f, 0.0f, 0.0f };   // where the player stood when the job was created: every machine / bag / board is chosen NEAREST TO THIS POINT
    void* orderKey = nullptr;              // alpha: accepted order (CustomerOrder object) that is being built
    int cfail[12] = {};                    // closeout: failures per task type
    int gateId = 0, gateN = 0;             // patty: do not start before alpha job gateId reaches plan task gateN (so patties do not burn while waiting)
};
enum { J_PATTY = 1, J_FRY, J_TRAY, J_BAG, J_SODA, J_SHAKE, J_INGR, J_SAUCE, J_ALPHA, J_CLOSE };
Job* jb(int id);

std::deque<Job> s_jobList;
std::mutex s_jobMtx;
std::vector<std::string> s_reqs, s_jobLines;
int s_nextJob = 1;
bool s_slotRes[2] = { false, false };       // oven slots reserved by tray jobs (0 top, 1 bottom)
int s_hand = 0;                             // job that currently owns the player's hand (only the ACTIVE item can be thrown)
ULONGLONG s_handT = 0;
ULONGLONG s_grillT = 0;                     // last grill Toggle() of any job (the game ignores toggles that come too fast)
const float* s_forceAnc = nullptr;           // alpha: its sub-jobs inherit its anchor
std::set<void*> s_handledOrders;              // accepted orders that alpha already builds
ULONGLONG s_bagT = 0;                       // last SpawnBag()

const char* const kDrinks[6] = { "Bump Cola", "Swimmers Choice", "Greap", "Jazzy Java", "Blob Aqua", "Agent Orange" };

std::map<std::string, void*> s_kc;
void* Kc(const char* n) {
    auto it = s_kc.find(n);
    if (it != s_kc.end()) return it->second;
    void* k = il2::find_class(il2::find_image("Assembly-CSharp.dll"), "", n);
    s_kc[n] = k;
    return k;
}
std::map<std::string, int> s_off;
int Of(const char* cls, const char* f) {
    std::string key = std::string(cls) + "." + f;
    auto it = s_off.find(key);
    if (it != s_off.end()) return it->second;
    void* k = Kc(cls);
    int o = k ? il2::field_off(k, f) : -1;
    if (o < 0) logf("cook: field %s not found", key.c_str());
    s_off[key] = o;
    return o;
}
std::map<std::string, void*> s_mt;
void* Mt(const char* cls, const char* name, int argc) {
    std::string key = std::string(cls) + "." + name + "/" + std::to_string(argc);
    auto it = s_mt.find(key);
    if (it != s_mt.end()) return it->second;
    void* k = Kc(cls);
    void* m = k ? il2::find_method(k, name, argc) : nullptr;
    if (!m) logf("cook: method %s not found", key.c_str());
    s_mt[key] = m;
    return m;
}
void* MtPid(const char* cls, const char* name) {
    std::string key = std::string(cls) + "." + name + "/pid";
    auto it = s_mt.find(key);
    if (it != s_mt.end()) return it->second;
    const char* pp[] = { "PurrNet.PlayerID" };
    void* k = Kc(cls);
    void* m = k ? il2::find_method_sig(k, name, pp, 1) : nullptr;
    if (!m) logf("cook: method %s(PlayerID) not found", key.c_str());
    s_mt[key] = m;
    return m;
}

std::string clean(std::string s) {
    size_t p = s.find("(Clone)");
    if (p != std::string::npos) s.erase(p);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
std::string low(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; }
bool has(const std::string& hay, const char* needle) { return low(hay).find(low(needle)) != std::string::npos; }

void* call0(void* m, void* obj) {
    if (!m || !obj) return nullptr;
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m, obj, nullptr, &exc);
    return exc ? nullptr : r;
}
bool call_int(void* m, void* obj, int v, void** ret = nullptr) {
    if (!m || !obj) return false;
    void* a[] = { &v };
    void* exc = nullptr;
    void* r = il2::runtime_invoke(m, obj, a, &exc);
    if (ret) *ret = r;
    return exc == nullptr;
}
bool interact(const char* cls, void* obj) {
    unsigned char pid[16];
    if (!obj || !get_pid(pid)) return false;
    void* m = MtPid(cls, "Interact");
    return m && invoke_pid(m, obj, pid);
}

// PurrNet.SyncVar<T>: read the stored value (field "_value"; falls back to a dump of the fields)
bool sv_read(void* sv, void* out, int size) {
    if (!sv) return false;
    static std::map<void*, int> cache;
    void* kl = il2::object_get_class(sv);
    auto it = cache.find(kl);
    int off = -1;
    if (it != cache.end()) off = it->second;
    else {
        for (void* kk = kl; kk && off < 0; kk = il2::class_get_parent(kk)) {
            void* iter = nullptr;
            while (void* f = il2::class_get_fields(kk, &iter)) {
                if (il2::field_get_flags(f) & 0x10) continue;
                const char* n = il2::field_get_name(f);
                if (n && (strcmp(n, "_value") == 0 || strcmp(n, "value") == 0)) { off = static_cast<int>(il2::field_get_offset(f)); break; }
            }
        }
        if (off < 0) {
            logf("cook: SyncVar value field not found in '%s'; fields:", il2::class_get_name(kl));
            for (void* kk = kl; kk; kk = il2::class_get_parent(kk)) {
                void* iter = nullptr;
                while (void* f = il2::class_get_fields(kk, &iter))
                    logf("cook:    %s.%s @0x%x flags=%x", il2::class_get_name(kk), il2::field_get_name(f), static_cast<unsigned>(il2::field_get_offset(f)), il2::field_get_flags(f));
            }
        } else logf("cook: SyncVar value offset 0x%x in '%s'", off, il2::class_get_name(kl));
        cache[kl] = off;
    }
    if (off < 0) return false;
    memcpy(out, static_cast<char*>(sv) + off, static_cast<size_t>(size));
    return true;
}
int sv_bool(void* owner, int off) {            // -1 unknown, 0, 1
    if (!owner || off < 0) return -1;
    unsigned char v = 0;
    return sv_read(rd_ptr(owner, off), &v, 1) ? (v ? 1 : 0) : -1;
}
int sv_int(void* owner, int off, int def = -1) {
    if (!owner || off < 0) return def;
    int v = 0;
    return sv_read(rd_ptr(owner, off), &v, 4) ? v : def;
}
float sv_float(void* owner, int off, float def = -1.0f) {
    if (!owner || off < 0) return def;
    float v = 0;
    return sv_read(rd_ptr(owner, off), &v, 4) ? v : def;
}

int free_slot() {
    void* pl = local_player();
    void* m = Mt("ScytheNetworkPlayer", "FindEmptySlot", 0);
    void* r = call0(m, pl);
    return r ? *reinterpret_cast<int*>(static_cast<char*>(r) + 0x10) : -1;
}
void* new_held(const std::vector<void*>& snap) {
    for (void* h : held_items()) if (std::find(snap.begin(), snap.end(), h) == snap.end()) return h;
    return nullptr;
}
bool in_held(void* x) { for (void* h : held_items()) if (h == x) return true; return false; }
bool is_gone(void* o) { return !o || uname(o).empty(); }

void* nearest(void* klass, const float p[3]) {
    void* best = nullptr; float bd = 1e30f;
    for (void* o : find_objects(klass)) {
        float q[3] = {};
        if (!pos_of(o, q)) continue;
        float d = dist(q, p);
        if (d < bd) { bd = d; best = o; }
    }
    return best;
}
void player_pos(float p[3]) { p[0] = p[1] = p[2] = 0; pos_of(local_player(), p); }
void ancp(const Job& j, float p[3]) { p[0] = j.anc[0]; p[1] = j.anc[1]; p[2] = j.anc[2]; }
void* nearest_of(const std::vector<void*>& v, const float p[3]) {
    void* best = nullptr; float bd = 1e30f;
    for (void* o : v) { float q[3] = {}; float d = pos_of(o, q) ? dist(q, p) : 1e20f; if (d < bd) { bd = d; best = o; } }
    return best;
}
// nearest object of a class to a point; if needId, only objects whose int field at offId equals want
void* near_obj_p(const char* cls, const float rp[3], int offId, int want, bool needId) {
    if (needId && offId < 0) return nullptr;
    void* best = nullptr; float bd = 1e30f;
    for (void* o : find_objects(Kc(cls))) {
        if (needId && rd_int(o, offId) != want) continue;
        float q[3] = {}; float d = pos_of(o, q) ? dist(q, rp) : 1e20f;
        if (d < bd) { bd = d; best = o; }
    }
    return best;
}
void* near_obj(const char* cls, void* ref, int offId, int want, bool needId = true) {
    float rp[3] = {}; pos_of(ref, rp);
    return near_obj_p(cls, rp, offId, want, needId);
}

void* pick_box(const char* t1, const char* t2 = nullptr, const char* t3 = nullptr, const float* anc = nullptr) {
    void* best = nullptr; float bd = 1e30f;
    for (void* b : boxes_sorted()) {
        void* ts = o_toSpawn >= 0 ? rd_ptr(b, o_toSpawn) : nullptr;
        std::string sp = clean(uname(ts));
        if (sp.empty()) continue;
        if (t1 && !has(sp, t1)) continue;
        if (t2 && !has(sp, t2)) continue;
        if (t3 && !has(sp, t3)) continue;
        if (!anc) return b;
        float q[3] = {}; float d = pos_of(b, q) ? dist(q, anc) : 1e20f;
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}

void hand_release_id(int id) { if (s_hand == id) s_hand = 0; }
void player_pos(float p[3]);
void fail(Job& j, const std::string& why) {
    if (j.kind == J_PATTY && j.item && j.step < 5 && in_held(j.item)) {   // raw patty still in the hotbar: put it down next to the player
        float pp[3]; player_pos(pp);
        float t[3] = { pp[0], pp[1] + 1.0f, pp[2] };
        spy_note("job #%d: cleaning up - the patty is put down next to you (it must not stay in the inventory)", j.id);
        throw_to(j.item, t, "job");
    }
    if (j.kind == J_BAG && j.item && in_held(j.item)) {                    // a wrong item must not stay in the hotbar: put it down next to the player
        float pp[3]; player_pos(pp);
        float t[3] = { pp[0], pp[1] + 1.0f, pp[2] };
        spy_note("job #%d: cleaning up - '%s' is put down next to you", j.id, uname(j.item).c_str());
        throw_to(j.item, t, "job");
    }
    hand_release_id(j.id);
    if (j.kind == J_ALPHA && j.orderKey) s_handledOrders.erase(j.orderKey);   // the order can be started again
    j.done = true; j.finished = GetTickCount64(); j.status = "FAILED: " + why;
    spy_note("job #%d '%s' FAILED: %s", j.id, j.label.c_str(), why.c_str());
    if (j.kind == J_TRAY && j.n >= 0 && j.n < 2 && j.mask) s_slotRes[j.n] = false;
}
void finish(Job& j, const std::string& what) {
    hand_release_id(j.id);
    j.done = true; j.finished = GetTickCount64(); j.status = "DONE: " + what;
    spy_note("job #%d '%s' DONE: %s (%.1f s)", j.id, j.label.c_str(), what.c_str(), static_cast<double>(j.finished - j.started) / 1000.0);
    if (j.kind == J_TRAY && j.n >= 0 && j.n < 2 && j.mask) s_slotRes[j.n] = false;
}

bool throw_above(Job& j, void* item, void* target, float up, int tries) {
    float p[3] = {};
    if (!pos_of(target, p)) { fail(j, "cannot read target position"); return false; }
    float t[3];
    if (std::string(il2::class_get_name(il2::object_get_class(target))) == "AssemblyBoard") {
        // board: tiny shifts (1-3 cm) left / right / forward / back on every retry, so the item lands on another spot of the stack
        static const float dx[6] = { 0.0f, 0.015f, -0.02f, 0.01f, -0.03f, 0.025f };
        static const float dz[6] = { 0.0f, 0.0f, 0.01f, -0.03f, -0.01f, 0.02f };
        int k = tries < 0 ? 0 : tries % 6;
        t[0] = p[0] + dx[k]; t[1] = p[1] + up; t[2] = p[2] + dz[k];
    } else {
        float jit = 0.10f * static_cast<float>(tries);
        t[0] = p[0] + ((tries % 2) ? jit : -jit); t[1] = p[1] + up; t[2] = p[2] + ((tries % 3) ? jit : 0.0f);
    }
    spy_note("job #%d: throw '%s' above %s at (%.2f, %.2f, %.2f)", j.id, uname(item).c_str(), il2::class_get_name(il2::object_get_class(target)),
             static_cast<double>(t[0]), static_cast<double>(t[1]), static_cast<double>(t[2]));
    throw_to(item, t, "job");
    return true;
}

bool hand_acquire(Job& j, ULONGLONG now) {
    if (s_hand == j.id) return true;
    bool freeNow = s_hand == 0 || now - s_handT > 20000;
    if (!freeNow) {
        freeNow = true;
        for (const Job& o : s_jobList) if (o.id == s_hand && !o.done) { freeNow = false; break; }
    }
    if (!freeNow) { j.status = "waiting: another list is using the hand"; return false; }
    s_hand = j.id; s_handT = now;
    return true;
}
void hand_release(Job& j) { hand_release_id(j.id); }

// -- take something from a box / source and recognise the new item in the hand
bool wait_slot(Job& j, ULONGLONG now) {
    if (free_slot() >= 0) return true;
    if (now - j.started > 30000) { fail(j, "no free hotbar slot for 30 s"); return false; }
    j.status = "waiting for a free hotbar slot";
    return false;
}
void identify_held(Job& j, ULONGLONG now, int nextStep) {
    if (now < j.t) return;
    void* it = new_held(j.snap);
    if (!it) { if (now - j.t > 4500) fail(j, "the item did not appear in the hand"); return; }
    j.item = it;
    spy_note("job #%d: got '%s' in the hand", j.id, uname(it).c_str());
    j.step = nextStep;
}

std::string norm(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') o += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}
int drink_index(const std::string& name) {
    std::string n = norm(name);
    for (int i = 0; i < 6; i++) if (norm(kDrinks[i]) == n) return i;
    return -1;
}
int shake_kind(const std::string& name) {          // 1 = strawberry, 2 = blueberry, 0 = not a shake
    std::string n = norm(name);
    if (n.find("strawberry") != std::string::npos || n.find("jacuzzi") != std::string::npos) return 1;
    if (n.find("junglecola") != std::string::npos || n.find("freeze") != std::string::npos || n.find("blueberry") != std::string::npos) return 2;
    return 0;
}
void* pick_cup(const char* tok, const float* anc = nullptr) {
    int off = Of("ScytheCupDispenser", "toSpawn");
    float pp[3]; pp[0] = pp[1] = pp[2] = 0;
    if (anc) { pp[0] = anc[0]; pp[1] = anc[1]; pp[2] = anc[2]; } else pos_of(local_player(), pp);
    void* best = nullptr; float bd = 1e30f;
    for (void* d : find_objects(Kc("ScytheCupDispenser"))) {
        void* ts = off >= 0 ? rd_ptr(d, off) : nullptr;
        std::string sp = ts ? clean(uname(ts)) : std::string();
        if (sp.empty() ? !has(uname(d), tok) : !has(sp, tok)) continue;
        float q[3] = {};
        float dd = pos_of(d, q) ? dist(q, pp) : 1e20f;
        if (dd < bd) { bd = dd; best = d; }
    }
    return best;
}
// machine nearest to the player and the trigger that belongs to it
bool pair_machine(const char* mc, const char* tc, void*& m, void*& t, const float* anc = nullptr) {
    float pp[3]; pp[0] = pp[1] = pp[2] = 0;
    if (anc) { pp[0] = anc[0]; pp[1] = anc[1]; pp[2] = anc[2]; } else pos_of(local_player(), pp);
    m = nearest(Kc(mc), pp);
    t = nullptr;
    if (!m) return false;
    int fo = Of(tc, "machine");
    for (void* x : find_objects(Kc(tc))) if (fo >= 0 && rd_ptr(x, fo) == m) { t = x; break; }
    if (!t) { float mp[3] = {}; pos_of(m, mp); t = nearest(Kc(tc), mp); }
    return t != nullptr;
}

int board_count(void* board);

// ------------------------------- PATTY ---------------------------------------
int grill_state(void* zone) { return sv_bool(zone, Of("CookingZone", "grillState")); }
bool zone_tracks(void* zone, void* meat) {
    void* tl = o_tracked >= 0 ? rd_ptr(zone, o_tracked) : nullptr;
    if (!tl) return false;
    int n = rd_int(tl, o_listSize);
    void* arr = rd_ptr(tl, o_listItems);
    for (int i = 0; arr && i < n && i < 64; i++)
        if (*reinterpret_cast<void**>(static_cast<char*>(arr) + 0x20 + 8 * i) == meat) return true;
    return false;
}
void step_patty(Job& j, ULONGLONG now) {
    unsigned char pid[16];
    switch (j.step) {
    case 0: {
        if (j.gateId) {                                // alpha: a patty of a LATER burger starts only when the previous burger has its patties (else it burns on the grill)
            Job* ga = jb(j.gateId);
            if (ga && !ga->done && ga->n < j.gateN) { j.status = "waiting for the previous burger"; return; }
            j.gateId = 0; j.started = now;
        }
        if (!wait_slot(j, now)) return;
        {
            float pp0[3]; ancp(j, pp0);
            void* z0 = nearest(k_zone, pp0);
            void* tl = (z0 && o_tracked >= 0) ? rd_ptr(z0, o_tracked) : nullptr;
            if (tl && rd_int(tl, o_listSize) >= 6) { j.status = "grill is full - waiting"; return; }
        }
        if (!hand_acquire(j, now)) return;
        void* box = nullptr; float bdist = 1e30f;                 // the box NEAREST to where the job was started
        for (void* b : boxes_sorted()) {
            void* ts = o_toSpawn >= 0 ? rd_ptr(b, o_toSpawn) : nullptr;
            if (low(clean(uname(ts))) != low(j.param)) continue;
            float q[3] = {}; float d = pos_of(b, q) ? dist(q, j.anc) : 1e20f;
            if (d < bdist) { bdist = d; box = b; }
        }
        if (!box) { fail(j, "no ingredient box spawns '" + j.param + "'"); return; }
        if (!get_pid(pid)) { fail(j, "no local PlayerID"); return; }
        j.snap = held_items();
        spy_note("job #%d: take '%s' from box '%s'", j.id, j.param.c_str(), uname(box).c_str());
        invoke_pid(m_boxInteract, box, pid);
        j.t = now + 250; j.step = 1; j.status = "taking the patty";
        return;
    }
    case 1: identify_held(j, now, 2); return;
    case 2: {
        if (now < j.t) return;
        float pp[3]; ancp(j, pp);
        void* zone = nearest(k_zone, pp);
        if (!zone) { fail(j, "no grill (CookingZone)"); return; }
        int gs = grill_state(zone);
        if (gs == 0) {
            if (now - s_grillT > 2500) {
                call0(m_zoneToggle, zone);
                s_grillT = now;
                spy_note("job #%d: grill was OFF -> Toggle()", j.id);
            }
            j.t = now + 900; j.status = "switching the grill on";
            return;                                    // re-check next time
        }
        j.a = zone;
        j.b = component_of(j.item, k_meat);
        int slot = 0;
        {
            void* tl = o_tracked >= 0 ? rd_ptr(zone, o_tracked) : nullptr;
            slot = tl ? rd_int(tl, o_listSize) : 0;
            if (slot > 5) slot = 5;
        }
        float zp[3] = {};
        pos_of(zone, zp);
        float t[3] = { zp[0] + (static_cast<float>(slot % 3) - 1.0f) * 0.30f, zp[1] + 0.35f, zp[2] + (static_cast<float>(slot / 3) - 0.5f) * 0.30f };
        spy_note("job #%d: throw patty on the grill (spot %d)", j.id, slot);
        throw_to(j.item, t, "job");
        j.t = now + 800; j.step = 3; j.status = "placed on the grill";
        return;
    }
    case 20: {                                         // burnt patty: take it off the grill
        if (!hand_acquire(j, now)) return;
        if (!in_held(j.item)) {
            if (free_slot() < 0) { hand_release(j); j.status = "burnt patty - waiting for a free slot"; return; }
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.item };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
        }
        j.t = now + 700; j.step = 21; j.status = "removing the burnt patty";
        return;
    }
    case 21: {                                         // ... and throw it into the trash
        if (now < j.t) return;
        void* tz = nearest(Kc("TrashDisposalZone"), j.anc);
        if (tz) throw_above(j, j.item, tz, 0.4f, 0);
        else { float pp[3]; player_pos(pp); float t[3] = { pp[0], pp[1] + 1.0f, pp[2] }; throw_to(j.item, t, "job"); }
        fail(j, "patty burnt while waiting (thrown away)");
        return;
    }
    case 3: {
        if (now < j.t) return;
        bool tracked = j.b && zone_tracks(j.a, j.b);
        if (in_held(j.item)) {                        // lies on the grill but still occupies a hotbar slot ("phantom")
            spy_note("job #%d: the patty still occupies a hotbar slot -> removing it from the inventory", j.id);
            void* gen = component_of(j.item, k_generic);
            hotbar_remove(gen ? gen : j.item, "job");
        }
        bool stillHeld = in_held(j.item);
        if (tracked && !stillHeld) { spy_note("job #%d: grill tracks the patty, inventory is free", j.id); hand_release(j); j.step = 4; j.tries = 0; j.t2 = 0; j.t3 = 0; j.status = "cooking"; return; }
        if (j.tries++ < 5) {
            float zp[3] = {};
            pos_of(j.a, zp);
            float t[3] = { zp[0] + 0.1f * static_cast<float>(j.tries), zp[1] + 0.35f, zp[2] };
            spy_note("job #%d: %s, throwing again (%d)", j.id, tracked ? "patty is on the grill but not released" : "grill did not take the patty", j.tries);
            throw_to(j.item, t, "job");
            j.t = now + 1300;
        } else fail(j, "grill did not accept the patty");
        return;
    }
    case 4: {
        int st = j.b ? meat_state(j.b) : -1;
        float ct = (j.b && o_cookTime >= 0) ? rd_flt(j.b, o_cookTime) : 0.0f;
        float need = (j.b && o_cookedTime >= 0) ? rd_flt(j.b, o_cookedTime) : 0.0f;
        char b[96];
        snprintf(b, sizeof(b), "cooking %.1f / %.0f s", static_cast<double>(ct), static_cast<double>(need));
        j.status = b;
        if (j.burnt) {
            if (st == 2) { spy_note("job #%d: patty is BURNT (wanted)", j.id); j.step = 5; j.tries = 0; return; }
            if (st == 1) j.status = "cooked - waiting until it burns";
        } else {
            if (st == 1) { spy_note("job #%d: patty is COOKED", j.id); j.step = 5; j.tries = 0; return; }
            if (st == 2) { fail(j, "patty burnt"); return; }
        }
        {   // the patty fell off / was pulled back to the hand by something else: place it again (max 3 times)
            bool tracked = j.b && zone_tracks(j.a, j.b);
            if (tracked) j.t2 = 0;
            else if (!j.t2) j.t2 = now;
            else if (now - j.t2 > 2500 && j.n < 3) {
                if (hand_acquire(j, now)) {
                    j.n++; j.t2 = 0;
                    spy_note("job #%d: the grill lost the patty -> placing it again (%d)", j.id, j.n);
                    j.step = 2; j.tries = 0; j.t = now;
                    return;
                }
            }
        }
        {   // the game may switch the grill off by itself (ForceGrillOff): switch it on again, with a cooldown
            if (grill_state(j.a) == 0) {
                if (!j.t3) j.t3 = now;
                else if (now - j.t3 > 3000 && now - s_grillT > 8000) {
                    call0(m_zoneToggle, j.a); s_grillT = now; j.t3 = 0;
                    spy_note("job #%d: the grill is OFF -> Toggle()", j.id);
                }
            } else j.t3 = 0;
        }
        if (now - j.started > 240000) fail(j, "timeout while cooking");
        return;
    }
    case 5: {
        int dst = j.dest >= 0 ? j.dest : (g.dstInv.load() ? 1 : (g.dstBoard.load() ? 0 : -1));
        if (dst == 1) {
            if (!hand_acquire(j, now)) return;
            if (free_slot() < 0) { hand_release(j); j.status = "cooked - waiting for a free slot"; if (now - j.t > 60000) fail(j, "no free slot to collect the patty (left on the grill)"); return; }
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.item };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("job #%d: collect patty -> HotbarGrab %s", j.id, exc ? "threw" : "called");
            j.t = now + 800; j.step = 6; j.n = 0; j.status = "collecting";
        } else if (dst == 0) {
            if (j.hold) {
                j.status = "cooked - waiting for its turn in the order";
                if (!j.burnt && j.b && meat_state(j.b) == 2) { spy_note("job #%d: the patty BURNT while it waited - it is thrown away and a new one is cooked", j.id); j.step = 20; j.tries = 0; }
                return;
            }
            void* board = j.board ? j.board : nearest(k_board, j.anc);
            if (!board) { fail(j, "no assembly board"); return; }
            // A patty that lies on the grill cannot be thrown (LocalDrop only lets go of a HELD item), so take it into
            // the hand first and throw it from there - exactly what a player does.
            if (!hand_acquire(j, now)) return;
            if (free_slot() < 0) { hand_release(j); j.status = "cooked - waiting for a free slot"; if (now - j.t > 60000) fail(j, "no free slot to collect the patty (left on the grill)"); return; }
            j.board = board;
            j.mask = board_count(board);                  // ingredients on the board before the throw
            j.tries = 0; j.n2 = 1;                        // n2 = 1: after collecting, throw it onto the board
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.item };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("job #%d: collect the cooked patty -> HotbarGrab %s, then throw it onto the board", j.id, exc ? "threw" : "called");
            j.t = now + 350; j.step = 6; j.n = 0; j.status = "collecting";
        } else finish(j, "cooked, left on the grill (both destinations are off)");
        return;
    }
    case 6: {
        if (now < j.t) return;
        if (in_held(j.item)) {
            if (j.n2 == 1) {                              // destination = assembly board: it is in the hand now -> throw it
                throw_above(j, j.item, j.board, 0.425f, 0);
                hand_release(j);
                j.t = now + 400; j.step = 7; j.tries = 0; j.status = "thrown to the board";
                return;
            }
            finish(j, "patty is in the inventory");
            return;
        }
        if (j.n2 == 1 && (is_gone(j.item) || board_count(j.board) > j.mask)) { finish(j, "the patty is on the assembly board"); return; }
        if (j.n == 0) {
            if (get_pid(pid) && m_meatInteract && j.b) invoke_pid(m_meatInteract, j.b, pid);
            spy_note("job #%d: HotbarGrab did not put it in the hand -> Meat.Interact", j.id);
            j.n = 1; j.t = now + 450;
        } else if (is_gone(j.item)) finish(j, "the patty was used by something else (board / bag)");
        else fail(j, "could not take the cooked patty (the hand is busy or the patty was taken - see Spy lines above)");
        return;
    }
    case 7: {                                          // watch the board every tick; re-throw only after ~1.1 s without effect
        int cnt = board_count(j.board);
        if (is_gone(j.item) || (cnt >= 0 && cnt > j.mask)) { finish(j, "the patty is on the assembly board"); return; }
        if (now < j.t) return;
        if (j.tries++ < 3) {
            spy_note("job #%d: the board did not take the patty yet, throwing again (%d)", j.id, j.tries);
            throw_above(j, j.item, j.board, 0.425f, j.tries);
            j.t = now + 1100;
            return;
        }
        finish(j, "thrown above the board, but the board did not report it - check the board (the patty is on the counter)");
        return;
    }
    }
}

// --------------------------------- FRYER -------------------------------------
int basket_filled(void* fryer, int i) { return sv_bool(fryer, Of("Fryer", i == 0 ? "basket1Filled" : "basket2Filled")); }
int basket_dunked(void* fryer, int i) { return sv_bool(fryer, Of("Fryer", i == 0 ? "basket1Dunked" : "basket2Dunked")); }
void step_fry(Job& j, ULONGLONG now) {
    switch (j.step) {
    case 0: {
        if (!wait_slot(j, now)) return;
        {
            void* f0 = nearest_of(find_objects(Kc("Fryer")), j.anc);
            if (f0 && basket_filled(f0, 0) > 0 && basket_filled(f0, 1) > 0) {
                j.status = "both baskets are busy - waiting";
                if (now - j.started > 120000) fail(j, "both baskets stay busy");
                return;
            }
        }
        void* box = j.param == "nuggets" ? pick_box("nug", nullptr, nullptr, j.anc) : pick_box("fries", nullptr, nullptr, j.anc);
        if (!box) { fail(j, "no ingredient box for " + j.param); return; }
        if (!hand_acquire(j, now)) return;
        j.snap = held_items();
        spy_note("job #%d: take %s bag from '%s'", j.id, j.param.c_str(), uname(box).c_str());
        unsigned char pid[16];
        if (!get_pid(pid)) { fail(j, "no local PlayerID"); return; }
        invoke_pid(m_boxInteract, box, pid);
        j.t = now + 400; j.step = 1; j.status = "taking the bag";
        return;
    }
    case 1: identify_held(j, now, 2); return;
    case 2: {
        std::vector<void*> fr = find_objects(Kc("Fryer"));
        if (fr.empty()) { fail(j, "no Fryer"); return; }
        j.a = nearest_of(fr, j.anc);
        int f0 = basket_filled(j.a, 0), f1 = basket_filled(j.a, 1);
        j.mask = (f0 > 0 ? 1 : 0) | (f1 > 0 ? 2 : 0);
        if (j.mask == 3) { j.status = "both baskets are busy - waiting"; if (now - j.started > 90000) fail(j, "both baskets stay busy"); return; }
        int want = (j.mask & 1) ? 1 : 0;                       // trigger of a free basket
        void* trig = nullptr;
        int tid = Of("FryerTrigger", "triggerID");
        trig = near_obj("FryerTrigger", j.a, tid, want);
        if (!trig) { fail(j, "no FryerTrigger for the free basket"); return; }
        j.b = trig;
        throw_above(j, j.item, trig, 0.35f, 0);
        j.t = now + 1400; j.step = 3; j.status = "bag thrown into the fryer";
        return;
    }
    case 3: {
        if (now < j.t) return;
        int f0 = basket_filled(j.a, 0), f1 = basket_filled(j.a, 1);
        int mask = (f0 > 0 ? 1 : 0) | (f1 > 0 ? 2 : 0);
        if (mask != j.mask) { j.n = (mask & ~j.mask) & 1 ? 0 : 1; spy_note("job #%d: basket %d loaded", j.id, j.n + 1); hand_release(j); j.step = 4; j.tries = 0; j.t = now; return; }
        if (j.tries++ < 3) { throw_above(j, j.item, j.b, 0.35f, j.tries); j.t = now + 1400; }
        else fail(j, "fryer did not take the bag");
        return;
    }
    case 4: {
        if (now < j.t) return;
        int st = Of("Fryer", "temperatureState") >= 0 ? rd_int(j.a, Of("Fryer", "temperatureState")) : -1;
        if (st >= 2) { j.step = 5; j.tries = 0; return; }              // Medium or Hot
        if (j.tries++ >= 7) { fail(j, "could not heat the oil"); return; }
        void* ctl = nullptr;
        ctl = near_obj("FryerControl", j.a, -1, 0, false);
        interact("FryerControl", ctl);
        spy_note("job #%d: oil state %d -> press the fryer knob", j.id, st);
        j.t = now + 1100; j.status = "heating the oil";
        return;
    }
    case 5: {
        void* basket = nullptr;
        int fid = Of("FryerBasket", "fryerID");
        basket = near_obj("FryerBasket", j.a, fid, j.n);
        if (!basket) { fail(j, "no FryerBasket #" + std::to_string(j.n)); return; }
        j.b = basket;
        interact("FryerBasket", basket);
        spy_note("job #%d: dunk basket %d", j.id, j.n + 1);
        j.t = now + 1000; j.step = 6; j.status = "dunking the basket";
        return;
    }
    case 6: {
        if (now < j.t) return;
        if (basket_dunked(j.a, j.n) == 1) { j.step = 7; j.tries = 0; return; }
        if (j.tries++ < 2) { interact("FryerBasket", j.b); j.t = now + 1000; }
        else fail(j, "basket did not go down");
        return;
    }
    case 7: {
        float fry = sv_float(j.a, Of("Fryer", j.n == 0 ? "fryValueBasket1" : "fryValueBasket2"));
        float cooked = Of("Fryer", "cookedValue") >= 0 ? rd_flt(j.a, Of("Fryer", "cookedValue")) : 1.0f;
        char b[96];
        snprintf(b, sizeof(b), "frying %.2f / %.2f", static_cast<double>(fry), static_cast<double>(cooked));
        j.status = b;
        if (basket_dunked(j.a, j.n) == 0 && now - j.t > 3000) { spy_note("job #%d: basket is already up -> retrieve the food", j.id); j.step = 10; j.tries = 0; j.t = now; return; }
        if (fry >= cooked && fry >= 0) { spy_note("job #%d: food is cooked", j.id); j.step = 8; j.tries = 0; return; }
        if (now - j.started > 180000) fail(j, "timeout while frying");
        return;
    }
    case 8: {
        if (basket_dunked(j.a, j.n) == 0) { j.step = 10; j.tries = 0; j.t = now; return; }
        interact("FryerBasket", j.b);
        spy_note("job #%d: lift basket %d", j.id, j.n + 1);
        j.t = now + 1000; j.step = 9; j.status = "lifting the basket";
        return;
    }
    case 9: {
        if (now < j.t) return;
        if (basket_dunked(j.a, j.n) == 0) { j.step = 10; j.tries = 0; j.t = now; return; }
        if (j.tries++ < 2) { interact("FryerBasket", j.b); j.t = now + 1000; }
        else fail(j, "basket did not come up");
        return;
    }
    case 10: {                                         // basket is up: a SECOND press empties it into the dump station (RetrieveBasketInternal)
        if (now < j.t) return;
        if (basket_filled(j.a, j.n) == 0) { finish(j, j.param + " are in the dump station (use the Bag buttons)"); return; }
        if (basket_dunked(j.a, j.n) == 1) { interact("FryerBasket", j.b); j.t = now + 1000; return; }   // it went down again: lift it
        if (j.tries++ >= 2) { finish(j, j.param + ": basket emptied (assumed - the 'filled' flag did not clear)"); return; }
        interact("FryerBasket", j.b);
        spy_note("job #%d: retrieve the food from basket %d", j.id, j.n + 1);
        j.t = now + 1200; j.status = "emptying the basket";
        return;
    }
    }
}

// ---------------------------------- OVEN -------------------------------------
int tray_type(void* oven, int slot) { return sv_int(oven, Of("Oven", slot == 0 ? "topTrayType" : "bottomTrayType")); }
int door_open(void* oven, int slot) { return sv_bool(oven, Of("Oven", slot == 0 ? "topOvenDoorOpen" : "bottomOvenDoorOpen")); }
int oven_temp(void* oven, int slot) { return sv_int(oven, Of("Oven", slot == 0 ? "topOvenTempState" : "bottomOvenTempState")); }
void door_set(Job& j, void* oven, int slot, int wantOpen) {
    int cur = door_open(oven, slot);
    if (cur < 0 || cur == wantOpen) return;
    call0(Mt("Oven", slot == 0 ? "ToggleTopOvenDoor" : "ToggleBottomOvenDoor", 0), oven);
    spy_note("job #%d: oven %s door -> %s", j.id, slot == 0 ? "top" : "bottom", wantOpen ? "open" : "closed");
}
void step_tray(Job& j, ULONGLONG now) {          // j.param = "cookie" | "pie"; j.n = slot; j.mask = slot reserved
    switch (j.step) {
    case 0: {                                          // choose a free oven slot first, open its door
        if (now < j.t) return;
        std::vector<void*> ov = find_objects(Kc("Oven"));
        if (ov.empty()) { fail(j, "no Oven"); return; }
        j.a = nearest_of(ov, j.anc);
        int slot = -1;
        for (int s : { 1, 0 }) if (!s_slotRes[s] && tray_type(j.a, s) == 0) { slot = s; break; }
        if (slot < 0) { j.status = "both oven slots are busy - waiting"; if (now - j.started > 150000) fail(j, "no free oven slot"); return; }
        s_slotRes[slot] = true; j.n = slot; j.mask = 1;
        door_set(j, j.a, slot, 1);
        j.t = now + 1000; j.step = 1; j.status = "opening the oven";
        return;
    }
    case 1: {                                          // take the tray and throw it at once (only the active item can be thrown)
        if (now < j.t) return;
        for (const Job& o : s_jobList) if (&o != &j && !o.done && o.kind == J_TRAY && o.id < j.id && o.step >= 1 && o.step <= 4) { j.status = "waiting for the other tray to be loaded"; return; }
        if (!wait_slot(j, now)) return;
        void* box = pick_box("tray", j.param.c_str(), nullptr, j.anc);
        if (!box) { fail(j, "no ingredient box with a " + j.param + " tray"); return; }
        if (!hand_acquire(j, now)) return;
        unsigned char pid[16];
        if (!get_pid(pid)) { fail(j, "no local PlayerID"); return; }
        j.snap = held_items();
        spy_note("job #%d: take %s tray from '%s'", j.id, j.param.c_str(), uname(box).c_str());
        invoke_pid(m_boxInteract, box, pid);
        j.t = now + 400; j.step = 2; j.status = "taking the tray";
        return;
    }
    case 2: identify_held(j, now, 3); return;
    case 3: {
        void* trig = nullptr;
        int fo = Of("OvenTrigger", "isTopOven");
        {
            float op[3] = {}; pos_of(j.a, op); float bd = 1e30f;
            for (void* t : find_objects(Kc("OvenTrigger"))) {
                if (fo < 0 || (*(static_cast<char*>(t) + fo) != 0) != (j.n == 0)) continue;
                float q[3] = {}; float d = pos_of(t, q) ? dist(q, op) : 1e20f;
                if (d < bd) { bd = d; trig = t; }
            }
        }
        if (!trig) { fail(j, "no OvenTrigger for this slot"); return; }
        j.b = trig;
        door_set(j, j.a, j.n, 1);
        // the bottom trigger sits right under the top one: drop at its centre, not above it
        // the log showed the top slot missing at +0.35 above its trigger (6 misses in a row): aim lower, and vary the height on retries
        static const float kTopUp[6] = { 0.10f, 0.0f, 0.18f, -0.06f, 0.05f, 0.14f };
        throw_above(j, j.item, trig, j.n == 0 ? kTopUp[j.tries % 6] : 0.0f, j.tries);
        j.t = now + 1600; j.step = 4; j.status = "tray thrown into the oven";
        return;
    }
    case 4: {
        if (now < j.t) return;
        if (tray_type(j.a, j.n) > 0) { spy_note("job #%d: oven slot %d loaded", j.id, j.n); hand_release(j); door_set(j, j.a, j.n, 0); j.t = now + 1000; j.step = 5; j.tries = 0; return; }
        if (++j.tries < 6) { j.step = 3; return; }
        door_set(j, j.a, j.n, 0);
        fail(j, "oven did not take the tray");
        return;
    }
    case 5: {
        if (now < j.t) return;
        int st = oven_temp(j.a, j.n);
        if (st >= 2) { j.step = 6; j.tries = 0; return; }
        if (j.tries++ >= 7) { fail(j, "could not set the oven temperature"); return; }
        call0(Mt("Oven", j.n == 0 ? "TopKnobControl" : "BottomKnobControl", 0), j.a);
        spy_note("job #%d: oven dial state %d -> turn", j.id, st);
        j.t = now + 900; j.status = "setting the temperature";
        return;
    }
    case 6: {
        void* fake = nullptr;
        { void* r = nullptr; if (call_int(Mt("Oven", "GetActiveFakeTray", 1), j.a, j.n, &r)) fake = r; }
        j.b = fake;
        if (!fake) { j.status = "baking (tray object not found yet)"; if (now - j.started > 240000) fail(j, "no tray object"); return; }
        int ck = sv_bool(fake, Of("FakeOvenTray", "cooked")), bn = sv_bool(fake, Of("FakeOvenTray", "burnt"));
        float pr = sv_float(fake, Of("FakeOvenTray", "bakingProgress"));
        char b[96];
        snprintf(b, sizeof(b), "baking (progress %.2f)", static_cast<double>(pr));
        j.status = b;
        if (ck == 1) { spy_note("job #%d: tray is COOKED", j.id); j.step = 7; j.tries = 0; return; }
        if (bn == 1) { fail(j, "tray burnt"); return; }
        if (now - j.started > 240000) fail(j, "timeout while baking");
        return;
    }
    case 7: {
        door_set(j, j.a, j.n, 1);
        j.t = now + 1000; j.step = 8; j.status = "opening the oven to collect";
        return;
    }
    case 8: {
        if (now < j.t) return;
        interact("FakeOvenTray", j.b);
        spy_note("job #%d: FakeOvenTray.Interact (collect)", j.id);
        j.t = now + 1000; j.step = 9;
        return;
    }
    case 9: {
        if (now < j.t) return;
        if (tray_type(j.a, j.n) == 0) { door_set(j, j.a, j.n, 0); finish(j, j.param + "s are in the dump station (use the Bag buttons)"); return; }
        if (j.tries++ < 2) { j.step = 8; return; }
        fail(j, "could not collect the tray");
        return;
    }
    }
}

// ------------------------------ BAG (and finishing soda) ----------------------
void step_bag(Job& j, ULONGLONG now) {          // j.param = cookie|pie|fries|nuggets, or the item is already in j.item (step>=2)
    switch (j.step) {
    case 0: {
        if (!wait_slot(j, now)) return;
        if (!hand_acquire(j, now)) return;
        void* src = nullptr;
        if (j.param == "cookie" || j.param == "pie") {
            int fo = Of("BakedGoodsFoodSources", "foodType");
            src = near_obj_p("BakedGoodsFoodSources", j.anc, fo, j.param == "pie" ? 1 : 0, true);
            j.b = src;
            if (src) { j.snap = held_items(); interact("BakedGoodsFoodSources", src); }
        } else {
            int fo = Of("DumpStationFoodSources", "foodType");
            src = near_obj_p("DumpStationFoodSources", j.anc, fo, j.param == "nuggets" ? 1 : 0, true);
            j.b = src;
            if (src) { j.snap = held_items(); interact("DumpStationFoodSources", src); }
        }
        if (!src) { fail(j, "nothing to take: the " + j.param + " dump station is empty (cook it first)"); return; }
        spy_note("job #%d: take %s from its dump station", j.id, j.param.c_str());
        j.t = now + 200; j.step = 1; j.status = "taking from the dump station";
        return;
    }
    case 1: {
        if (now < j.t) return;
        void* it = new_held(j.snap);
        if (!it) {                                              // the game may spawn it next to the station instead of in the hand
            float sp[3] = {};
            if (j.b && pos_of(j.b, sp)) {
                for (void* o : find_objects(k_generic)) {
                    float q[3] = {};
                    if (!pos_of(o, q) || dist(q, sp) > 6.0f) continue;
                    std::string n = uname(o);
                    if (has(n, j.param == "cookie" ? "cookie" : j.param == "pie" ? "pie" : j.param == "fries" ? "fries" : "nugget")) { it = o; break; }
                }
            }
        }
        if (!it) { if (now - j.t > 3000) fail(j, "nothing came out (dump station empty?)"); return; }
        j.item = it;
        spy_note("job #%d: got '%s'", j.id, uname(it).c_str());
        j.step = 2;
        return;
    }
    case 2: {
        j.a = nearest(Kc("BaggingSystem"), j.anc);          // the bag NEAREST to where this job was started
        if (!j.a) { fail(j, "no BaggingSystem"); return; }
        if (sv_bool(j.a, Of("BaggingSystem", "hasBag")) == 0) {
            if (now - s_bagT < 3500) { j.t = now + 400; j.status = "waiting for the bag to open"; return; }
            call0(Mt("BaggingSystem", "SpawnBag", 0), j.a);
            s_bagT = now;
            spy_note("job #%d: no open bag -> SpawnBag()", j.id);
            j.t = now + 600; j.step = 3; j.status = "opening a bag"; j.n2 = 0;
        } else { j.t = now; j.step = 3; j.n2 = 0; }
        return;
    }
    case 3: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("BaggingSystem", "hasBag")) == 0 && now - s_bagT < 2500) { j.t = now + 100; j.status = "waiting for the bag to open"; return; }   // poll instead of a fixed wait
        if (!in_held(j.item) && j.n2 == 0) {            // finished drinks / burgers lie in the world: take them first, then throw
            if (!hand_acquire(j, now)) return;
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.item };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("job #%d: '%s' lies in the world -> HotbarGrab %s", j.id, uname(j.item).c_str(), exc ? "threw" : "called");
            j.n2 = 1; j.t = now + 400;
            return;
        }
        int off = Of("BaggingSystem", "bagSpawnPos");
        void* tr = off >= 0 ? rd_ptr(j.a, off) : nullptr;
        throw_above(j, j.item, tr ? tr : j.a, 0.35f, j.tries);
        j.t = now + 1000; j.step = 4; j.status = "thrown into the bag";
        return;
    }
    case 4: {
        if (is_gone(j.item)) { finish(j, "item is in the bag"); return; }
        if (now < j.t) return;
        if (++j.tries < 4) { j.step = 3; return; }
        fail(j, "the bag did not take the item");
        return;
    }
    }
}

// ----------------------------------- SODA -------------------------------------
void step_soda(Job& j, ULONGLONG now) {          // j.param = drink name
    int di = drink_index(j.param);
    switch (j.step) {
    case 0: {
        if (di < 0) { fail(j, "unknown drink '" + j.param + "'"); return; }
        if (!wait_slot(j, now)) return;
        void* src = pick_cup("soda", j.anc);
        if (!src) { fail(j, "no cup dispenser for 'Empty Soda Cup' (ScytheCupDispenser)"); return; }
        {
            void *m0 = nullptr, *t0 = nullptr;
            if (!pair_machine("DrinkMachine", "DrinkMachineTrigger", m0, t0, j.anc)) { fail(j, "no drink machine"); return; }
            if (sv_bool(m0, Of("DrinkMachine", "cupLoaded")) == 1) { j.status = "drink machine is busy - waiting"; if (now - j.started > 90000) fail(j, "drink machine stays busy"); return; }
            for (const Job& o : s_jobList) if (&o != &j && !o.done && (o.kind == J_SODA) && o.step >= 1 && o.step <= 9 && (o.a == m0 || !o.a)) { j.status = "waiting for the other drink to leave the machine"; return; }
            if (now - j.started < 20000) {                  // the finished drink must really be taken out of the machine (it blocks the cup trigger), not only claimed by a bag job
                float tp[3] = {};
                if (pos_of(t0, tp))
                    for (void* o : find_objects(k_generic)) {
                        float q[3] = {};
                        if (!pos_of(o, q) || dist(q, tp) > 1.3f || in_held(o)) continue;
                        if (low(uname(o)).find("empty") != std::string::npos) continue;
                        j.status = "waiting for the finished drink to be taken out of the machine"; return;
                    }
            }
        }
        if (!hand_acquire(j, now)) return;
        j.snap = held_items();
        interact("ScytheCupDispenser", src);
        spy_note("job #%d: take a cup from '%s'", j.id, uname(src).c_str());
        j.t = now + 500; j.step = 1; j.status = "taking a cup";
        return;
    }
    case 1: identify_held(j, now, 2); return;
    case 2: {
        if (!pair_machine("DrinkMachine", "DrinkMachineTrigger", j.a, j.b, j.anc)) { fail(j, "no drink machine"); return; }
        {   // the soda cup trigger is lower than the shake one: drop it closer (0.15 m above the trigger, 5 cm lower after every retry)
            float up = 0.15f - 0.05f * static_cast<float>(j.tries);
            if (up < 0.0f) up = 0.0f;
            throw_above(j, j.item, j.b, up, j.tries);
        }
        j.t = now + 1500; j.step = 3; j.status = "cup thrown into the machine";
        return;
    }
    case 3: {
        if (sv_bool(j.a, Of("DrinkMachine", "cupLoaded")) == 1) { hand_release(j); j.step = 4; j.tries = 0; j.t = now; return; }   // polled every tick
        if (now < j.t) return;
        if (++j.tries < 4) { j.step = 2; return; }
        fail(j, "machine did not take the cup");
        return;
    }
    case 4: {
        int want = di + 1;
        if (sv_int(j.a, Of("DrinkMachine", "selectedSoda")) == want) { j.step = 5; j.tries = 0; return; }
        if (now < j.t) return;
        if (j.tries++ >= 3) { fail(j, "could not select the drink"); return; }
        int so = Of("DrinkMachineButton", "drinkSelection");
        void* btn = nullptr;
        btn = near_obj("DrinkMachineButton", j.a, so, want);
        if (!btn) { fail(j, "no machine button for " + j.param); return; }
        interact("DrinkMachineButton", btn);
        spy_note("job #%d: press the '%s' button", j.id, j.param.c_str());
        j.t = now + 900; j.status = "selecting the drink";
        return;
    }
    case 5: {
        call0(Mt("DrinkMachine", "RequestIceFill", 0), j.a);
        spy_note("job #%d: RequestIceFill", j.id);
        j.t = now + 300; j.step = 6; j.status = "filling ice";
        return;
    }
    case 6: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("DrinkMachine", "iceFilled")) == 1) { j.step = 7; return; }
        if (now - j.t > 6000) fail(j, "ice was not filled");
        return;
    }
    case 7: {
        call0(Mt("DrinkMachine", "RequestSodaFill", 0), j.a);
        spy_note("job #%d: RequestSodaFill", j.id);
        j.t = now + 400; j.step = 8; j.status = "pouring soda";
        return;
    }
    case 8: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("DrinkMachine", "sodaFilled")) == 1 || sv_bool(j.a, Of("DrinkMachine", "cupLoaded")) == 0) { j.step = 9; j.t = now + 150; return; }
        if (now - j.t > 9000) fail(j, "soda was not poured");
        return;
    }
    case 9: {
        if (now < j.t) return;
        int off = Of("DrinkMachine", "cupSpawnPos");
        void* sp = off >= 0 ? rd_ptr(j.a, off) : nullptr;
        float p[3] = {};
        if (!sp || !pos_of(sp, p)) pos_of(j.a, p);
        const std::string tok = di >= 0 ? norm(kDrinks[di]) : norm(j.param);   // canonical name: the order text may say "Swimmer's Choice", the object is "Swimmers Choice"
        void* found = nullptr;
        for (void* o : find_objects(k_generic)) {
            const bool isCup = o == j.item;                 // the cup we threw in may become the finished drink itself (its name changes)
            float q[3] = {};
            if (!pos_of(o, q) || dist(q, p) > 4.0f) continue;
            const std::string raw = low(uname(o));
            if (raw.find("empty") != std::string::npos) continue;              // an EMPTY cup is never the finished drink
            const std::string n = norm(raw);
            if (n.find(tok) != std::string::npos || (!isCup && now - j.t > 5000 && (n.find("cup") != std::string::npos || n.find("drink") != std::string::npos))) { found = o; break; }
        }
        if (!found) { j.status = "waiting for the finished drink"; if (now - j.t > 12000) fail(j, "finished drink not found near the machine"); return; }
        j.item = found;
        spy_note("job #%d: finished drink '%s' -> bag", j.id, uname(found).c_str());
        j.kind = J_BAG; j.step = 2; j.tries = 0; j.label += " (bag)";
        return;
    }
    }
}

// ----------------------------------- SHAKE ------------------------------------
// j.param = "strawberry" | "blueberry"
void* shake_interaction(void* machine) {
    int fo = Of("ShakeCupInteraction", "machine");
    for (void* x : find_objects(Kc("ShakeCupInteraction"))) if (fo >= 0 && rd_ptr(x, fo) == machine) return x;
    return nullptr;
}
void step_shake(Job& j, ULONGLONG now) {
    const int type = j.param == "strawberry" ? 1 : 2;
    const int need = type == 1 ? 0 : 1;               // cup position: 0 = left nozzle (red / strawberry), 1 = right nozzle (blue / blueberry)
    switch (j.step) {
    case 0: {
        if (!wait_slot(j, now)) return;
        void* src = pick_cup("shake", j.anc);
        if (!src) { fail(j, "no cup dispenser for 'Empty Shake Cup' (ScytheCupDispenser)"); return; }
        void *m0 = nullptr, *t0 = nullptr;
        if (!pair_machine("ShakeMachine", "ShakeCupTrigger", m0, t0, j.anc)) { fail(j, "no shake machine"); return; }
        if (sv_bool(m0, Of("ShakeMachine", "cupLoaded")) == 1 || sv_bool(m0, Of("ShakeMachine", "isPouring")) == 1) {
            j.status = "shake machine is busy - waiting";
            if (now - j.started > 120000) fail(j, "shake machine stays busy");
            return;
        }
        for (const Job& o : s_jobList) if (&o != &j && !o.done && o.kind == J_SHAKE && o.step >= 1 && o.a == m0) { j.status = "waiting for the other shake"; return; }
        if (!hand_acquire(j, now)) return;
        j.a = m0; j.b = t0;
        j.snap = held_items();
        interact("ScytheCupDispenser", src);
        spy_note("job #%d: take a shake cup from '%s'", j.id, uname(src).c_str());
        j.t = now + 500; j.step = 1; j.status = "taking a cup";
        return;
    }
    case 1: identify_held(j, now, 2); return;
    case 2: {
        throw_above(j, j.item, j.b, 0.35f, j.tries);
        j.t = now + 1500; j.step = 3; j.status = "cup thrown into the machine";
        return;
    }
    case 3: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("ShakeMachine", "cupLoaded")) == 1) { hand_release(j); j.step = 4; j.tries = 0; return; }
        if (++j.tries < 4) { j.step = 2; return; }
        fail(j, "shake machine did not take the cup");
        return;
    }
    case 4: {                                          // put the cup under the RIGHT nozzle (each press of E on the cup moves it 0 <-> 1)
        int cur = sv_int(j.a, Of("ShakeMachine", "anchorIndex"));
        if (cur == need) { j.step = 6; j.tries = 0; return; }
        void* ia = shake_interaction(j.a);
        if (!ia) { fail(j, "no ShakeCupInteraction for this machine"); return; }
        interact("ShakeCupInteraction", ia);
        spy_note("job #%d: slide the cup (position %d -> %d, %s nozzle)", j.id, cur, need, type == 1 ? "left / strawberry" : "right / blueberry");
        j.t = now + 1300; j.step = 5; j.status = "sliding the cup";
        return;
    }
    case 5: {
        if (now < j.t) return;
        if (sv_int(j.a, Of("ShakeMachine", "anchorIndex")) == need) { j.step = 6; j.tries = 0; return; }
        if (++j.tries < 4) { j.step = 4; return; }
        fail(j, "the cup did not reach the " + j.param + " nozzle");
        return;
    }
    case 6: {
        call0(Mt("ShakeMachine", type == 1 ? "PourStrawberry" : "PourBlueberry", 0), j.a);
        spy_note("job #%d: press the %s lever", j.id, j.param.c_str());
        j.t = now + 1200; j.step = 7; j.status = "pouring";
        return;
    }
    case 7: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("ShakeMachine", "isPouring")) == 1) { j.t = now + 5500; j.step = 8; j.tries = 0; return; }
        if (sv_int(j.a, Of("ShakeMachine", "anchorIndex")) != need) { j.step = 4; j.tries = 0; return; }   // the cup moved: re-aim
        if (++j.tries < 3) { j.step = 6; return; }
        fail(j, "the machine did not start pouring");
        return;
    }
    case 8: {
        if (now < j.t) return;
        void* ia = shake_interaction(j.a);
        if (ia) interact("ShakeCupInteraction", ia);
        spy_note("job #%d: ShakeCupInteraction.Interact (finish the pour)", j.id);
        j.t = now + 1500; j.step = 9; j.status = "collecting the shake";
        return;
    }
    case 9: {
        if (now < j.t) return;
        if (sv_bool(j.a, Of("ShakeMachine", "cupLoaded")) == 0) { j.step = 10; j.t = now; j.tries = 0; return; }
        if (++j.tries < 6) { j.step = 8; j.t = now + 1500; return; }
        fail(j, "the shake was not finished");
        return;
    }
    case 10: {
        float p[3] = {};
        pos_of(j.a, p);
        void* found = nullptr;
        for (void* o : find_objects(k_generic)) {
            if (o == j.item) continue;
            float q[3] = {};
            if (!pos_of(o, q) || dist(q, p) > 4.5f) continue;
            if (shake_kind(uname(o)) == type) { found = o; break; }
        }
        if (!found) { j.status = "waiting for the finished shake"; if (now - j.t > 10000) fail(j, "finished shake not found near the machine"); return; }
        j.item = found;
        spy_note("job #%d: finished shake '%s' -> bag", j.id, uname(found).c_str());
        j.kind = J_BAG; j.step = 2; j.tries = 0; j.n2 = 0; j.label += " (bag)";
        return;
    }
    }
}

// -------------------- board ingredients and sauces (burger building) ----------
int board_count(void* board) {
    int off = Of("AssemblyBoard", "activeIngredients");
    void* l = (board && off >= 0) ? rd_ptr(board, off) : nullptr;
    return l ? rd_int(l, o_listSize) : -1;
}
void* board_for(Job& j) {
    if (j.board) return j.board;
    return nearest(k_board, j.anc);
}
// j.param = box toSpawn name ("Bun Bottom", "Cheese", ...)
void step_ingr(Job& j, ULONGLONG now) {
    switch (j.step) {
    case 0: {
        if (!wait_slot(j, now)) return;
        void* board = board_for(j);
        if (!board) { fail(j, "no assembly board"); return; }
        float bp[3] = {};
        pos_of(board, bp);
        void* box = nullptr; float bd = 1e30f;
        for (int pass = 0; pass < 2 && !box; pass++) {
            for (void* b : boxes_sorted()) {
                void* ts = o_toSpawn >= 0 ? rd_ptr(b, o_toSpawn) : nullptr;
                std::string sp = low(clean(uname(ts)));
                if (sp.empty()) continue;
                bool ok = pass == 0 ? sp == low(j.param) : sp.find(low(j.param)) != std::string::npos;
                if (!ok) continue;
                float q[3] = {};
                float d = pos_of(b, q) ? dist(q, bp) : 1e20f;
                if (d < bd) { bd = d; box = b; }
            }
        }
        if (!box) { fail(j, "no ingredient box for '" + j.param + "'"); return; }
        if (!hand_acquire(j, now)) return;
        unsigned char pid[16];
        if (!get_pid(pid)) { fail(j, "no local PlayerID"); return; }
        j.board = board;
        j.mask = board_count(board);
        j.snap = held_items();
        spy_note("job #%d: take '%s' from box '%s'", j.id, j.param.c_str(), uname(box).c_str());
        invoke_pid(m_boxInteract, box, pid);
        j.t = now + 250; j.step = 1; j.status = "taking " + j.param;
        return;
    }
    case 1: identify_held(j, now, 2); return;
    case 2: {
        throw_above(j, j.item, j.board, 0.425f, j.tries);
        j.t = now + (j.tries == 0 ? 450 : 900); j.step = 3; j.status = "thrown onto the board";
        return;
    }
    case 3: {
        if (board_count(j.board) > j.mask) { hand_release(j); finish(j, "'" + j.param + "' is on the board"); return; }
        if (now < j.t) return;
        if (is_gone(j.item)) {                              // the board consumed the object: the count list is just late
            if (now - j.t < 1500) return;
            hand_release(j); finish(j, "'" + j.param + "' was taken by the board"); return;
        }
        if (++j.tries < 6) { j.step = 2; return; }
        fail(j, "the board did not take '" + j.param + "'");
        return;
    }
    }
}
// j.param = sauce name ("Mayo", "Hot Sauce", ...)
void step_sauce(Job& j, ULONGLONG now) {
    switch (j.step) {
    case 0: {
        void* board = board_for(j);
        if (!board) { fail(j, "no assembly board"); return; }
        int ob = Of("SauceDispenser", "board"), os = Of("SauceDispenser", "saucePrefab");
        void* d = nullptr;
        float bp[3] = {};
        pos_of(board, bp);
        float bd = 1e30f;
        for (void* x : find_objects(Kc("SauceDispenser"))) {
            void* sp = os >= 0 ? rd_ptr(x, os) : nullptr;
            if (norm(uname(sp)) != norm(j.param)) continue;
            float q[3] = {};
            float dd = pos_of(x, q) ? dist(q, bp) : 1e20f;
            if (ob >= 0 && rd_ptr(x, ob) == board) dd -= 1000.0f;       // the dispenser that belongs to this board wins
            if (dd < bd) { bd = dd; d = x; }
        }
        if (!d) { fail(j, "no sauce dispenser for '" + j.param + "'"); return; }
        j.board = board;
        j.mask = board_count(board);
        interact("SauceDispenser", d);
        spy_note("job #%d: press the '%s' dispenser", j.id, j.param.c_str());
        j.t = now + 700; j.step = 1; j.status = "squirting " + j.param;
        return;
    }
    case 1: {
        if (board_count(j.board) > j.mask) { finish(j, "'" + j.param + "' is on the burger"); return; }
        if (now < j.t) return;
        if (++j.tries < 4) { j.step = 0; j.t = now + 400; return; }
        fail(j, "the sauce was not added");
        return;
    }
    }
}

// ----------------------------------- ALPHA ------------------------------------
// Builds the whole order of the register nearest to the player: opens a bag, cooks / takes every item
// and throws it into the bag; burgers are assembled on the nearest assembly board in the recipe order.
Job* jb(int id) { for (Job& o : s_jobList) if (o.id == id) return &o; return nullptr; }
int add_job(int kind, const std::string& label, const std::string& param, ULONGLONG delay = 0);

bool job_failed(const Job* k) { return k && k->status.rfind("FAILED", 0) == 0; }

std::string patty_for(const std::string& id) {
    std::string n = norm(id);
    if (n.find("pork") != std::string::npos) return "Raw Pork Patty";
    if (n.find("fish") != std::string::npos) return "Raw Fish Patty";
    if (n.find("chicken") != std::string::npos) return "Raw Chicken Patty";
    return "Raw Beef Patty";                           // "Cooked Burger" / "Beef Patty"
}
std::string task_for(const std::string& id) {
    std::string n = norm(id);
    if (n.rfind("raw", 0) == 0) {                      // backrooms: an UNCOOKED patty goes straight onto the board
        if (n.find("pork") != std::string::npos) return "ing:Raw Pork Patty";
        if (n.find("fish") != std::string::npos) return "ing:Raw Fish Patty";
        if (n.find("chicken") != std::string::npos) return "ing:Raw Chicken Patty";
        return "ing:Raw Beef Patty";                   // "Raw Burger"
    }
    if (n.rfind("burnt", 0) == 0) return "pburnt:" + patty_for(id);     // backrooms: a BURNED patty (cooked until it burns)
    if (n.find("bottombun") != std::string::npos) return "ing:Bun Bottom";
    if (n.find("topbun") != std::string::npos) return "ing:Bun Top";
    if (n.find("patty") != std::string::npos || n == "cookedburger") return "patty:" + patty_for(id);
    if (n == "mayo" || n == "mustard" || n == "hotsauce" || n == "ketchup" || n == "bbq") return "sauce:" + id;
    return "ing:" + id;
}

// ------------------------------- side stock -----------------------------------
// Cooked fries / nuggets / cookies / pies wait in the dump stations (SyncVar counters). Alpha uses what is there and cooks
// only what is missing; STOCK cooks one batch of every missing side so that the next order is fast.
int side_stock(const std::string& what, const float anc[3]) {
    const bool baked = what == "cookie" || what == "pie";
    const char* cls = baked ? "BakedGoodsDumpStation" : "DumpStation";
    const char* f = what == "fries" ? "friesAmount" : what == "nuggets" ? "nuggetsAmount" : what == "cookie" ? "cookiesAmount" : "piesAmount";
    void* st = nearest(Kc(cls), anc);
    if (!st) return 0;
    return sv_int(st, Of(cls, f), 0);
}
bool job_active(int kind, const std::string& param) {
    for (const Job& o : s_jobList) if (!o.done && o.kind == kind && o.param == param) return true;
    return false;
}
int start_stock(const char* why) {
    float pp[3]; player_pos(pp);
    int n = 0;
    struct S { const char* what; int kind; const char* label; };
    static const S kS[4] = { { "fries", J_FRY, "Stock: fries batch" }, { "nuggets", J_FRY, "Stock: nuggets batch" },
                             { "cookie", J_TRAY, "Stock: cookie tray" }, { "pie", J_TRAY, "Stock: pie tray" } };
    for (const S& x : kS) {
        if (side_stock(x.what, pp) > 0 || job_active(x.kind, x.what)) continue;
        add_job(x.kind, x.label, x.what, x.kind == J_TRAY ? 3000 : 0);
        n++;
    }
    spy_note("stock (%s): %d batch(es) started", why, n);
    return n;
}

void step_alpha(Job& j, ULONGLONG now) {
    switch (j.step) {
    case 0: {                                          // read the order and plan everything
        // The order comes from the ACCEPTED orders (OrderManager.activeOrders): the oldest one that is not being built yet.
        // Only if nothing was accepted (e.g. the list is not available) the text on the nearest register is used.
        std::vector<AcceptedOrder> acc;
        features_accepted_orders(acc);
        std::vector<std::string> items;
        std::string src;
        // orders are told apart by their object, not by id/content: the game may give the same order several times
        for (auto it = s_handledOrders.begin(); it != s_handledOrders.end();) {      // forget orders that are gone
            bool alive = false;
            for (const AcceptedOrder& a : acc) if (a.key == *it) { alive = true; break; }
            it = alive ? std::next(it) : s_handledOrders.erase(it);
        }
        const int wantId = j.param.empty() ? -1 : atoi(j.param.c_str());      // a ticket clicked in the book: exactly this order
        for (const AcceptedOrder& a : acc) {
            if (s_handledOrders.count(a.key)) continue;
            if (wantId >= 0 && a.id != wantId) continue;
            if (wantId < 0 && a.type == 1 && !g.alphaDrive.load()) continue;          // drive-thru order, switch is off
            items = a.items; j.orderKey = a.key; src = "accepted order #" + std::to_string(a.id) + (a.type == 1 ? " (drive-thru)" : "");
            break;
        }
        if (items.empty() && wantId >= 0) { fail(j, "that order is gone or already being built"); return; }
        if (items.empty() && !acc.empty()) { fail(j, g.alphaDrive.load() ? "all accepted orders are already being built" : "no suitable accepted order (drive-thru orders are skipped, or all are already being built)"); return; }
        if (items.empty()) {
            std::vector<void*> regs;
            std::vector<std::vector<std::string>> orders;
            features_register_orders(regs, orders);
            if (regs.empty()) { fail(j, "no accepted order (and no order text on a register)"); return; }
            size_t best = 0; float bd = 1e30f;
            for (size_t i = 0; i < regs.size(); i++) {
                float q[3] = {};
                float d = pos_of(regs[i], q) ? dist(q, j.anc) : 1e20f;
                if (d < bd) { bd = d; best = i; }
            }
            items = orders[best]; src = "register text";
        }
        if (j.orderKey) s_handledOrders.insert(j.orderKey);
        j.board = nearest(k_board, j.anc);
        if (!j.board) { fail(j, "no assembly board"); return; }
        std::string list;
        int fries = 0, nugs = 0, cookies = 0, pies = 0, bad = 0;
        int lastPattyIdx = -1;                          // plan index of the last patty task of the previous burger
        std::vector<int> gated;
        for (const std::string& it : items) {
            int kind = 0;
            std::vector<std::string> ids;
            features_recipe_info(it.c_str(), kind, ids);
            list += it + "; ";
            if (kind == 1 && !ids.empty()) {
                const int gateAt = lastPattyIdx + 1;         // patties of this burger start when the previous burger has taken its last patty
                const size_t firstKid = j.kids.size();
                {   // the recipe list may run top -> bottom (even when it starts with a sauce): decide by the positions of the buns
                    int iTop = -1, iBot = -1;
                    for (size_t q = 0; q < ids.size(); q++) {
                        const std::string nq = norm(ids[q]);
                        if (iTop < 0 && nq.find("topbun") != std::string::npos) iTop = static_cast<int>(q);
                        if (iBot < 0 && nq.find("bottombun") != std::string::npos) iBot = static_cast<int>(q);
                    }
                    const bool rev = (iTop >= 0 && iBot >= 0) ? iTop < iBot : (iTop == 0 || iBot == static_cast<int>(ids.size()) - 1);
                    std::string dbg;
                    for (const std::string& x : ids) dbg += x + ", ";
                    spy_note("alpha: recipe '%s' ids (as stored): %s%s", it.c_str(), dbg.c_str(), rev ? "-> reversed" : "-> as is");
                    if (rev) std::reverse(ids.begin(), ids.end());   // bottom -> top
                }
                {   // a sauce can only be squirted onto a burger that already has something on the board: leading sauces move behind the first real ingredient
                    std::vector<std::string> head, rest;
                    size_t q = 0;
                    while (q < ids.size() && task_for(ids[q]).rfind("sauce:", 0) == 0) head.push_back(ids[q++]);
                    if (!head.empty() && q < ids.size()) {
                        spy_note("alpha: '%s' starts with %d sauce(s) - they are put behind '%s'", it.c_str(), static_cast<int>(head.size()), ids[q].c_str());
                        std::vector<std::string> o2;
                        o2.push_back(ids[q]);
                        o2.insert(o2.end(), head.begin(), head.end());
                        o2.insert(o2.end(), ids.begin() + static_cast<std::ptrdiff_t>(q) + 1, ids.end());
                        ids = o2;
                    }
                }
                for (const std::string& id : ids) {
                    std::string t = task_for(id);
                    if (t.rfind("patty:", 0) == 0) j.kids.push_back(add_job(J_PATTY, "Alpha: " + t.substr(6), t.substr(6)));
                    else if (t.rfind("pburnt:", 0) == 0) {
                        int pid = add_job(J_PATTY, "Alpha: BURNT " + t.substr(7), t.substr(7));
                        if (Job* pk = jb(pid)) pk->burnt = true;
                        j.kids.push_back(pid);
                    }
                    j.plan.push_back(t);
                    if (t.rfind("patty:", 0) == 0 || t.rfind("pburnt:", 0) == 0) lastPattyIdx = static_cast<int>(j.plan.size()) - 1;
                }
                for (size_t gi = firstKid; gi < j.kids.size(); gi++) if (gateAt > 0) if (Job* gk = jb(j.kids[gi])) { gk->gateId = j.id; gk->gateN = gateAt; }
                j.plan.push_back("finish:" + it);
            } else if (kind == 2) {
                int sk = shake_kind(it);
                if (sk) j.chain.push_back({ add_job(J_SHAKE, "Alpha shake: " + it, sk == 1 ? "strawberry" : "blueberry"), "" });
                else if (drink_index(it) >= 0) j.chain.push_back({ add_job(J_SODA, "Alpha soda: " + it, it), "" });
                else { bad++; spy_note("alpha: unknown drink '%s'", it.c_str()); }
            } else if (kind == 3) {
                if (norm(it).find("nugget") != std::string::npos) nugs++; else fries++;
            } else if (kind == 4) {
                if (norm(it).find("pie") != std::string::npos) pies++; else cookies++;
            } else { bad++; spy_note("alpha: unknown item '%s' (no recipe found)", it.c_str()); }
        }
        // patty jobs wait cooked on the grill until the plan asks for them; they go to the board
        for (int id : j.kids) if (Job* k = jb(id)) { k->dest = 0; k->hold = true; k->board = j.board; }
        auto side = [&](int n, int kind, const char* label, const char* param, const char* bag) {
            if (n <= 0) return;
            const int have = side_stock(bag, j.anc);
            if (have >= n) {                                // already cooked and waiting in the dump station: do NOT cook again
                spy_note("alpha: %d x %s already in the dump station (%d there) - not cooked again", n, bag, have);
                for (int i = 0; i < n; i++) j.chain.push_back({ 0, bag });
                return;
            }
            int id = add_job(kind, label, param, kind == J_TRAY ? 3000 : 0);
            for (int i = 0; i < n; i++) j.chain.push_back({ id, bag });
        };
        side(fries, J_FRY, "Alpha: fries batch", "fries", "fries");
        side(nugs, J_FRY, "Alpha: nuggets batch", "nuggets", "nuggets");
        side(cookies, J_TRAY, "Alpha: cookie tray", "cookie", "cookie");
        side(pies, J_TRAY, "Alpha: pie tray", "pie", "pie");
        spy_note("alpha: %s = %s(%d plan steps, %d unknown)", src.c_str(), list.c_str(), static_cast<int>(j.plan.size()), bad);
        j.dest = 0; j.n = 0; j.step = 1; j.status = "planned: " + list;
        return;
    }
    case 1: {
        // 1) side jobs / drinks that finished: chain entries with a bag parameter -> bag job.
        //    A failed job is REPEATED (up to 8 repeats per order), it is not skipped.
        std::map<int, int> remap;
        for (size_t i = 0; i < j.chain.size();) {
            const int oldId = j.chain[i].first;
            Job* k = jb(oldId);
            const std::string bagParam = j.chain[i].second;
            if (k && !k->done) { i++; continue; }
            if (k && job_failed(k)) {
                auto rm = remap.find(oldId);
                if (rm != remap.end()) { j.chain[i].first = rm->second; i++; continue; }          // several bags wait for the same batch job
                int nid = 0;
                bool consumed = false;
                if (j.cfail[2] < 8) {
                    if (k->kind == J_BAG) {
                        if (k->item && is_gone(k->item)) consumed = true;                           // the item is already in the bag
                        else if (drink_index(k->param) >= 0 && (!k->item || has(low(uname(k->item)), "empty")))
                            nid = add_job(J_SODA, "Alpha soda: " + k->param, k->param, 1500);       // the bag job held an EMPTY cup: make the drink again
                        else {
                            nid = add_job(J_BAG, k->label, k->param, 1500);
                            if (k->item) if (Job* nk = jb(nid)) { nk->item = k->item; nk->step = 2; nk->t = now + 1500; }
                        }
                    } else if (k->kind == J_SODA || k->kind == J_SHAKE || k->kind == J_FRY || k->kind == J_TRAY) {
                        nid = add_job(k->kind, k->label, k->param, k->kind == J_TRAY ? 3000 : 1500);
                    }
                    if (nid) { j.cfail[2]++; remap[oldId] = nid; j.chain[i].first = nid; spy_note("alpha: '%s' failed (%s) - REPEATING (%d/8)", k->label.c_str(), k->status.c_str(), j.cfail[2]); i++; continue; }
                }
                if (!consumed) { spy_note("alpha: '%s' failed (%s) and could not be repeated - skipped", k->label.c_str(), k->status.c_str()); j.n2++; }
            } else if (!bagParam.empty()) {
                // the batch job is done: bag it, and keep watching the bag job so that a failed bag is repeated as well
                int bid = add_job(J_BAG, "Alpha bag: " + bagParam, bagParam);
                j.chain[i] = { bid, "" };
                i++; continue;
            }
            j.chain.erase(j.chain.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // 2) the burger plan, one task at a time
        if (j.kid) {
            Job* k = jb(j.kid);
            if (k && !k->done) { j.status = "task " + std::to_string(j.n + 1) + "/" + std::to_string(j.plan.size()) + ": " + k->status; return; }
            if (job_failed(k)) {
                const std::string task = j.plan[static_cast<size_t>(j.n)];
                const std::string tk = task.substr(0, task.find(':'));
                if (tk == "finish" && k->item && is_gone(k->item)) { j.kid = 0; j.tries = 0; j.n++; j.t = now + 20; return; }   // the burger is already in the bag
                if (j.tries < 3 && now - j.started < 900000) {
                    j.tries++;
                    spy_note("alpha: task '%s' failed (%s) - REPEATING (%d/3)", task.c_str(), k->status.c_str(), j.tries);
                    j.t = now + 1500;
                    if (tk == "patty" || tk == "pburnt") {                    // a new patty job takes the place of the failed one
                        const size_t idx = static_cast<size_t>(j.dest - 1);
                        const std::string pname = task.substr(task.find(':') + 1);
                        int pid = add_job(J_PATTY, "Alpha: " + std::string(tk == "pburnt" ? "BURNT " : "") + pname, pname);
                        if (Job* pk = jb(pid)) { pk->burnt = (tk == "pburnt"); pk->dest = 0; pk->board = j.board; pk->hold = false; }
                        if (idx < j.kids.size()) j.kids[idx] = pid; else j.kids.push_back(pid);
                        j.kid = pid;
                    } else if (tk == "finish") {                              // bag the finished burger again
                        int bid = add_job(J_BAG, "Alpha bag: burger", "burger");
                        if (Job* nk = jb(bid)) { nk->item = k->item; nk->step = 2; nk->t = now + 1500; }
                        j.kid = bid;
                    } else j.kid = 0;                                         // ingredient / sauce: the same task is started again
                    return;
                }
                fail(j, "task '" + task + "' failed 3 times: " + k->status);
                return;
            }
            j.kid = 0; j.tries = 0; j.n++; j.t = now + 20;
            return;
        }
        if (j.n < static_cast<int>(j.plan.size())) {
            if (now < j.t) return;
            const std::string task = j.plan[static_cast<size_t>(j.n)];
            const size_t c = task.find(':');
            const std::string kind = task.substr(0, c), arg = task.substr(c + 1);
            j.status = "task " + std::to_string(j.n + 1) + "/" + std::to_string(j.plan.size()) + ": " + task;
            if (kind == "ing") { j.kid = add_job(J_INGR, "Alpha: " + arg, arg); if (Job* k = jb(j.kid)) k->board = j.board; }
            else if (kind == "sauce") { j.kid = add_job(J_SAUCE, "Alpha sauce: " + arg, arg); if (Job* k = jb(j.kid)) k->board = j.board; }
            else if (kind == "patty" || kind == "pburnt") {
                int idx = j.dest++;
                Job* pk = idx < static_cast<int>(j.kids.size()) ? jb(j.kids[static_cast<size_t>(idx)]) : nullptr;
                if (!pk) { fail(j, "the patty job disappeared"); return; }
                pk->hold = false;
                j.kid = pk->id;
            } else if (kind == "finish") {
                j.step = 2; j.t = now + 1400; j.status = "checking the burger: " + arg;
            }
            return;
        }
        // 3) everything planned is started; wait for the rest
        bool pending = !j.chain.empty();
        for (int id : j.kids) { Job* k = jb(id); if (k && !k->done && k->kind == J_BAG) pending = true; }
        if (!pending) {
            if (j.n2 > 0) {
                char b[160];
                snprintf(b, sizeof(b), "%d item(s) could not be made - the bag is left OPEN, finish it by hand", j.n2);
                finish(j, b);
            } else { j.step = 4; j.tries = 0; j.t = now + 600; j.status = "closing the bag"; }
        } else j.status = "waiting for sides / drinks / bag";
        return;
    }
    case 2: {                                          // burger finished building: check, press finish, bag it
        if (now < j.t) return;
        int off = Of("AssemblyBoard", "_wasMatchingRecipe");
        bool match = off >= 0 && *(static_cast<char*>(j.board) + off) != 0;
        if (!match) {
            fail(j, "the board does not show a recognised recipe (" + j.plan[static_cast<size_t>(j.n)] + "); ingredients on it: " + std::to_string(board_count(j.board)) +
                    " - clear the board and look at the Spy lines above");
            return;
        }
        float bp[3] = {};
        pos_of(j.board, bp);
        j.snap.clear();
        for (void* o : find_objects(k_generic)) {
            float q[3] = {};
            if (pos_of(o, q) && dist(q, bp) < 3.5f) j.snap.push_back(o);
        }
        call0(Mt("AssemblyBoard", "FinishBurger", 0), j.board);
        spy_note("alpha: AssemblyBoard.FinishBurger()");
        j.t = now + 1800; j.step = 3; j.status = "burger finished";
        return;
    }
    case 3: {
        if (now < j.t) return;
        float bp[3] = {};
        pos_of(j.board, bp);
        void* found = nullptr;
        for (void* o : find_objects(k_generic)) {
            if (std::find(j.snap.begin(), j.snap.end(), o) != j.snap.end()) continue;
            float q[3] = {};
            if (pos_of(o, q) && dist(q, bp) < 3.5f) { found = o; break; }
        }
        if (!found) { j.status = "waiting for the finished burger"; if (now - j.t > 8000) fail(j, "the finished burger did not appear on the board"); return; }
        spy_note("alpha: finished burger = '%s' -> bag", uname(found).c_str());
        int id = add_job(J_BAG, "Alpha bag: burger", "burger");
        if (Job* k = jb(id)) { k->item = found; k->step = 2; k->t = now; }
        j.kid = id;
        j.step = 1;                                    // the kid-wait logic continues with the next plan task
        return;
    }
    case 4: {                                          // every item is in the bag: close it (the bell: BaggingSystem.FinishBag)
        if (now < j.t) return;
        j.a = nearest(Kc("BaggingSystem"), j.anc);
        if (!j.a) { finish(j, "order assembled (no BaggingSystem found to close the bag)"); return; }
        if (sv_bool(j.a, Of("BaggingSystem", "hasBag")) != 1) { finish(j, "order assembled (the bag was already closed)"); return; }
        float bp[3] = {};
        pos_of(j.a, bp);
        j.snap.clear();
        for (void* o : find_objects(k_generic)) {
            float q[3] = {};
            if (pos_of(o, q) && dist(q, bp) < 36.0f) j.snap.push_back(o);
        }
        call0(Mt("BaggingSystem", "FinishBag", 0), j.a);
        spy_note("alpha: BaggingSystem.FinishBag() - the bag is closed");
        j.t = now + 1500; j.step = 5; j.status = "bag closed - waiting for the finished bag";
        return;
    }
    case 5: {                                          // find the new "To-Go Bag" (a GenericObject with a BaggedOrder)
        if (now < j.t) return;
        float bp[3] = {};
        pos_of(j.a, bp);
        void* found = nullptr;
        for (void* o : find_objects(k_generic)) {
            if (std::find(j.snap.begin(), j.snap.end(), o) != j.snap.end()) continue;
            float q[3] = {};
            if (!pos_of(o, q) || dist(q, bp) > 36.0f) continue;
            if (component_of(o, Kc("BaggedOrder")) || has(uname(o), "bag")) { found = o; break; }
        }
        if (!found) {
            j.status = "waiting for the finished bag";
            if (now - j.t > 6000) {
                if (j.tries++ < 1) { spy_note("alpha: no finished bag appeared - closing again"); j.step = 4; j.t = now; }
                else fail(j, "the finished bag did not appear (the order is in the open bag)");
            }
            return;
        }
        j.item = found; j.tries = 0; j.n = 0;
        spy_note("alpha: finished bag = '%s'", uname(found).c_str());
        j.step = 6; j.t = now;
        return;
    }
    case 6: {                                          // take the bag
        if (now < j.t) return;
        if (is_gone(j.item)) { hand_release(j); finish(j, "order assembled and the bag is gone (handed over)"); if (g.stockAuto.load()) start_stock("after the order"); return; }
        if (!in_held(j.item)) {
            if (!hand_acquire(j, now)) return;
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.item };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("alpha: HotbarGrab(finished bag) %s", exc ? "threw" : "called");
            j.t = now + 500;
        }
        j.step = 7; j.status = "taking the finished bag";
        return;
    }
    case 7: {                                          // throw it onto the matching serving window
        if (now < j.t) return;
        std::vector<void*> wins = find_objects(Kc("FulfillmentWindow"));
        if (wins.empty()) { hand_release(j); finish(j, "order assembled, the bag is closed (no FulfillmentWindow found - hand it over yourself)"); return; }
        void* bo = component_of(j.item, Kc("BaggedOrder"));
        int want = -1; const int ow = Of("BaggedOrder", "intendedWindowType");
        if (bo && ow >= 0) want = rd_int(bo, ow);
        const int wo = Of("FulfillmentWindow", "orderWindowType");
        // matching type first, then nearest to the bagging system; every retry takes the next candidate
        std::vector<std::pair<float, void*>> cand;
        float bp[3] = {};
        pos_of(j.a, bp);
        for (void* w : wins) {
            float q[3] = {};
            float d = pos_of(w, q) ? dist(q, bp) : 1e20f;
            const bool match = want >= 0 && wo >= 0 && rd_int(w, wo) == want;
            cand.push_back({ (match ? 0.0f : 1e10f) + d, w });
        }
        std::sort(cand.begin(), cand.end(), [](const std::pair<float, void*>& x, const std::pair<float, void*>& y) { return x.first < y.first; });
        void* w = cand[static_cast<size_t>(j.n) % cand.size()].second;
        spy_note("alpha: bag type=%d -> window type=%d (%d windows)", want, wo >= 0 ? rd_int(w, wo) : -1, static_cast<int>(wins.size()));
        throw_above(j, j.item, w, 0.3f, j.tries);
        j.t = now + 1800; j.step = 8; j.status = "bag thrown onto the window";
        return;
    }
    case 8: {
        if (is_gone(j.item)) { hand_release(j); finish(j, "order assembled, bag closed and handed to the window"); if (g.stockAuto.load()) start_stock("after the order"); return; }
        if (now < j.t) return;
        j.tries++;
        if (j.tries % 2 == 0) j.n++;                    // every second retry: another window
        if (j.tries < 6) { j.step = 6; j.t = now; return; }
        hand_release(j);
        fail(j, "the window did not take the bag (it lies near the window - take it by hand)");
        return;
    }
    }
}


// ------------------------------- BETA CLOSEOUT --------------------------------
// Looks at the closeout duties that are ACTIVE right now (CloseoutDuty.dutyActive && !dutyComplete) and does
// them one after another with the local player's own actions: knob / button presses (Interact with the local
// PlayerID) and carrying objects (HotbarGrab + throw into the trigger zone, like the patties). No NPC methods.
// Task ids: 1 fires, 2 thermostats, 3 rat traps, 4 appliances off, 5 trash bags, 6 kitchen trash, 7 grease,
//           8 napkins, 9 air filters, 10 restock boxes.
struct DutyDef { const char* cls; const char* name; };
const DutyDef kDuties[10] = {
    { "FireExtinguishCloseoutDuty", "fires" }, { "ThermostatCloseoutDuty", "thermostats" }, { "RatTrapCloseoutDuty", "rat traps" },
    { "TurnOffAppliancesCloseoutDuty", "appliances" }, { "TrashCloseoutDuty", "trash bags" }, { "KitchenTrashCloseoutDuty", "kitchen trash" },
    { "GreaseTrapCloseoutDuty", "grease traps" }, { "NapkinRefillCloseoutDuty", "napkins" }, { "AirFilterCloseoutDuty", "air filters" },
    { "RestockCloseoutDuty", "restock" } };
std::set<void*> s_closeSkip;

bool rd_bool(void* o, int off) { return o && off >= 0 && *(reinterpret_cast<unsigned char*>(o) + off) != 0; }
void* duty_active(const char* cls) {
    for (void* d : find_objects(Kc(cls)))
        if (sv_bool(d, Of("CloseoutDuty", "dutyActive")) == 1 && sv_bool(d, Of("CloseoutDuty", "dutyComplete")) != 1) return d;
    return nullptr;
}
std::string duty_progress(const char* cls, void* d) {
    void* a = call0(Mt(cls, "get_CurrentProgress", 0), d);
    void* b = call0(Mt(cls, "get_MaxProgress", 0), d);
    if (!a || !b) return "?";
    char t[40]; snprintf(t, sizeof(t), "%d/%d", *reinterpret_cast<int*>(static_cast<char*>(a) + 0x10), *reinterpret_cast<int*>(static_cast<char*>(b) + 0x10));
    return t;
}
std::string str_field(void* o, const char* cls, const char* f) {
    int off = Of(cls, f);
    void* s = (o && off >= 0) ? rd_ptr(o, off) : nullptr;
    return s ? il2::to_utf8(s) : std::string();
}
bool carry_done(const Job& j) {
    switch (j.n) {
    case 5: case 7: case 10: return is_gone(j.item) || rd_bool(j.item, Of("CloseoutDutyObject", "hasBeenCompleted"));
    case 6: return is_gone(j.item) || rd_bool(j.item, Of("KitchenTrashItem", "hasBeenDisposed"));
    case 8: return sv_bool(j.b, Of("NapkinDispenser", "isRefilled")) == 1 || is_gone(j.a);
    case 9: return sv_bool(j.b, Of("ACUnit", "hasCleanFilter")) == 1 || is_gone(j.a);
    }
    return true;
}
// storage zone for a restock box: the zone whose name shares the most words with the box name; retries take the next best
void* restock_zone(void* box, int tries) {
    std::vector<void*> zs = find_objects(Kc("StorageZone"));
    if (zs.empty()) return nullptr;
    std::string bn = low(clean(uname(box))) + " " + low(clean(uname(component_of(box, k_generic))));
    float bp[3] = {}; pos_of(box, bp);
    struct S { int score; float d; void* z; };
    std::vector<S> v;
    for (void* z : zs) {
        std::string zn = low(str_field(z, "StorageZone", "zoneName")) + " " + low(clean(uname(z)));
        int sc = 0; std::string tok;
        for (size_t i = 0; i <= zn.size(); i++) {
            char c = i < zn.size() ? zn[i] : ' ';
            if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') { if (tok.size() >= 3 && bn.find(tok) != std::string::npos) sc++; tok.clear(); }
            else tok += c;
        }
        float q[3] = {}; float d = pos_of(z, q) ? dist(q, bp) : 1e20f;
        v.push_back({ sc, d, z });
    }
    std::stable_sort(v.begin(), v.end(), [](const S& a, const S& b) { return a.score != b.score ? a.score > b.score : a.d < b.d; });
    S& s = v[static_cast<size_t>(tries) % v.size()];
    spy_note("closeout: restock box '%s' -> zone '%s' / '%s' (word match %d, try %d)", clean(uname(box)).c_str(),
             str_field(s.z, "StorageZone", "zoneName").c_str(), clean(uname(s.z)).c_str(), s.score, tries);
    return s.z;
}
void* trash_zone(void* item, int tries) {
    std::vector<void*> zs = find_objects(Kc("TrashDisposalZone"));
    if (zs.empty()) return nullptr;
    float ip[3] = {}; pos_of(item, ip);
    std::sort(zs.begin(), zs.end(), [&](void* a, void* b) { float p[3] = {}, q[3] = {}; float da = pos_of(a, p) ? dist(p, ip) : 1e20f, db = pos_of(b, q) ? dist(q, ip) : 1e20f; return da < db; });
    return zs[static_cast<size_t>(tries) % zs.size()];
}

bool start_task(Job& j, int id, void* duty, ULONGLONG now) {
    float pp[3]; player_pos(pp);
    j.tries = 0; j.n2 = 0; j.dest = 0; j.a = j.b = j.item = nullptr; j.t2 = now; j.t3 = now;
    auto notSkipped = [&](void* o) { return !s_closeSkip.count(o); };
    switch (id) {
    case 1: {
        std::vector<void*> f;
        for (void* o : find_objects(Kc("Fire"))) if (sv_bool(o, Of("Fire", "extinguished")) != 1 && notSkipped(o)) f.push_back(o);
        if (f.empty()) return false;
        j.n = 1; j.step = 50; spy_note("closeout: fires (%s) -> extinguisher", duty_progress(kDuties[0].cls, duty).c_str()); return true;
    }
    case 2: {
        std::vector<void*> t;
        for (void* o : find_objects(Kc("Thermostat")))
            if (sv_bool(o, Of("Thermostat", "isActive")) == 1 && sv_bool(o, Of("Thermostat", "isMatched")) != 1 && notSkipped(o)) t.push_back(o);
        if (t.empty()) return false;
        j.n = 2; j.a = nearest_of(t, pp); j.step = 10;
        spy_note("closeout: thermostat target %d, actual %d (%s)", sv_int(j.a, Of("Thermostat", "targetTemp"), -999), sv_int(j.a, Of("Thermostat", "actualTemp"), -999),
                 duty_progress(kDuties[1].cls, duty).c_str());
        return true;
    }
    case 3: {
        std::vector<void*> t;
        for (void* o : find_objects(Kc("RatTrap"))) if (sv_bool(o, Of("RatTrap", "isTriggered")) == 1 && notSkipped(o)) t.push_back(o);
        if (t.empty()) return false;
        j.n = 3; j.a = nearest_of(t, pp); j.step = 20;
        spy_note("closeout: rat trap triggered (%s) -> reset", duty_progress(kDuties[2].cls, duty).c_str()); return true;
    }
    case 4: {
        for (void* f : find_objects(Kc("Fryer"))) {
            int off = Of("Fryer", "temperatureState");
            if (off >= 0 && rd_int(f, off) > 0 && notSkipped(f)) { j.n = 4; j.a = f; j.n2 = 0; j.step = 30; spy_note("closeout: fryer is on -> turn off"); return true; }
        }
        for (void* o : find_objects(Kc("Oven")))
            for (int s = 0; s < 2; s++) {
                void* key = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(o) + static_cast<uintptr_t>(s));
                if (oven_temp(o, s) > 0 && notSkipped(key)) { j.n = 4; j.a = o; j.n2 = 1 + s; j.step = 30; spy_note("closeout: oven %s is on -> turn off", s ? "bottom" : "top"); return true; }
            }
        return false;
    }
    case 5: case 6: case 7: case 10: {
        const char* cls = id == 5 ? "CloseoutTrashBag" : id == 6 ? "KitchenTrashItem" : id == 7 ? "GreaseBucket" : "RestockBox";
        std::vector<void*> t;
        for (void* o : find_objects(Kc(cls))) {
            if (!notSkipped(o)) continue;
            if (id == 6 ? rd_bool(o, Of("KitchenTrashItem", "hasBeenDisposed")) : rd_bool(o, Of("CloseoutDutyObject", "hasBeenCompleted"))) continue;
            t.push_back(o);
        }
        if (t.empty()) return false;
        j.n = id; j.item = nearest_of(t, pp); j.step = 40;
        spy_note("closeout: %s '%s' (%s) -> carry", kDuties[id - 1].name, clean(uname(j.item)).c_str(), duty_progress(kDuties[id - 1].cls, duty).c_str()); return true;
    }
    case 8: case 9: {
        const char* uc = id == 8 ? "NapkinDispenser" : "ACUnit";
        void* unit = nullptr;
        for (void* o : find_objects(Kc(uc))) {
            if (!notSkipped(o)) continue;
            bool need = id == 8 ? (sv_bool(o, Of(uc, "needsRefill")) == 1 && sv_bool(o, Of(uc, "isRefilled")) != 1) : (sv_bool(o, Of(uc, "hasCleanFilter")) == 0);
            if (need) { unit = o; break; }
        }
        if (!unit) return false;
        std::vector<void*> t;
        for (void* o : find_objects(Kc(id == 8 ? "NapkinBox" : "AirFilterItem"))) {
            if (!notSkipped(o)) continue;
            if (id == 9 && !rd_bool(o, Of("AirFilterItem", "isClean"))) continue;
            t.push_back(o);
        }
        if (t.empty()) { spy_note("closeout: %s needs service but no %s found in the world", kDuties[id - 1].name, id == 8 ? "NapkinBox" : "clean AirFilterItem"); return false; }
        j.n = id; j.b = unit; j.item = nearest_of(t, pp); j.step = 40;
        spy_note("closeout: %s '%s' (%s) -> carry to the unit", kDuties[id - 1].name, clean(uname(j.item)).c_str(), duty_progress(kDuties[id - 1].cls, duty).c_str()); return true;
    }
    }
    return false;
}

void task_end(Job& j, ULONGLONG now, bool ok, const char* why, void* skipKey = nullptr) {
    hand_release(j);
    if (!ok) {
        if (skipKey) s_closeSkip.insert(skipKey);
        if (j.n >= 1 && j.n < 12 && ++j.cfail[j.n] >= 3) { j.mask |= (1 << j.n); spy_note("closeout: task '%s' failed 3 times - disabled for this run", kDuties[j.n - 1].name); }
    }
    spy_note("closeout: %s - %s", ok ? "OK" : "gave up", why);
    j.step = 1; j.t = now + 60; j.t2 = now;
}

void step_close(Job& j, ULONGLONG now) {
    switch (j.step) {
    case 0: {                                                    // report what the game has now
        s_closeSkip.clear();
        int active = 0;
        for (int i = 0; i < 10; i++) {
            void* d = duty_active(kDuties[i].cls);
            if (d) { active++; spy_note("closeout: ACTIVE duty %s (%s) progress %s", kDuties[i].cls, kDuties[i].name, duty_progress(kDuties[i].cls, d).c_str()); }
        }
        if (!active) { fail(j, "no active closeout duty (finish the shift and wait for the after-shift tasks)"); return; }
        j.t2 = now; j.step = 1; j.status = "looking for work";
        return;
    }
    case 1: {                                                    // pick the next piece of work
        if (now < j.t) return;
        bool any = false; std::string left;
        for (int i = 0; i < 10; i++) {
            void* d = duty_active(kDuties[i].cls);
            if (!d) continue;
            any = true; left += std::string(kDuties[i].name) + " " + duty_progress(kDuties[i].cls, d) + "; ";
            if (j.mask & (1 << (i + 1))) continue;
            if (start_task(j, i + 1, d, now)) { j.status = std::string("working: ") + kDuties[i].name; return; }
        }
        if (!any) { if (now - j.t2 > 3000) finish(j, "all closeout duties are complete"); else j.status = "duties complete?"; return; }
        j.status = "waiting: " + left;
        if (now - j.t2 > 25000) finish(j, "nothing more the mod can do. Left: " + left);
        j.t = now + 600;
        return;
    }
    case 10: {                                                   // thermostat: press up / down until it matches
        if (now < j.t) return;
        void* th = j.a;
        if (is_gone(th) || sv_bool(th, Of("Thermostat", "isMatched")) == 1 || sv_bool(th, Of("Thermostat", "isActive")) == 0) { task_end(j, now, true, "thermostat matched"); return; }
        int tgt = sv_int(th, Of("Thermostat", "targetTemp"), -999), act = sv_int(th, Of("Thermostat", "actualTemp"), -999);
        if (tgt == -999 || act == -999) { task_end(j, now, false, "cannot read the thermostat values", th); return; }
        int diff = act > tgt ? act - tgt : tgt - act;
        if (diff == 0) { j.t = now + 450; if (++j.tries > 12) task_end(j, now, false, "temperature equal but not matched", th); return; }
        if (j.n2 > 0 && static_cast<float>(diff) > j.a2) { j.dest ^= 1; spy_note("closeout: the temperature moved the wrong way -> buttons swapped"); }
        if (j.n2 > 90) { task_end(j, now, false, "too many presses", th); return; }
        bool up = (act < tgt) ^ (j.dest != 0);
        void* btn = nullptr;
        for (void* b : find_objects(Kc("ThermostatButton")))
            if (rd_ptr(b, Of("ThermostatButton", "parent")) == th && (rd_bool(b, Of("ThermostatButton", "isUpButton")) == up)) { btn = b; break; }
        if (!btn) { task_end(j, now, false, "no thermostat button found", th); return; }
        interact("ThermostatButton", btn);
        spy_note("closeout: thermostat actual %d -> target %d, press %s", act, tgt, up ? "UP" : "DOWN");
        j.a2 = static_cast<float>(diff); j.n2++; j.t = now + 300;
        return;
    }
    case 20: {                                                   // rat trap
        if (now < j.t) return;
        if (is_gone(j.a) || sv_bool(j.a, Of("RatTrap", "isTriggered")) != 1) { task_end(j, now, true, "rat trap reset"); return; }
        if (j.tries == 0 || now - j.t3 >= 1500) {                // checked every 0.2 s, pressed again only after 1.5 s without result
            if (j.tries >= 3) { task_end(j, now, false, "rat trap did not reset", j.a); return; }
            interact("RatTrap", j.a);
            j.tries++; j.t3 = now;
            spy_note("closeout: RatTrap.Interact (try %d)", j.tries);
        }
        j.t = now + 200;
        return;
    }
    case 30: {                                                   // appliance: turn the knob until Off
        if (now < j.t) return;
        int st = -1;
        if (j.n2 == 0) { int off = Of("Fryer", "temperatureState"); st = off >= 0 ? rd_int(j.a, off) : -1; }
        else st = oven_temp(j.a, j.n2 - 1);
        void* key = j.n2 == 0 ? j.a : reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(j.a) + static_cast<uintptr_t>(j.n2 - 1));
        if (st <= 0) { task_end(j, now, true, "appliance is off"); return; }
        if (j.tries == 0 || now - j.t3 >= 1100) {                // checked every 0.2 s, pressed again only after 1.1 s
            if (j.tries >= 6) { task_end(j, now, false, "could not turn the appliance off", key); return; }
            if (j.n2 == 0) interact("FryerControl", near_obj("FryerControl", j.a, -1, 0, false));
            else call0(Mt("Oven", j.n2 == 1 ? "TopKnobControl" : "BottomKnobControl", 0), j.a);
            j.tries++; j.t3 = now;
            spy_note("closeout: appliance state %d -> knob press %d", st, j.tries);
        }
        j.t = now + 200;
        return;
    }
    case 40: {                                                   // carry: take the object
        if (!hand_acquire(j, now)) return;
        j.a = component_of(j.item, k_generic);
        if (!j.a) j.a = j.item;
        if (!in_held(j.a)) {
            if (free_slot() < 0) { j.status = "waiting for a free hotbar slot"; if (now - j.t2 > 20000) task_end(j, now, false, "no free hotbar slot", j.item); return; }
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1);
            void* pl = local_player();
            void* exc = nullptr;
            void* a[] = { j.a };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("closeout: HotbarGrab('%s') %s", clean(uname(j.item)).c_str(), exc ? "threw" : "called");
        }
        j.t3 = now; j.step = 41;
        return;
    }
    case 41: {
        if (!in_held(j.a) && now - j.t3 < 700) return;        // polled every tick: continue as soon as the item is in the hotbar
        if (in_held(j.a)) { hotbar_activate(j.a, "closeout"); spy_note("closeout: '%s' is in the hand", clean(uname(j.item)).c_str()); }
        else spy_note("closeout: '%s' did not go into the hotbar - it is moved directly", clean(uname(j.item)).c_str());
        j.step = 42;
        return;
    }
    case 42: {                                                   // carry: throw it above the destination
        void* tgt = nullptr;
        if (j.n == 5 || j.n == 6 || j.n == 7) tgt = trash_zone(j.item, j.tries);
        else if (j.n == 8 || j.n == 9) tgt = j.b;
        else tgt = restock_zone(j.item, j.tries);
        float tp[3];
        if (!tgt || !pos_of(tgt, tp)) { task_end(j, now, false, "no destination zone found in the world", j.item); return; }
        throw_above(j, j.a, tgt, 0.5f, j.tries);
        j.t = now + 1200; j.step = 43; j.status = "thrown to the destination";
        return;
    }
    case 43: {
        if (carry_done(j)) { task_end(j, now, true, "delivered"); return; }
        if (now < j.t) return;
        if (j.tries == 2 && (j.n == 8 || j.n == 9)) interact(j.n == 8 ? "NapkinDispenser" : "ACUnit", j.b);
        if (++j.tries >= 5) { task_end(j, now, false, "the destination did not accept the object", j.item); return; }
        j.step = 42;
        return;
    }
    case 50: {                                                   // fires: take an extinguisher
        if (!hand_acquire(j, now)) return;
        void* best = nullptr; void* bestGen = nullptr; float bd = 1e30f; float pp[3]; player_pos(pp);
        for (void* e : find_objects(Kc("FireExtinguisher"))) {
            void* gen = component_of(e, k_generic);
            if (!gen) continue;
            float q[3] = {}; float d = pos_of(e, q) ? dist(q, pp) : 1e20f;
            if (in_held(gen)) d = -1.0f;
            if (d < bd) { bd = d; best = e; bestGen = gen; }
        }
        if (!best) { j.mask |= (1 << 1); hand_release(j); spy_note("closeout: no FireExtinguisher in the world - fires disabled"); j.step = 1; return; }
        j.item = best; j.a = bestGen;
        if (!in_held(bestGen)) {
            if (free_slot() < 0) { j.status = "waiting for a free hotbar slot"; return; }
            void* m = Mt("ScytheNetworkPlayer", "HotbarGrab", 1); void* pl = local_player(); void* exc = nullptr; void* a[] = { bestGen };
            if (m && pl) il2::runtime_invoke(m, pl, a, &exc);
            spy_note("closeout: HotbarGrab(fire extinguisher) %s", exc ? "threw" : "called");
        }
        j.t = now + 700; j.step = 51;
        return;
    }
    case 51: {
        if (now < j.t) return;
        if (!in_held(j.a)) { j.mask |= (1 << 1); hand_release(j); spy_note("closeout: the extinguisher did not go into the hand - fires disabled"); j.step = 1; return; }
        hotbar_activate(j.a, "closeout");
        j.b = nullptr; j.n2 = 0; j.t = now + 400; j.step = 52;
        return;
    }
    case 52: {                                                   // spray the fire: the extinguisher's own ray RPC, aimed down at the fire
        j.next = 0;                                              // run every frame while spraying
        void* fire = j.b;
        if (fire && (is_gone(fire) || sv_bool(fire, Of("Fire", "extinguished")) == 1)) {
            spy_note("closeout: fire extinguished (%.1f s)", static_cast<double>(now - j.t3) / 1000.0);
            if (j.n2) { call0(Mt("FireExtinguisher", "DeUse", 0), j.item); j.n2 = 0; }
            fire = j.b = nullptr;
        }
        if (!fire) {
            float pp[3]; player_pos(pp);
            std::vector<void*> f;
            for (void* o : find_objects(Kc("Fire"))) if (sv_bool(o, Of("Fire", "extinguished")) != 1 && !s_closeSkip.count(o)) f.push_back(o);
            if (f.empty()) { j.step = 53; return; }
            fire = j.b = nearest_of(f, pp); j.t3 = now; j.t = now;
            spy_note("closeout: next fire (amount %.2f), %d left", static_cast<double>(sv_float(fire, Of("Fire", "fireAmount"), -1.0f)), static_cast<int>(f.size()));
        }
        if (now - j.t3 > 15000) { s_closeSkip.insert(fire); spy_note("closeout: this fire did not go out in 15 s - skipped"); if (j.n2) { call0(Mt("FireExtinguisher", "DeUse", 0), j.item); j.n2 = 0; } j.cfail[1]++; j.b = nullptr; return; }
        if (!j.n2) { call0(Mt("FireExtinguisher", "Use", 0), j.item); j.n2 = 1; spy_note("closeout: FireExtinguisher.Use()"); }
        if (now < j.t) return;
        float fp[3] = {};
        if (pos_of(fire, fp)) {
            float o[3] = { fp[0], fp[1] + 0.9f, fp[2] }, d[3] = { 0.0f, -1.0f, 0.0f };
            void* a[] = { o, d };
            void* exc = nullptr;
            il2::runtime_invoke(Mt("FireExtinguisher", "ServerUpdateSprayRay", 2), j.item, a, &exc);
            if (exc) { spy_note("closeout: ServerUpdateSprayRay threw"); j.mask |= (1 << 1); j.step = 53; return; }
        }
        j.t = now + 50;
        return;
    }
    case 53: {                                                   // put the extinguisher down
        j.next = 0;
        if (j.n2) { call0(Mt("FireExtinguisher", "DeUse", 0), j.item); j.n2 = 0; }
        float pp[3]; player_pos(pp);
        float t[3] = { pp[0], pp[1] + 1.0f, pp[2] };
        throw_to(j.a, t, "closeout");
        task_end(j, now, true, "fires done, extinguisher put down");
        return;
    }
    }
}

// ------------------------------- engine --------------------------------------
int add_job(int kind, const std::string& label, const std::string& param, ULONGLONG delay) {
    Job j;
    j.id = s_nextJob++; j.kind = kind; j.label = label; j.param = param;
    j.started = GetTickCount64(); j.t = j.started + delay; j.status = "queued";
    if (s_forceAnc) { j.anc[0] = s_forceAnc[0]; j.anc[1] = s_forceAnc[1]; j.anc[2] = s_forceAnc[2]; }   // sub-jobs of alpha
    else player_pos(j.anc);                                                                               // everything is chosen near the player who started it
    if (kind == J_TRAY) j.n = -1;
    s_jobList.push_back(j);
    spy_note("job #%d '%s' started", j.id, label.c_str());
    return j.id;
}

// Items of the order the cook jobs should work on: the oldest ACCEPTED order; the register text only as a fallback.
std::vector<std::string> order_items_now() {
    std::vector<AcceptedOrder> acc;
    features_accepted_orders(acc);
    for (const AcceptedOrder& a : acc) if (!s_handledOrders.count(a.key) && (a.type != 1 || g.alphaDrive.load())) return a.items;   // oldest order that alpha is not building
    if (!acc.empty()) return acc.front().items;
    std::vector<std::string> v;
    features_order_items(v);
    return v;
}

void handle_request(const std::string& r) {
    if (r == "cancel") {
        for (const Job& j : s_jobList) if (!j.done) spy_note("job #%d cancelled", j.id);
        s_jobList.clear();
        s_slotRes[0] = s_slotRes[1] = false;
        s_hand = 0;
        s_handledOrders.clear();
    } else if (r == "closeout") {
        for (const Job& o : s_jobList) if (o.kind == J_CLOSE && !o.done) { spy_note("closeout: already running (job #%d)", o.id); return; }
        add_job(J_CLOSE, "BETA CLOSEOUT (after-shift duties)", "");
    } else if (r.rfind("alpha:", 0) == 0) {
        for (const Job& o : s_jobList) if (o.kind == J_ALPHA && !o.done) { spy_note("alpha: already running (job #%d)", o.id); return; }
        add_job(J_ALPHA, "ALPHA COOKING (order #" + r.substr(6) + ")", r.substr(6));
    } else if (r == "alpha") {
        for (const Job& o : s_jobList) if (o.kind == J_ALPHA && !o.done) { spy_note("alpha: already running (job #%d)", o.id); return; }
        add_job(J_ALPHA, "ALPHA COOKING (whole order)", "");
    } else if (r == "tray:cookie") add_job(J_TRAY, "Oven: cookie tray", "cookie");
    else if (r == "tray:pie") add_job(J_TRAY, "Oven: pie tray", "pie");
    else if (r.rfind("shake:", 0) == 0) {
        std::string d = r.substr(6);
        if (d == "order") {
            std::vector<std::string> items;
            items = order_items_now();
            int n = 0;
            for (auto& it : items) { int sk = shake_kind(it); if (sk) { add_job(J_SHAKE, "Shake: " + it, sk == 1 ? "strawberry" : "blueberry"); n++; } }
            if (!n) spy_note("shake: no shake found in the accepted order");
        } else add_job(J_SHAKE, "Shake: " + d, d);
    } else if (r.rfind("patty:", 0) == 0) add_job(J_PATTY, "Auto-cook " + r.substr(6), r.substr(6));
    else if (r == "fries" || r == "nuggets") add_job(J_FRY, std::string("Fryer batch: ") + r, r);
    else if (r == "trays") { add_job(J_TRAY, "Oven: cookie tray", "cookie"); add_job(J_TRAY, "Oven: pie tray", "pie", 4500); }
    else if (r == "stock") start_stock("button");
    else if (r.rfind("bag:", 0) == 0) add_job(J_BAG, "To bag: " + r.substr(4), r.substr(4));
    else if (r.rfind("soda:", 0) == 0) {
        std::string d = r.substr(5);
        if (d == "order") {
            std::vector<std::string> items;
            items = order_items_now();
            int n = 0;
            for (auto& it : items) { int di = drink_index(it); if (di >= 0) { add_job(J_SODA, std::string("Soda: ") + kDrinks[di], kDrinks[di]); n++; } }
            if (!n) spy_note("soda: no drink found in the accepted order");
            return;
        }
        add_job(J_SODA, "Soda: " + d, d);
    }
}

void jobs_tick() {
    std::vector<std::string> reqs;
    { std::lock_guard<std::mutex> lk(s_jobMtx); reqs.swap(s_reqs); }
    if (!reqs.empty() || !s_jobList.empty()) resolve();
    for (auto& r : reqs) handle_request(r);
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < s_jobList.size(); i++) {
        Job& j = s_jobList[i];
        if (j.done || now < j.next) continue;
        j.next = now + 60;
        switch (j.kind) {
        case J_PATTY: step_patty(j, now); break;
        case J_FRY:   step_fry(j, now); break;
        case J_TRAY:  step_tray(j, now); break;
        case J_BAG:   step_bag(j, now); break;
        case J_SODA:  step_soda(j, now); break;
        case J_SHAKE: step_shake(j, now); break;
        case J_INGR:  step_ingr(j, now); break;
        case J_SAUCE: step_sauce(j, now); break;
        case J_CLOSE: step_close(j, now); break;
        case J_ALPHA: s_forceAnc = j.anc; step_alpha(j, now); s_forceAnc = nullptr; break;
        }
    }
    for (size_t i = 0; i < s_jobList.size();)
        if (s_jobList[i].done && now - s_jobList[i].finished > 12000) s_jobList.erase(s_jobList.begin() + static_cast<std::ptrdiff_t>(i));
        else i++;
    std::vector<std::string> lines;
    for (const Job& j : s_jobList) {
        char b[220];
        snprintf(b, sizeof(b), "#%d %s: %s", j.id, j.label.c_str(), j.status.c_str());
        lines.push_back(b);
    }
    std::lock_guard<std::mutex> lk(s_jobMtx);
    s_jobLines.swap(lines);
}

void run_steps() {
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < s_queue.size();) {
        if (s_queue[i].at > now) { i++; continue; }
        Step st = s_queue[i];
        s_queue.erase(s_queue.begin() + static_cast<std::ptrdiff_t>(i));
        if (st.kind == 10) cmd_report();
        else if (st.kind == 11) {
            void* tl = o_tracked >= 0 && st.obj ? rd_ptr(st.obj, o_tracked) : nullptr;
            logf("lab[2]: grill now tracks %d item(s) (was the item accepted by the trigger?)", tl ? rd_int(tl, o_listSize) : -1);
        }
        else if (st.kind == 12) phase_board(st.obj);
        else if (st.kind == 13) {
            float mp[3] = {};
            pos_of(st.obj, mp);
            logf("lab[4]: 1 s later '%s' is at (%.2f, %.2f, %.2f) - check the log for AssemblyBoardTrigger / BuildBurger", uname(st.obj).c_str(),
                 static_cast<double>(mp[0]), static_cast<double>(mp[1]), static_cast<double>(mp[2]));
        }
    }
}

void lab_inner() {
    int cmd = g.labCmd.exchange(0);
    bool want = g.labVisible.exchange(0) != 0;
    bool jobsActive = !s_jobList.empty();
    if (!jobsActive) { std::lock_guard<std::mutex> lk(s_jobMtx); jobsActive = !s_reqs.empty(); }
    if (!cmd && s_queue.empty() && !want && !jobsActive) return;
    resolve();
    if (want && GetTickCount64() - s_boxLast > 2000) { s_boxLast = GetTickCount64(); refresh_boxes(); }
    if (cmd == 1) cmd_take();
    else if (cmd == 2) cmd_drop_grill();
    else if (cmd == 3) cmd_toggle_grill();
    else if (cmd == 4) cmd_cooked_to_board();
    else if (cmd == 5) cmd_report();
    run_steps();
    jobs_tick();
}

}  // namespace

void lab_tick() {
    __try { lab_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("lab: SEH exception (code %08lx) - commands and jobs aborted", GetExceptionCode());
        s_queue.clear();
        s_jobList.clear();
        s_slotRes[0] = s_slotRes[1] = false;
        s_hand = 0;
    }
}

bool lab_box_name(int i, char* out, int cap) {
    std::lock_guard<std::mutex> lk(s_boxMtx);
    if (i < 0 || i >= static_cast<int>(s_boxNames.size())) return false;
    snprintf(out, static_cast<size_t>(cap), "%s", s_boxNames[static_cast<size_t>(i)].c_str());
    return true;
}

void cook_request(const char* cmd) {
    std::lock_guard<std::mutex> lk(s_jobMtx);
    s_reqs.push_back(cmd ? cmd : "");
}
bool cook_order_busy(void* key) { return key && s_handledOrders.count(key) != 0; }

bool cook_job_line(int i, char* out, int cap) {
    std::lock_guard<std::mutex> lk(s_jobMtx);
    if (i < 0 || i >= static_cast<int>(s_jobLines.size())) return false;
    snprintf(out, static_cast<size_t>(cap), "%s", s_jobLines[static_cast<size_t>(i)].c_str());
    return true;
}
bool cook_patty_name(int i, char* out, int cap) {
    std::lock_guard<std::mutex> lk(s_boxMtx);
    if (i < 0 || i >= static_cast<int>(s_patties.size())) return false;
    snprintf(out, static_cast<size_t>(cap), "%s", s_patties[static_cast<size_t>(i)].c_str());
    return true;
}
