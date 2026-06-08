#ifndef __E8910_H
#define __E8910_H

void e8910_init_sound(void* pd); /* pd is a PlaydateAPI*; void* to keep the SDK out of the header */
void e8910_done_sound(void);
void e8910_write(int r, int v);
void e8910_generate(int n); /* generate n audio samples (call after the emulator runs) */

#endif
