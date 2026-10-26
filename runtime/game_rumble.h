#ifndef NFSU2_GAME_RUMBLE_H
#define NFSU2_GAME_RUMBLE_H
#include "cpu.h"
#include <math.h>
#include <string.h>
/* --- sub_00406460: ForceFeedback / Telemetri ve Fizik Titresim Kancasi ----
 * __thiscall(ecx = ForceFeedback* (normal Driver+0x220; DragDriver+0x690))
 * Gercek oyun ici carpisma, duvar surtmesi, bordur/toprak ve drift titresimi. */
#if defined(__APPLE__) || defined(NFS_RUMBLE_TEST)
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(NFS_RUMBLE_TEST) || TARGET_OS_IOS || TARGET_OS_SIMULATOR
extern void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness);
extern void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms);
extern void nfsu2_input_clear_rumble(void);
extern void nfsu2_input_note_rumble_hook(uint32_t gate);
#else
static inline void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness){
    (void)continuous_intensity; (void)continuous_sharpness;
}
static inline void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms){
    (void)intensity; (void)sharpness; (void)duration_ms;
}
static inline void nfsu2_input_clear_rumble(void){ }
static inline void nfsu2_input_note_rumble_hook(uint32_t gate){ (void)gate; }
#endif
#else
static inline void nfsu2_input_report_rumble(float continuous_intensity, float continuous_sharpness){
    (void)continuous_intensity; (void)continuous_sharpness;
}
static inline void nfsu2_input_trigger_impact(float intensity, float sharpness, uint32_t duration_ms){
    (void)intensity; (void)sharpness; (void)duration_ms;
}
static inline void nfsu2_input_clear_rumble(void){ }
static inline void nfsu2_input_note_rumble_hook(uint32_t gate){ (void)gate; }
#endif

void sub_00406460_orig(CPU *c);

static uint32_t s_prev_veh = 0;
static int s_has_prev = 0;
static uint32_t s_prev_sim_tick = 0;
static float s_prev_vx = 0.0f;
static float s_prev_vy = 0.0f;
static float s_prev_vz = 0.0f;
static float s_prev_speed = 0.0f;
static uint32_t s_prev_body_hit = 0;
static uint32_t s_prev_ground_mask = 0;
static uint32_t s_prev_surface[4];
static float s_prev_slip = 0;
static float s_cont_intensity = 0, s_cont_sharpness = 0;
static int s_prev_wall_wheels = 0;
static int s_airborne_ticks = 0;

static void rumble_reset_state(void){
    s_prev_veh = 0;
    s_has_prev = 0;
    s_prev_sim_tick = 0;
    s_prev_vx = 0.0f;
    s_prev_vy = 0.0f;
    s_prev_vz = 0.0f;
    s_prev_speed = 0.0f;
    s_prev_body_hit = 0;
    s_prev_ground_mask = 0;
    memset(s_prev_surface, 0, sizeof(s_prev_surface));
    s_prev_slip = 0;
    s_cont_intensity = s_cont_sharpness = 0;
    s_prev_wall_wheels = 0;
    s_airborne_ticks = 0;
}

