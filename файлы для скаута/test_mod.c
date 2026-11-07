/* Host test of mod.h with a fake player object. Not shipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mod.h"

void plat_write(void *h, const char *d, u32 n) { fwrite(d, 1, n, (FILE *)h); }
void *plat_getproc(void *m, const char *n) { (void)m; (void)n; return 0; }

static char player[4096]; static void *g_player = 0;
static const char *names[] = {"instance","currentSprintAbility","maxSprintAbility","sprintDrainRate","defaultMaxWalkSpeed","defaultMaxRunSpeed",0};
static unsigned offs[] = {0,0x74c,0x748,0x750,0x730,0x734};
static void *f_cfn(void *k, const char *n){ (void)k; for(int i=0;names[i];i++) if(!strcmp(n,names[i])) return (void*)(long)(i+1); return 0; }
static u64 f_off(void *f){ return offs[(long)f-1]; }
static void f_static(void *f, void *out){ (void)f; *(void**)out = g_player; }
static void *f_cfrom(void *img,const char*ns,const char*n){ (void)img;(void)ns; return strcmp(n,"ScytheNetworkPlayer")?0:(void*)1; }
#define F(o) (*(float*)(player+(o)))
static int fails=0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
int main(void){
    Out log={stdout}; Mod m; memset(&m,0,sizeof m);
    il2cpp_class_from_name=f_cfrom; il2cpp_class_get_field_from_name=f_cfn;
    il2cpp_field_get_offset=f_off; il2cpp_field_static_get_value=f_static;
    CHECK(mod_init(&m,(void*)1,&log));
    F(0x730)=4.0f; F(0x734)=7.0f; F(0x750)=10.0f; F(0x748)=100.0f; F(0x74c)=20.0f;
    m.speed_pct=100;
    g_player=0; mod_tick(&m); CHECK(!strcmp(m.status,"Player: not in a match"));
    g_player=player; mod_tick(&m); CHECK(!strcmp(m.status,"Player: found"));
    CHECK(F(0x74c)==20.0f && F(0x750)==10.0f && F(0x730)==4.0f);          /* nothing on: untouched */
    m.inf_stamina=1; mod_tick(&m);
    CHECK(F(0x74c)==100.0f && F(0x750)==0.0f);                            /* stamina full, drain 0 */
    m.speed_on=1; m.speed_pct=200; mod_tick(&m);
    CHECK(F(0x730)==8.0f && F(0x734)==14.0f);                             /* x2 */
    mod_tick(&m); mod_tick(&m);
    CHECK(F(0x730)==8.0f && F(0x734)==14.0f);                             /* no compounding */
    m.speed_pct=150; mod_tick(&m); CHECK(F(0x730)==6.0f && F(0x734)==10.5f);
    m.speed_pct=9999; mod_tick(&m); CHECK(F(0x730)==16.0f);               /* clamped to x4 */
    m.speed_on=0; mod_tick(&m); CHECK(F(0x730)==4.0f && F(0x734)==7.0f);  /* restored */
    m.inf_stamina=0; mod_tick(&m); CHECK(F(0x750)==10.0f);                /* drain restored */
    /* new match: different object, different originals */
    static char player2[4096]; memcpy(player2,player,sizeof player);
    *(float*)(player2+0x730)=5.0f; *(float*)(player2+0x734)=9.0f; *(float*)(player2+0x750)=3.0f;
    g_player=player2; m.speed_on=1; m.speed_pct=200; mod_tick(&m);
    CHECK(*(float*)(player2+0x730)==10.0f && *(float*)(player2+0x734)==18.0f);
    /* garbage originals -> paused, no writes */
    static char player3[4096]; memset(player3,0,sizeof player3); g_player=player3; mod_tick(&m);
    CHECK(!strcmp(m.status,"Player: values look wrong, paused"));
    printf(fails? "TESTS FAILED\n":"all mod tests passed\n");
    return fails;
}
