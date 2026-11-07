/* Platform-independent dump logic. Uses only il2cpp_* exports resolved at runtime.
 * Read-only: it never writes to game memory and never hooks anything. */
#ifndef DUMPER_H
#define DUMPER_H

typedef unsigned long long u64;
typedef unsigned int u32;

/* ---- provided by the platform layer (scout.c / test.c) ---- */
void plat_write(void *h, const char *data, u32 len);
void *plat_getproc(void *module, const char *name);

/* ---- tiny buffered output ---- */
typedef struct {
    void *h;
    u32 n;
    char buf[32768];
} Out;

static void out_flush(Out *o) {
    if (o->n) {
        plat_write(o->h, o->buf, o->n);
        o->n = 0;
    }
}
static void out_ch(Out *o, char c) {
    if (o->n >= sizeof(o->buf)) out_flush(o);
    o->buf[o->n++] = c;
}
static void out_s(Out *o, const char *s) {
    if (!s) s = "(null)";
    while (*s) out_ch(o, *s++);
}
static void out_u(Out *o, u64 v) {
    char t[24];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out_ch(o, t[--n]);
}
static void out_x(Out *o, u64 v) {
    const char *d = "0123456789abcdef";
    char t[24];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = d[v & 15]; v >>= 4; }
    out_s(o, "0x");
    while (n) out_ch(o, t[--n]);
}

