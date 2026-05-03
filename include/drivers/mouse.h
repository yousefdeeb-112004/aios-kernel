#ifndef _DRIVERS_MOUSE_H
#define _DRIVERS_MOUSE_H
#include <kernel/types.h>
typedef struct { int32_t x; int32_t y; bool left_btn; bool right_btn; bool middle_btn; uint32_t total_moves; uint32_t total_clicks; uint32_t packets_received; } mouse_state_t;
extern mouse_state_t g_mouse;
void mouse_init(void); void mouse_dump(void);
#endif
