#ifndef PC_WASM_NOOPS_H
#define PC_WASM_NOOPS_H

#include "game_h.h"
#include "m_actor_type.h"
#include "m_play_h.h"
#include "ac_npc_h.h"

void pc_noop_game(GAME* game);
void pc_noop_actor(ACTOR* actor);
void pc_noop_actor_game(ACTOR* actor, GAME* game);
void pc_noop_nature(ACTOR* actor);
void pc_noop_npc_draw_before(GAME* game, NPC_ACTOR* nactorx, void* rot_p, void* trans_p);
void pc_noop_npc_draw_after(GAME* game, NPC_ACTOR* nactorx, int joint_idx);
void pc_noop_npc_sub(NPC_ACTOR* nactorx, GAME_PLAY* play);
void pc_noop_npc_schedule(NPC_ACTOR* nactorx, GAME_PLAY* play, int type);
void pc_noop_ptr_play(void* actor, GAME_PLAY* play);

#ifdef __EMSCRIPTEN__
#define PC_WASM_NOOP_PROC(type) ((type)pc_noop_actor)
#define PC_WASM_NULL_NOOP_PROC(type) ((type)pc_noop_actor)
#else
#define PC_WASM_NOOP_PROC(type) ((type)none_proc1)
#define PC_WASM_NULL_NOOP_PROC(type) NULL
#endif

#endif
