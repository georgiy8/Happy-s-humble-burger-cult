/* Linux host test of dumper.h with a fake il2cpp runtime. Not shipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dumper.h"

void plat_write(void *h, const char *data, u32 len) { fwrite(data, 1, len, (FILE *)h); }
void *plat_getproc(void *m, const char *n) { (void)m; (void)n; return 0; }

typedef struct { const char *name; const char *type; unsigned off; int st; } F;
typedef struct { const char *name; const char *ret; int np; const char *pt[3]; const char *pn[3]; int st; } M;
typedef struct { const char *ns, *name; int parent; int nf, nm; F f[4]; M m[4]; } C;

static C classes[] = {
    {"", "PlayerController", 1, 2, 2,
     {{"staminaLevel", "System.Int32", 0x20, 0}, {"instance", "PlayerController", 0x0, 1}},
     {{"RestoreStamina", "System.Void", 1, {"System.Single"}, {"fraction"}, 0},
      {"Update", "System.Void", 0, {0}, {0}, 0}}},
    {"", "MonoBehaviour", -1, 0, 0, {{0}}, {{0}}},
    {"Game", "UnrelatedThing", 1, 1, 0, {{"x", "System.Int32", 8, 0}}, {{0}}},
    {"", "OrderManager", 1, 1, 1,
     {{"pendingOrders", "List<Order>", 0x30, 0}},
     {{"CheckPendingOrderTimeouts", "System.Void", 0, {0}, {0}, 0}}},
    {"", "Zzz", 1, 1, 0, {{"timeScaleCopy", "System.Single", 4, 0}}, {{0}}},
};
static void *CI(int i) { return (void *)(long)(i + 1); }
static C *get(void *k) { return &classes[(long)k - 1]; }
static char *dup(const char *s) { return strdup(s); }

static const char *f_cn(void *k) { return get(k)->name; }
static const char *f_cns(void *k) { return get(k)->ns; }
static void *f_cp(void *k) { int p = get(k)->parent; return p < 0 ? 0 : CI(p); }
static void *f_cf(void *k, void **it) {
    long i = (long)*it; C *c = get(k);
    if (i >= c->nf) return 0;
    *it = (void *)(i + 1);
    return &c->f[i];
}
static void *f_cm(void *k, void **it) {
    long i = (long)*it; C *c = get(k);
    if (i >= c->nm) return 0;
    *it = (void *)(i + 1);
    return &c->m[i];
}
static const char *f_fn(void *f) { return ((F *)f)->name; }
static void *f_ft(void *f) { return (void *)((F *)f)->type; }
static u64 f_fo(void *f) { return ((F *)f)->off; }
static int f_ff(void *f) { return ((F *)f)->st ? 0x10 : 0; }
static char *f_tn(void *t) { return dup((const char *)t); }
static void f_free(void *p) { free(p); }
static const char *f_mn(void *m) { return ((M *)m)->name; }
static u32 f_mpc(void *m) { return ((M *)m)->np; }
static void *f_mrt(void *m) { return (void *)((M *)m)->ret; }
static void *f_mp(void *m, u32 i) { return (void *)((M *)m)->pt[i]; }
static const char *f_mpn(void *m, u32 i) { return ((M *)m)->pn[i]; }
static u32 f_mf(void *m, u32 *fl) { (void)fl; return ((M *)m)->st ? 0x10 : 0; }
static u64 f_icc(void *img) { (void)img; return sizeof(classes) / sizeof(classes[0]); }
static void *f_ic(void *img, u64 i) { (void)img; return CI((int)i); }

int main(void) {
    Out log = {stdout}, cl = {stdout}, de = {stdout};
    il2cpp_class_get_name = f_cn; il2cpp_class_get_namespace = f_cns; il2cpp_class_get_parent = f_cp;
    il2cpp_class_get_fields = f_cf; il2cpp_class_get_methods = f_cm;
    il2cpp_field_get_name = f_fn; il2cpp_field_get_type = f_ft; il2cpp_field_get_offset = f_fo;
    il2cpp_field_get_flags = f_ff; il2cpp_type_get_name = f_tn; il2cpp_free = f_free;
    il2cpp_method_get_name = f_mn; il2cpp_method_get_param_count = f_mpc;
    il2cpp_method_get_return_type = f_mrt; il2cpp_method_get_param = f_mp;
    il2cpp_method_get_param_name = f_mpn; il2cpp_method_get_flags = f_mf;
    il2cpp_image_get_class_count = f_icc; il2cpp_image_get_class = f_ic;
    printf("--- LOG/CLASSES/DETAILS interleaved (all go to stdout) ---\n");
    run_dump((void *)1, &log, &cl, &de);
    return 0;
}
