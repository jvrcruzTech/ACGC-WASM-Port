#include "pc_wasm_noops.h"

void pc_noop_game(GAME* game) {
    (void)game;
}

void pc_noop_void(void) {
}

void pc_noop_actor(ACTOR* actor) {
    (void)actor;
}

void pc_noop_actor_game(ACTOR* actor, GAME* game) {
    (void)actor;
    (void)game;
}

void pc_noop_nature(ACTOR* actor) {
    (void)actor;
}

void pc_noop_npc_draw_before(GAME* game, NPC_ACTOR* nactorx, void* rot_p, void* trans_p) {
    (void)game;
    (void)nactorx;
    (void)rot_p;
    (void)trans_p;
}

void pc_noop_npc_draw_after(GAME* game, NPC_ACTOR* nactorx, int joint_idx) {
    (void)game;
    (void)nactorx;
    (void)joint_idx;
}

void pc_noop_npc_sub(NPC_ACTOR* nactorx, GAME_PLAY* play) {
    (void)nactorx;
    (void)play;
}

void pc_noop_npc_schedule(NPC_ACTOR* nactorx, GAME_PLAY* play, int type) {
    (void)nactorx;
    (void)play;
    (void)type;
}

void pc_noop_ptr_play(void* actor, GAME_PLAY* play) {
    (void)actor;
    (void)play;
}
