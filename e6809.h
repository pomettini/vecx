#ifndef __E6809_H
#define __E6809_H

void e6809_reset (void);
unsigned e6809_sstep (unsigned irq_i, unsigned irq_f);
unsigned e6809_get_pc (void);
unsigned e6809_get_a (void);
unsigned e6809_get_dp (void);

#endif
