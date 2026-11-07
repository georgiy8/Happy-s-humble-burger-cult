/* Mod logic (platform independent): reads ScytheNetworkPlayer.instance and applies toggles
 * by writing float fields. Field offsets are looked up BY NAME at runtime through the
 * il2cpp API, so nothing is hard-coded. No hooks, no method calls into the game. */
#ifndef MOD_H
#define MOD_H
#include "dumper.h"

typedef struct {
    /* controls (written by UI thread, read by worker) */
    volatile int inf_stamina;
    volatile int speed_on;
    volatile int speed_pct; /* 50..400 */
    const char *status;
    /* resolved */
    void *f_inst;
    int ready;
    int o_cur, o_max, o_drain, o_walk, o_run;
    /* per-instance state */
    void *last;
    float orig_walk, orig_run, orig_drain;
    int a_stam, a_speed;
} Mod;

static float *fptr(void *obj, int off) { return (float *)((char *)obj + off); }
static int plausible(float v) { return v == v && v > 0.0f && v < 100000.0f; }

static int field_off(void *klass, const char *name, Out *log) {
    void *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) {
        out_s(log, "mod: field not found: ");
        out_s(log, name);
        out_ch(log, '\n');
        out_flush(log);
        return -1;
    }
    return (int)il2cpp_field_get_offset(f);
}

/* returns 1 when all needed fields were found */
static int mod_init(Mod *m, void *image, Out *log) {
    void *k = il2cpp_class_from_name(image, "", "ScytheNetworkPlayer");
    m->ready = 0;
    m->status = "Player: waiting";
    if (!k) {
        out_s(log, "mod: class ScytheNetworkPlayer not found\n");
        out_flush(log);
        return 0;
    }
    m->f_inst = il2cpp_class_get_field_from_name(k, "instance");
    m->o_cur = field_off(k, "currentSprintAbility", log);
    m->o_max = field_off(k, "maxSprintAbility", log);
    m->o_drain = field_off(k, "sprintDrainRate", log);
    m->o_walk = field_off(k, "defaultMaxWalkSpeed", log);
    m->o_run = field_off(k, "defaultMaxRunSpeed", log);
    if (!m->f_inst || m->o_cur < 0 || m->o_max < 0 || m->o_drain < 0 || m->o_walk < 0 || m->o_run < 0) {
        out_s(log, "mod: missing fields, mod disabled\n");
        out_flush(log);
        return 0;
    }
    out_s(log, "mod: fields resolved (cur/max/drain/walk/run offsets): ");
    out_x(log, (u64)m->o_cur); out_ch(log, ' ');
    out_x(log, (u64)m->o_max); out_ch(log, ' ');
    out_x(log, (u64)m->o_drain); out_ch(log, ' ');
    out_x(log, (u64)m->o_walk); out_ch(log, ' ');
    out_x(log, (u64)m->o_run); out_ch(log, '\n');
    out_flush(log);
    m->ready = 1;
    return 1;
}

/* One iteration; call about every 50 ms from the worker thread. */
static void mod_tick(Mod *m) {
    void *p = 0;
    float mult;
    if (!m->ready) return;
    il2cpp_field_static_get_value(m->f_inst, &p);
    if (!p) { m->status = "Player: not in a match"; m->last = 0; m->a_stam = m->a_speed = 0; return; }

    if (p != m->last) { /* new player object: remember the game's own values */
        m->last = p;
        m->a_stam = m->a_speed = 0;
        m->orig_walk = *fptr(p, m->o_walk);
        m->orig_run = *fptr(p, m->o_run);
        m->orig_drain = *fptr(p, m->o_drain);
    }
    if (!plausible(m->orig_walk) || !plausible(m->orig_run)) {
        m->status = "Player: values look wrong, paused";
        return;
    }
    m->status = "Player: found";

    if (m->inf_stamina) {
        if (!m->a_stam) m->a_stam = 1;
        *fptr(p, m->o_drain) = 0.0f;
        *fptr(p, m->o_cur) = *fptr(p, m->o_max);
    } else if (m->a_stam) {
        *fptr(p, m->o_drain) = m->orig_drain;
        m->a_stam = 0;
    }

    mult = (float)m->speed_pct / 100.0f;
    if (mult < 0.5f) mult = 0.5f;
    if (mult > 4.0f) mult = 4.0f;
    if (m->speed_on) {
        m->a_speed = 1;
        *fptr(p, m->o_walk) = m->orig_walk * mult;
        *fptr(p, m->o_run) = m->orig_run * mult;
    } else if (m->a_speed) {
        *fptr(p, m->o_walk) = m->orig_walk;
        *fptr(p, m->o_run) = m->orig_run;
        m->a_speed = 0;
    }
}
#endif