static int lower_c(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int contains_ci(const char *h, const char *n) {
    if (!h || !n) return 0;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && lower_c(*a) == lower_c(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- il2cpp API (all exported by GameAssembly.dll) ---- */
static void *(*il2cpp_domain_get)(void);
static void *(*il2cpp_thread_attach)(void *);
static void **(*il2cpp_domain_get_assemblies)(void *, u64 *);
static void *(*il2cpp_assembly_get_image)(void *);
static const char *(*il2cpp_image_get_name)(void *);
static u64 (*il2cpp_image_get_class_count)(void *);
static void *(*il2cpp_image_get_class)(void *, u64);
static const char *(*il2cpp_class_get_name)(void *);
static const char *(*il2cpp_class_get_namespace)(void *);
static void *(*il2cpp_class_get_parent)(void *);
static void *(*il2cpp_class_get_fields)(void *, void **);
static void *(*il2cpp_class_get_methods)(void *, void **);
static const char *(*il2cpp_field_get_name)(void *);
static void *(*il2cpp_field_get_type)(void *);
static u64 (*il2cpp_field_get_offset)(void *);
static int (*il2cpp_field_get_flags)(void *);
static char *(*il2cpp_type_get_name)(void *);
static void (*il2cpp_free)(void *);
static const char *(*il2cpp_method_get_name)(void *);
static u32 (*il2cpp_method_get_param_count)(void *);
static void *(*il2cpp_method_get_return_type)(void *);
static void *(*il2cpp_method_get_param)(void *, u32);
static const char *(*il2cpp_method_get_param_name)(void *, u32);
static u32 (*il2cpp_method_get_flags)(void *, u32 *);
static void *(*il2cpp_class_from_name)(void *, const char *, const char *);
static void *(*il2cpp_class_get_field_from_name)(void *, const char *);
static void (*il2cpp_field_static_get_value)(void *, void *);

#define RESOLVE(mod, log, missing, name)                                  \
    do {                                                                  \
        name = (__typeof__(name))plat_getproc(mod, #name);                \
        if (!name) {                                                      \
            out_s(log, "MISSING export: " #name "\n");                    \
            missing++;                                                    \
        }                                                                 \
    } while (0)

/* Returns number of REQUIRED exports that are missing (il2cpp_free is optional). */
static int resolve_api(void *mod, Out *log) {
    int missing = 0;
    RESOLVE(mod, log, missing, il2cpp_domain_get);
    RESOLVE(mod, log, missing, il2cpp_thread_attach);
    RESOLVE(mod, log, missing, il2cpp_domain_get_assemblies);
    RESOLVE(mod, log, missing, il2cpp_assembly_get_image);
    RESOLVE(mod, log, missing, il2cpp_image_get_name);
    RESOLVE(mod, log, missing, il2cpp_image_get_class_count);
    RESOLVE(mod, log, missing, il2cpp_image_get_class);
    RESOLVE(mod, log, missing, il2cpp_class_get_name);
    RESOLVE(mod, log, missing, il2cpp_class_get_namespace);
    RESOLVE(mod, log, missing, il2cpp_class_get_parent);
    RESOLVE(mod, log, missing, il2cpp_class_get_fields);
    RESOLVE(mod, log, missing, il2cpp_class_get_methods);
    RESOLVE(mod, log, missing, il2cpp_field_get_name);
    RESOLVE(mod, log, missing, il2cpp_field_get_type);
    RESOLVE(mod, log, missing, il2cpp_field_get_offset);
    RESOLVE(mod, log, missing, il2cpp_field_get_flags);
    RESOLVE(mod, log, missing, il2cpp_type_get_name);
    RESOLVE(mod, log, missing, il2cpp_method_get_name);
    RESOLVE(mod, log, missing, il2cpp_method_get_param_count);
    RESOLVE(mod, log, missing, il2cpp_method_get_return_type);
    RESOLVE(mod, log, missing, il2cpp_method_get_param);
    RESOLVE(mod, log, missing, il2cpp_method_get_param_name);
    RESOLVE(mod, log, missing, il2cpp_method_get_flags);
    RESOLVE(mod, log, missing, il2cpp_class_from_name);
    RESOLVE(mod, log, missing, il2cpp_class_get_field_from_name);
    RESOLVE(mod, log, missing, il2cpp_field_static_get_value);
    il2cpp_free = (void (*)(void *))plat_getproc(mod, "il2cpp_free");
    return missing;
}

/* ---- what to dump ---- */
/* Classes whose NAME contains one of these (case-insensitive). */
static const char *CLASS_KW[] = {
    "Player", "Order", "Kitchen", "Appliance", "Cook", "Grill", "Fryer", "Oven",
    "Microwave", "Soda", "Shake", "Time", "Customer", "Shop", "Upgrade", "Stat",
    "Credit", "Money", "Shift", "Round", "Ticket", "Burger", "Patty", "Bag",
    "Recipe", "Stamina", "Cultist", "Happy", "DevConsole", "CharacterMovement", 0};
/* Classes having a FIELD or METHOD whose name contains one of these. */
static const char *MEMBER_KW[] = {
    "stamina", "timescale", "sprint", "movespeed", "walkspeed", "runspeed",
    "speedlevel", "maxspeed", 0};

static int any_kw(const char *s, const char **kw) {
    for (; *kw; kw++)
        if (contains_ci(s, *kw)) return 1;
    return 0;
}

static int class_matches(void *k) {
    void *it, *x;
    if (any_kw(il2cpp_class_get_name(k), CLASS_KW)) return 1;
    it = 0;
    while ((x = il2cpp_class_get_fields(k, &it)))
        if (any_kw(il2cpp_field_get_name(x), MEMBER_KW)) return 1;
    it = 0;
    while ((x = il2cpp_class_get_methods(k, &it)))
        if (any_kw(il2cpp_method_get_name(x), MEMBER_KW)) return 1;
    return 0;
}

static void out_type(Out *o, void *t) {
    char *n;
    if (!t) { out_s(o, "?"); return; }
    n = il2cpp_type_get_name(t);
    out_s(o, n);
    if (n && il2cpp_free) il2cpp_free(n);
}

static void out_class_name(Out *o, void *k) {
    const char *ns = il2cpp_class_get_namespace(k);
    if (ns && *ns) { out_s(o, ns); out_ch(o, '.'); }
    out_s(o, il2cpp_class_get_name(k));
}

static void dump_class(Out *d, void *k) {
    void *it, *p, *x;
    out_s(d, "\nclass ");
    out_class_name(d, k);
    p = il2cpp_class_get_parent(k);
    if (p) { out_s(d, " : "); out_class_name(d, p); }
    out_ch(d, '\n');

    it = 0;
    while ((x = il2cpp_class_get_fields(k, &it))) {
        out_s(d, "  field  ");
        out_type(d, il2cpp_field_get_type(x));
        out_ch(d, ' ');
        out_s(d, il2cpp_field_get_name(x));
        out_s(d, "  offset=");
        out_x(d, il2cpp_field_get_offset(x));
        if (il2cpp_field_get_flags(x) & 0x10) out_s(d, "  static");
        out_ch(d, '\n');
    }
    it = 0;
    while ((x = il2cpp_class_get_methods(k, &it))) {
        u32 pc = il2cpp_method_get_param_count(x), i, iflags = 0;
        u32 fl = il2cpp_method_get_flags(x, &iflags);
        out_s(d, "  method ");
        out_type(d, il2cpp_method_get_return_type(x));
        out_ch(d, ' ');
        out_s(d, il2cpp_method_get_name(x));
        out_ch(d, '(');
        for (i = 0; i < pc; i++) {
            if (i) out_s(d, ", ");
            out_type(d, il2cpp_method_get_param(x, i));
            out_ch(d, ' ');
            out_s(d, il2cpp_method_get_param_name(x, i));
        }
        out_ch(d, ')');
        if (fl & 0x10) out_s(d, "  static");
        out_ch(d, '\n');
    }
}

/* Walk every class in the image: all names -> classes, matches -> details. */
static void run_dump(void *image, Out *log, Out *classes, Out *details) {
    u64 n = il2cpp_image_get_class_count(image), i, matched = 0;
    out_s(log, "classes in image: ");
    out_u(log, n);
    out_ch(log, '\n');
    out_flush(log);
    for (i = 0; i < n; i++) {
        void *k = il2cpp_image_get_class(image, i);
        void *p;
        if (!k) continue;
        out_class_name(classes, k);
        p = il2cpp_class_get_parent(k);
        if (p) { out_s(classes, " : "); out_class_name(classes, p); }
        out_ch(classes, '\n');
        if (class_matches(k)) {
            dump_class(details, k);
            matched++;
        }
        if ((i % 250) == 0) {
            out_flush(classes);
            out_flush(details);
            out_s(log, "progress ");
            out_u(log, i);
            out_ch(log, '/');
            out_u(log, n);
            out_ch(log, '\n');
            out_flush(log);
        }
    }
    out_flush(classes);
    out_flush(details);
    out_s(log, "DONE. detailed classes: ");
    out_u(log, matched);
    out_ch(log, '\n');
    out_flush(log);
}

/* Find the game's own assembly image, or 0 if not loaded yet. */
static void *find_game_image(void *domain) {
    u64 count = 0, i;
    void **asms = il2cpp_domain_get_assemblies(domain, &count);
    if (!asms) return 0;
    for (i = 0; i < count; i++) {
        void *img = il2cpp_assembly_get_image(asms[i]);
        if (img && str_eq(il2cpp_image_get_name(img), "Assembly-CSharp.dll")) return img;
    }
    return 0;
}

#endif
