/******************************************************************************
 * memory.c
 * 
 * Copyright (c) 2002-2004 K A Fraser
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/perfc.h>
#include <xen/irq.h>
#include <asm/page.h>
#include <asm/flushtlb.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/domain_page.h>

/* Frame table and its size in pages. */
struct pfn_info *frame_table;
unsigned long frame_table_size;
unsigned long max_page;

extern void arch_init_memory(void);

void __init init_frametable(void *frametable_vstart, unsigned long nr_pages)
{
    unsigned long mfn;

    arch_init_memory();

    max_page = nr_pages;
    frame_table_size = nr_pages * sizeof(struct pfn_info);
    frame_table_size = (frame_table_size + PAGE_SIZE - 1) & PAGE_MASK;
    frame_table = frametable_vstart;

    if ( (__pa(frame_table) + frame_table_size) > (max_page << PAGE_SHIFT) )
        panic("Not enough memory for frame table - reduce Xen heap size?\n");

    memset(frame_table, 0, frame_table_size);

    /* Initialise to a magic of 0x55555555 so easier to spot bugs later. */
    memset(machine_to_phys_mapping, 0x55, 4<<20);

    /* Pin the ownership of the MP table so that DOM0 can map it later. */
    for ( mfn = virt_to_phys(&machine_to_phys_mapping[0<<20])>>PAGE_SHIFT;
          mfn < virt_to_phys(&machine_to_phys_mapping[1<<20])>>PAGE_SHIFT;
          mfn++ )
    {
        frame_table[mfn].u.inuse.count_info = 1 | PGC_allocated;
        frame_table[mfn].u.inuse.type_info = 1 | PGT_gdt_page; /* non-RW */
        frame_table[mfn].u.inuse.domain = &idle0_task;
    }
}