static void nfsu2_game_rumble_sample_vehicle(uint32_t veh){
    if (!veh || veh > UINT32_MAX - 0x433u ||
        !mem_is_committed(veh) || !mem_is_committed(veh + 0x433u)) {
        nfsu2_input_note_rumble_hook(1u);
        return;
    }
    if (MR32(veh + 0x28u) != 1u) {
        nfsu2_input_note_rumble_hook(2u);
        return;
    }

    /* Aktif Yaris ve Yerel Oyuncu Kontrolleri */
    uint32_t gfm = mem_is_committed(0x8900dbu) ? MR32(0x8900d8u) : 0u;
    int racing_active = (gfm && gfm <= UINT32_MAX - 3u && mem_is_committed(gfm) && mem_is_committed(gfm + 3u) && MR32(gfm) == 4u) ||
                        (mem_is_committed(0x8654a7u) && MR32(0x8654a4u) == 6u);
    int not_paused = !mem_is_committed(0x89e7d8u) || (MR8(0x89e7a0u + 0x38u) == 0u);
    int not_replay = !mem_is_committed(0x89010bu) || (MR32(0x890108u) == 0u);

    if (MR32(veh + 0x18u) == 0u || !racing_active || !not_paused || !not_replay){
        nfsu2_input_note_rumble_hook(3u);
        nfsu2_input_clear_rumble();
        rumble_reset_state();
        return;
    }

    nfsu2_input_note_rumble_hook(4u);
    /* Read every contact signal before suppressing duplicate samples. */
    uint32_t sim_tick = mem_is_committed(0x86518bu) ? MR32(0x865188u) : 0u;
    float vx = MRF32(veh + 0x70u), vy = MRF32(veh + 0x74u), vz = MRF32(veh + 0x78u);
    float speed = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(speed)) {
        nfsu2_input_clear_rumble();
        rumble_reset_state();
        return;
    }

    if (s_prev_veh != veh) rumble_reset_state();

    float vec_dv = 0.0f;
    float speed_drop = 0.0f;
    if (s_has_prev){
        float dvx = vx - s_prev_vx, dvy = vy - s_prev_vy, dvz = vz - s_prev_vz;
        vec_dv = sqrtf(dvx * dvx + dvy * dvy + dvz * dvz);
        if (isnan(vec_dv) || isinf(vec_dv)) vec_dv = 0.0f;
        speed_drop = (s_prev_speed > speed) ? (s_prev_speed - speed) : 0.0f;
    }

    /* Vehicle+0x2c -> physics object; physics+0x6f8 -> collision body.
     * The legacy code read Vehicle+0x6f8 and subtracted the DragDriver-only
     * offset from every FF object, including the normal Driver+0x220 object. */
    uint32_t physics = MR32(veh + 0x2cu);
    uint32_t col_body = physics && physics <= UINT32_MAX - 0x6fbu &&
        mem_is_committed(physics) && mem_is_committed(physics + 0x6fbu)
        ? MR32(physics + 0x6f8u) : 0;
    int body_hit = col_body && col_body <= UINT32_MAX - 0x1e3u &&
        mem_is_committed(col_body) && mem_is_committed(col_body + 0x1e3u) &&
        sim_tick != 0u && MR32(col_body + 0x1e0u) == sim_tick;
    /* Tekerlek Zemin Temasi (veh + 0x390..0x393) ve Yuzey Materyali (veh + 0x394..0x3a0) */
    int wheels_on_ground = 0;
    int wall_wheels = 0;
    int rough_wheels = 0;
    uint32_t ground_mask = 0, surface[4];
    for (uint32_t i = 0; i < 4u; i++){
        uint8_t on_gnd = MR8(veh + 0x390u + i);
        if (on_gnd) { wheels_on_ground++; ground_mask |= 1u << i; }
        uint32_t surf = surface[i] = MR32(veh + 0x394u + i * 4u);
        if (surf == 0xdu) wall_wheels++;
        else if (on_gnd && (surf - 8u) <= 9u) rough_wheels++;
    }

    /* 4 Tekerlek Kayma / Drift (veh + 0x370..0x37c) */
    float slip_sum = fabsf(MRF32(veh + 0x370u)) + fabsf(MRF32(veh + 0x374u)) +
                     fabsf(MRF32(veh + 0x378u)) + fabsf(MRF32(veh + 0x37cu));
    if (isnan(slip_sum) || isinf(slip_sum)) slip_sum = 0.0f;

    if (s_has_prev && s_prev_veh == veh && sim_tick == s_prev_sim_tick &&
        vx == s_prev_vx && vy == s_prev_vy && vz == s_prev_vz &&
        (uint32_t)body_hit == s_prev_body_hit && ground_mask == s_prev_ground_mask &&
        slip_sum == s_prev_slip && memcmp(surface, s_prev_surface, sizeof(surface)) == 0) {
        nfsu2_input_report_rumble(s_cont_intensity, s_cont_sharpness);
        return;
    }
    int new_body_hit = body_hit && (!s_prev_body_hit || sim_tick != s_prev_sim_tick);

    /* Darbe (Impact) Tetikleme Kosullari */
    if (s_has_prev && (vec_dv >= 0.70f || speed_drop >= 0.60f ||
        (new_body_hit && (vec_dv >= 0.25f || speed >= 1.5f)) ||
        (wall_wheels > 0 && s_prev_wall_wheels == 0 && speed >= 1.2f))){
        float imp = (vec_dv - 0.45f) / 5.5f;
        float drop_imp = (speed_drop - 0.35f) / 4.5f;
        if (drop_imp > imp) imp = drop_imp;
        if (new_body_hit && imp < 0.45f) imp = 0.45f + fminf(speed / 35.0f, 0.45f);
        if (wall_wheels > 0 && s_prev_wall_wheels == 0 && imp < 0.50f) imp = 0.50f + fminf(speed / 30.0f, 0.45f);
        if (imp < 0.35f) imp = 0.35f;
        if (imp > 1.0f) imp = 1.0f;
        float sharp = 0.45f + 0.55f * imp;
        uint32_t dur = (uint32_t)(90.0f + 160.0f * ((imp - 0.35f) / 0.65f));
        if (dur < 90u) dur = 90u;
        if (dur > 250u) dur = 250u;
        nfsu2_input_trigger_impact(imp, sharp, dur);
    }

    /* Ziplama Sonrasi Yere Inis Darbesi (Jump Landing Thud) */
    if (wheels_on_ground == 0){
        if (!s_has_prev || sim_tick != s_prev_sim_tick)
            s_airborne_ticks = s_airborne_ticks < 60 ? s_airborne_ticks + 1 : 60;
    } else {
        if (s_airborne_ticks >= 6 && s_has_prev){
            float land_imp = fminf(0.35f + (float)s_airborne_ticks * 0.035f + fabsf(vz - s_prev_vz) * 0.12f, 0.90f);
            nfsu2_input_trigger_impact(land_imp, 0.40f, 120u);
        }
        s_airborne_ticks = 0;
    }

    /* Surekli Titresim (nfsu2_input_report_rumble) */
    float cont_intensity = 0.0f;
    float cont_sharpness = 0.0f;
    if (wall_wheels > 0 && speed > 1.0f){
        cont_intensity = fminf(0.40f + speed * 0.025f, 0.90f);
        cont_sharpness = 0.75f;
    }
    if (rough_wheels > 0 && speed > 2.0f){
        float r_int = fminf(0.18f + (float)rough_wheels * 0.06f + speed * 0.012f, 0.58f);
        if (r_int > cont_intensity){
            cont_intensity = r_int;
            cont_sharpness = 0.40f;
        }
    }
    if (slip_sum > 0.12f && speed > 1.5f && wheels_on_ground > 0){
        float s_int = fminf((slip_sum - 0.12f) * 0.22f, 0.48f);
        if (s_int > cont_intensity){
            cont_intensity = s_int;
            cont_sharpness = 0.25f;
        }
    }
    nfsu2_input_report_rumble(cont_intensity, cont_sharpness);

    s_prev_veh = veh;
    s_has_prev = 1;
    s_prev_sim_tick = sim_tick;
    s_prev_vx = vx;
    s_prev_vy = vy;
    s_prev_vz = vz;
    s_prev_speed = speed;
    s_prev_body_hit = (uint32_t)body_hit;
    s_prev_ground_mask = ground_mask;
    memcpy(s_prev_surface, surface, sizeof(surface));
    s_prev_slip = slip_sum;
    s_cont_intensity = cont_intensity;
    s_cont_sharpness = cont_sharpness;
    s_prev_wall_wheels = wall_wheels;

}

/* Legacy FF entry still serves DragDriver and enabled native FF paths. */
void sub_00406460(CPU *c) {
    uint32_t ff = c->ecx;
    uint32_t veh = ff && ff <= UINT32_MAX - 3u &&
        mem_is_committed(ff) && mem_is_committed(ff + 3u) ? MR32(ff) : 0;
    nfsu2_game_rumble_sample_vehicle(veh);
    sub_00406460_orig(c);
}

/* Human-driver update is reached even when its native FF branch is disabled.
 * Entry ECX is Driver*. +0x2a8 is Vehicle* (also used by the original update).
 * Preserve the original CPU result and return-stack adjustment exactly. */
void sub_0041e2c0_orig(CPU *c);
void sub_0041e2c0(CPU *c) {
    uint32_t driver = c->ecx;
    sub_0041e2c0_orig(c);
    uint32_t veh = driver && driver <= UINT32_MAX - 0x2abu &&
        mem_is_committed(driver) && mem_is_committed(driver + 0x2abu)
        ? MR32(driver + 0x2a8u) : 0;
    nfsu2_game_rumble_sample_vehicle(veh);
}

#endif
