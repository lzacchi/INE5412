/*
 * Copyright(C) 2011-2016 Pedro H. Penna <pedrohenriquepenna@gmail.com>
 * 
 * This file is part of Nanvix.
 * 
 * Nanvix is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Nanvix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Nanvix. If not, see <http://www.gnu.org/licenses/>.
 */

#include <nanvix/config.h>
#include <nanvix/const.h>
#include <nanvix/clock.h>
#include <nanvix/dev.h>
#include <nanvix/fs.h>
#include <nanvix/hal.h>
#include <nanvix/klib.h>
#include <nanvix/mm.h>
#include <nanvix/region.h>
#include <signal.h>
#include "mm.h"

/*
 * Swapping area too small?
 */
#if (SWP_SIZE < MEMORY_SIZE)
	#error "swapping area to small"
#endif

/**
 * @brief Gets a page directory entry of a process.
 * 
 * @param p Process.
 * @param a Address.
 * 
 * @returns The requested page directory entry.
 */
#define getpde(p, a) \
	(&(p)->pgdir[PGTAB(a)])

/**
 * @brief Gets a page table entry of a process.
 * 
 * @param p Process.
 * @param a Address.
 * 
 * @returns The requested page table entry.
 */
#define getpte(p, a) \
	(&((struct pte *)((getpde(p, a)->frame << PAGE_SHIFT) + KBASE_VIRT))[PG(a)])


/*============================================================================*
 *                             Swapping System                                *
 *============================================================================*/

/**
 * @brief Swap space.
 */
PRIVATE struct
{
	unsigned count[SWP_SIZE/PAGE_SIZE];         /**< Reference count. */
	uint32_t bitmap[(SWP_SIZE/PAGE_SIZE) >> 5]; /**< Bitmap.          */
} swap = {{0, }, {0, } };

/**
 * @brief Clears the swap space that is associated to a page.
 * 
 * @param pg Page to be inspected.
 */
PRIVATE void swap_clear(struct pte *pg)
{
	unsigned i;
	
	i = pg->frame;
	
	/* Free swap space. */
	if (swap.count[i] > 0)
	{
		bitmap_clear(swap.bitmap, i);
		swap.count[i]--;
	}
}

/**
 * @brief Swaps a page out to disk.
 * 
 * @param proc Process where the page is located.
 * @param addr Address of the page to be swapped out.
 * 
 * @returns Zero upon success, and non-zero otherwise.
 */
PRIVATE int swap_out(struct process *proc, addr_t addr)
{
	unsigned blk;   /* Block number in swap device.  */
	struct pte *pg; /* Page table entry.             */
	off_t off;      /* Offset in swap device.        */
	ssize_t n;      /* # bytes written.              */
	void *kpg;      /* Kernel page used for copying. */
	
	addr &= PAGE_MASK;
	pg = getpte(proc, addr);
	
	/* Get kernel page. */
	if ((kpg = getkpg(0)) == NULL)
		goto error0;
	
	/* Get free block in swap device. */
	blk = bitmap_first_free(swap.bitmap, (SWP_SIZE/PAGE_SIZE) >> 3);
	if (blk == BITMAP_FULL)
		goto error1;
	
	/*
	 * Set block on swap device as used
	 * in advance, because we may sleep below.
	 */
	off = HDD_SIZE + blk*PAGE_SIZE;
	bitmap_set(swap.bitmap, blk);
	
	/* Write page to disk. */
	kmemcpy(kpg, (void *)addr, PAGE_SIZE);
	n = bdev_write(SWAP_DEV, (void *)addr, PAGE_SIZE, off);
	if (n != PAGE_SIZE)
		goto error2;
	swap.count[blk]++;
	
	/* Set page as non-present. */
	pg->present = 0;
	pg->frame = blk;
	tlb_flush();
	
	putkpg(kpg);
	return (0);

error2:
	bitmap_clear(swap.bitmap, blk);
error1:
	putkpg(kpg);
error0:
	return (-1);
}

/**
 * @brief Swaps a page in to disk.
 * 
 * @param frame Frame number where the page should be placed.
 * @param addr  Address of the page to be swapped in.
 * 
 * @returns Zero upon success, and non-zero otherwise.
 */
PRIVATE int swap_in(unsigned frame, addr_t addr)
{
	unsigned blk;   /* Block number in swap device.  */
	struct pte *pg; /* Page table entry.             */
	off_t off;      /* Offset in swap device.        */
	ssize_t n;      /* # bytes read.                 */
	void *kpg;      /* Kernel page used for copying. */
	
	addr &= PAGE_MASK;
	pg = getpte(curr_proc, addr);
	
	/* Get kernel page. */
	if ((kpg = getkpg(0)) == NULL)
		goto error0;
	
	/* Get block # in swap device. */
	blk = pg->frame;
	off = HDD_SIZE + blk*PAGE_SIZE;
	
	/* Read page from disk. */
	n = bdev_read(SWAP_DEV, kpg, PAGE_SIZE, off);
	if (n != PAGE_SIZE)
		goto error1;
	swap_clear(pg);
	
	/* Set page as present. */
	pg->present = 1;
	pg->frame = (UBASE_PHYS >> PAGE_SHIFT) + frame;
	tlb_flush();
	
	/* Copy page. */
	kmemcpy((void *)addr, kpg, PAGE_SIZE);
	pg->accessed = 0;
	pg->dirty = 0;
		
	putkpg(kpg);
	return (0);

error1:
	putkpg(kpg);
error0:
	return (-1);
}

/*============================================================================*
 *                             Kernel Page Pool                               *
 *============================================================================*/

/* Kernel pages. */
#define NR_KPAGES (KPOOL_SIZE/PAGE_SIZE) /* Number of kernel pages.  */
PRIVATE int kpages[NR_KPAGES] = { 0,  }; /* Reference count.         */

/**
 * @brief Allocates a kernel page.
 * 
 * @param clean Should the page be cleaned?
 * 
 * @returns Upon success, a pointer to a page is returned. Upon failure, a NULL
 *          pointer is returned instead.
 */
PUBLIC void *getkpg(int clean)
{
	unsigned i; /* Loop index.  */
	void *kpg;  /* Kernel page. */
	
	/* Search for a free kernel page. */
	for (i = 0; i < NR_KPAGES; i++)
	{
		/* Found it. */
		if (kpages[i] == 0)
			goto found;
	}

	kprintf("mm: kernel page pool overflow");
	
	return (NULL);

found:

	/* Set page as used. */
	kpg = (void *)(KPOOL_VIRT + (i << PAGE_SHIFT));
	kpages[i]++;
	
	/* Clean page. */
	if (clean)
		kmemset(kpg, 0, PAGE_SIZE);
	
	return (kpg);
}

/**
 * @brief Releases kernel page.
 * 
 * @param kpg Kernel page to be released.
 */
PUBLIC void putkpg(void *kpg)
{
	unsigned i;
	
	i = ((addr_t)kpg - KPOOL_VIRT) >> PAGE_SHIFT;
	
	/* Release page. */
	kpages[i]--;
	
	/* Double free. */
	if (kpages[i] < 0)
		kpanic("mm: releasing kernel page twice");
}

/*============================================================================*
 *                              Paging System                                 *
 *============================================================================*/

/* Number of page clock1. */
#define CLOCK1_FRAMES (UMEM_SIZE/PAGE_SIZE)
#define CLOCK2_FRAMES (CLOCK1_FRAMES/2)

/**
 * @brief Clock1 pages.
 */
PRIVATE struct
{
	unsigned count; /**< Reference count.     */
	unsigned age;   /**< Age.                 */
	pid_t owner;    /**< Page owner.          */
	addr_t addr;    /**< Address of the page. */
} clock1[CLOCK1_FRAMES] = {{0, 0, 0, 0},  };

/**
 * @brief Clock2 pages.
 */
PRIVATE struct
{
	unsigned count; /**< Reference count.     */
	unsigned age;   /**< Age.                 */
	pid_t owner;    /**< Page owner.          */
	addr_t addr;    /**< Address of the page. */
} clock2[CLOCK2_FRAMES] = {{0, 0, 0, 0},  };

/**
 * @brief Replace clock frames
 * @param first_clock Whether to replace the page from the first clock or not
 */
PRIVATE int replace_frame()
{
	// R bit -> reference bit (process->accessed)
	// M bit -> modified bit (process->dirty)
	
	int page_number = -1;
	int c1_process_index = curr_proc->c1_ptr;
	int c2_process_index = curr_proc->c2_ptr;
	struct pte* pg_1 = getpte(curr_proc, clock1[c1_process_index].addr);

	int i;
	for (i = c1_process_index; i < CLOCK1_FRAMES; i++)
	{
		c1_process_index = (c1_process_index + 1) % CLOCK1_FRAMES;
		pg_1 = getpte(curr_proc, clock1[c1_process_index].addr);
		
		// skip shared pages and non-owner accesses
		if (clock1[c1_process_index].count > 1 || clock1[c1_process_index].owner != curr_proc->pid)
			continue;
		
step2a:
		if (!pg_1->accessed && !pg_1->dirty) // R == 0 && M == 0 | step 2 (a) i.
		{
			// replace page and advance c1 pointer
			page_number = c1_process_index;
			curr_proc->c1_ptr = (c1_process_index+1) % CLOCK1_FRAMES;
		}
		else if (pg_1->accessed && !pg_1->dirty) // R == 1 && M == 0 | step 2 (a) ii.
		{
			// set R = 0 and advance C1 and go to 2 (a)
			pg_1->accessed = 0;
			curr_proc->c1_ptr = (c1_process_index+1) % CLOCK1_FRAMES;
			goto step2a;
		}
		else if (pg_1->dirty) // M == 1 | step 2 (a) iii.
		{
			// add to c2-1
			curr_proc->c2_ptr = (curr_proc->c2_ptr -1) % CLOCK2_FRAMES;;
			clock2[c2_process_index].count = clock1[c1_process_index].count;
			clock2[c2_process_index].age = clock1[c1_process_index].age;
			clock2[c2_process_index].owner = clock1[c1_process_index].owner;
			clock2[c2_process_index].addr = clock1[c1_process_index].addr;
			// remove page
			clock1[c1_process_index].count = 0;
			clock1[c1_process_index].age = 0;
			clock1[c1_process_index].owner = 0;
			clock1[c1_process_index].addr = 0;
			// set R to 0
			pg_1->accessed = 0;
			// advance c1
			curr_proc->c1_ptr = (c1_process_index+1) % CLOCK1_FRAMES;
			// repeat steap 2a
			goto step2a;
		}
	}
	
	if (swap_out(curr_proc, clock1[page_number].addr))
		return (-1);
	
	return page_number;
}

/**
 * @brief Allocates a page frame.
 * 
 * @returns Upon success, the number of the frame is returned. Upon failure, a
 *          negative number is returned instead.
 */
PRIVATE int allocf(void)
{
	int i;
	int c1_start = curr_proc->c1_ptr;
	int c2_start = curr_proc->c2_ptr;
	
	// finding empty frame
	for (i = 0; i < (c1_start + CLOCK1_FRAMES); i++)
	{
		curr_proc->c1_ptr = (curr_proc->c1_ptr + 1) % CLOCK1_FRAMES;
		if (clock1[i].count == 0) // empty
		{
			goto found_c1;
		}
	}
	
	// empty frame not found, search for c2
	for (i = 0; i < (c2_start + CLOCK2_FRAMES); i++)
	{
		curr_proc->c2_ptr = (curr_proc->c2_ptr + 1) % CLOCK2_FRAMES;
		if (clock2[i].count == 0) // empty
		{
			goto found_c2;
		}
	}
	
	// no empty frames were found | step 2 (b)
	i = replace_frame();
	if (i < 0) return -1;
	
	// swap clock2 and swap previous index on clock1
	int clock1_previous_index = (curr_proc->c1_ptr - 1) % CLOCK1_FRAMES;
	clock1[clock1_previous_index].age = ticks;
	clock1[clock1_previous_index].count = 1;
	goto found_c1;
	
found_c1:
	clock1[i].age = ticks;
	clock1[i].count = 1;
	return i;

found_c2:
	clock2[i].age = ticks;
	clock2[i].count = 1;
	return i;
}

/**
 * @brief Copies a page.
 * 
 * @brief pg1 Target page.
 * @brief pg2 Source page.
 * 
 * @returns Zero upon successful completion, and non-zero otherwise.
 * 
 * @note The source page is assumed to be in-core.
 */
PRIVATE int cpypg(struct pte *pg1, struct pte *pg2)
{
	int i;
	
	/* Allocate new user page. */
	if ((i = allocf()) < 0)
		return (-1);
	
	/* Handcraft page table entry. */
	pg1->present = pg2->present;
	pg1->writable = pg2->writable;
	pg1->user = pg2->user;
	pg1->cow = pg2->cow;
	pg1->frame = (UBASE_PHYS >> PAGE_SHIFT) + i;

	physcpy(pg1->frame << PAGE_SHIFT, pg2->frame << PAGE_SHIFT, PAGE_SIZE);
	
	return (0);
}

/**
 * @brief Allocates a user page.
 * 
 * @param addr     Address where the page resides.
 * @param writable Is the page writable?
 * 
 * @returns Zero upon successful completion, and non-zero otherwise.
 */
PRIVATE int allocupg(addr_t addr, int writable)
{
	int i;          /* Page frame index.         */
	struct pte *pg; /* Working page table entry. */
	
	/* Failed to allocate page frame. */
	if ((i = allocf()) < 0)
		return (-1);
	
	/* Initialize page frame. */
	clock1[i].owner = curr_proc->pid;
	clock1[i].addr = addr & PAGE_MASK;
	
	/* Allocate page. */
	pg = getpte(curr_proc, addr);
	kmemset(pg, 0, sizeof(struct pte));
	pg->present = 1;
	pg->writable = (writable) ? 1 : 0;
	pg->user = 1;
	pg->frame = (UBASE_PHYS >> PAGE_SHIFT) + i;
	tlb_flush();
	
	return (0);
}

/**
 * @brief Reads a page from a file.
 * 
 * @param reg  Region where the page resides.
 * @param addr Address where the page should be loaded. 
 * 
 * @returns Zero upon successful completion, and non-zero upon failure.
 */
PRIVATE int readpg(struct region *reg, addr_t addr)
{
	char *p;             /* Read pointer.             */
	off_t off;           /* Block offset.             */
	ssize_t count;       /* Bytes read.               */
	struct inode *inode; /* File inode.               */
	struct pte *pg;      /* Working page table entry. */
	
	addr &= PAGE_MASK;
	
	/* Assign a user page. */
	if (allocupg(addr, reg->mode & MAY_WRITE))
		return (-1);
	
	/* Find page table entry. */
	pg = getpte(curr_proc, addr);
	
	/* Read page. */
	off = reg->file.off + (PG(addr) << PAGE_SHIFT);
	inode = reg->file.inode;
	p = (char *)(addr & PAGE_MASK);
	count = file_read(inode, p, PAGE_SIZE, off);
	
	/* Failed to read page. */
	if (count < 0)
	{
		freeupg(pg);
		return (-1);
	}
	
	/* Fill remainder bytes with zero. */
	else if (count < PAGE_SIZE)
		kmemset(p + count, 0, PAGE_SIZE - count);
	
	return (0);
}

/**
 * @brief Maps a page table into user address space.
 * 
 * @param proc  Process in which the page table should be mapped.
 * @param addr  Address where the page should be mapped.
 * @param pgtab Page table to map.
 */
PUBLIC void mappgtab(struct process *proc, addr_t addr, void *pgtab)
{
	struct pde *pde;
	
	pde = &proc->pgdir[PGTAB(addr)];
	
	/* Bad page table. */
	if (pde->present)
		kpanic("busy page table entry");
	
	/* Map kernel page. */
	pde->present = 1;
	pde->writable = 1;
	pde->user = 1;
	pde->frame = (ADDR(pgtab) - KBASE_VIRT) >> PAGE_SHIFT;
	
	/* Flush changes. */
	if (proc == curr_proc)
		tlb_flush();
}

/**
 * @brief Unmaps a page table from user address space.
 * 
 * @param proc Process in which the page table should be unmapped.
 * @param addr Address where the page should be unmapped.
 * 
 * @returns Zero upon success, and non zero otherwise.
 */
PUBLIC void umappgtab(struct process *proc, addr_t addr)
{
	struct pde *pde;
	
	pde = &proc->pgdir[PGTAB(addr)];
	
	/* Bad page table. */
	if (!(pde->present))
		kpanic("unmap non-present page table");

	/* Unmap kernel page. */
	kmemset(pde, 0, sizeof(struct pde));
	
	/* Flush changes. */
	if (proc == curr_proc)
		tlb_flush();
}

/**
 * @brief Frees a user page.
 * 
 * @param pg Page to be freed.
 */
PUBLIC void freeupg(struct pte *pg)
{
	unsigned i;
	
	/* In-disk page. */
	if (!pg->present)
	{
		swap_clear(pg);
		kmemset(pg, 0, sizeof(struct pte));
		return;
	}
		
	i = pg->frame - (UBASE_PHYS >> PAGE_SHIFT);
		
	/* Double free. */
	if (clock1[i].count == 0)
		kpanic("freeing user page twice");
	
	/* Free user page. */
	if (--clock1[i].count)
		clock1[i].owner = 0;
	kmemset(pg, 0, sizeof(struct pte));
	tlb_flush();
}

/**
 * @brief Marks a page.
 * 
 * @param pg   Page to be marked.
 * @param mark Mark.
 */
PUBLIC void markpg(struct pte *pg, int mark)
{
	/* Bad page. */
	if (pg->present)
		kpanic("demand fill on a present page");
	
	/* Mark page. */
	switch (mark)
	{
		/* Demand fill. */
		case PAGE_FILL:
			pg->fill = 1;
			pg->zero = 0;
			break;
		
		/* Demand zero. */
		case PAGE_ZERO:
			pg->fill = 0;
			pg->zero = 1;
			break;
	}
}

/**
 * @brief Links two pages.
 * 
 * @param upg1 Source page.
 * @param upg2 Target page.
 */
PUBLIC void linkupg(struct pte *upg1, struct pte *upg2)
{	
	unsigned i;

	/* In-core page. */
	if (upg1->present)
	{
		/* Set copy on write. */
		if (upg1->writable)
		{
			upg1->writable = 0;
			upg1->cow = 1;
		}
	
		i = upg1->frame - (UBASE_PHYS >> PAGE_SHIFT);
		clock1[i].count++;
	}
	
	/* In-disk page. */
	else
	{
		 i = upg1->frame;
		 swap.count[i]++;
	}
	
	kmemcpy(upg2, upg1, sizeof(struct pte));
}

/**
 * @brief Creates a page directory for a process.
 * 
 * @param proc Target process.
 * 
 * @returns Upon successful completion, zero is returned. Upon failure, non-zero
 *          is returned instead.
 */
PUBLIC int crtpgdir(struct process *proc)
{
	void *kstack;             /* Kernel stack.     */
	struct pde *pgdir;        /* Page directory.   */
	struct intstack *s1, *s2; /* Interrupt stacks. */
	
	/* Get kernel page for page directory. */
	pgdir = getkpg(1);
	if (pgdir == NULL)
		goto err0;

	/* Get kernel page for kernel stack. */
	kstack = getkpg(0);
	if (kstack == NULL)
		goto err1;

	/* Build page directory. */
	pgdir[0] = curr_proc->pgdir[0];
	pgdir[PGTAB(KBASE_VIRT)] = curr_proc->pgdir[PGTAB(KBASE_VIRT)];
	pgdir[PGTAB(KPOOL_VIRT)] = curr_proc->pgdir[PGTAB(KPOOL_VIRT)];
	pgdir[PGTAB(INITRD_VIRT)] = curr_proc->pgdir[PGTAB(INITRD_VIRT)];
	
	/* Clone kernel stack. */
	kmemcpy(kstack, curr_proc->kstack, KSTACK_SIZE);
	
	/* Adjust stack pointers. */
	proc->kesp = (curr_proc->kesp -(dword_t)curr_proc->kstack)+(dword_t)kstack;
	if (curr_proc == IDLE)
	{
		s1 = (struct intstack *) curr_proc->kesp;
		s2 = (struct intstack *) proc->kesp;	
		s2->ebp = (s1->ebp - (dword_t)curr_proc->kstack) + (dword_t)kstack;
	}
	/* Assign page directory. */
	proc->cr3 = ADDR(pgdir) - KBASE_VIRT;
	proc->pgdir = pgdir;
	proc->kstack = kstack;
	
	return (0);

err1:
	putkpg(pgdir);
err0:
	return (-1);
}

/**
 * @brief Destroys the page directory of a process.
 * 
 * @param proc Target process.
 * 
 * @note The current running process may not be the target process.
 */
PUBLIC void dstrypgdir(struct process *proc)
{
	putkpg(proc->kstack);
	putkpg(proc->pgdir);
}

/**
 * @brief Handles a validity page fault.
 * 
 * @brief addr Faulting address.
 * 
 * @returns Upon successful completion, zero is returned. Upon failure, non-zero
 *          is returned instead.
 */
PUBLIC int vfault(addr_t addr)
{
	int frame;            /* Frame index of page to be swapped in. */
	struct pte *pg;       /* Working page.                         */
	struct region *reg;   /* Working region.                       */
	struct pregion *preg; /* Working process region.               */
	
	/* Get associated region. */
	preg = findreg(curr_proc, addr);
	if (preg == NULL)
		goto error0;
	
	lockreg(reg = preg->reg);
	
	/* Outside virtual address space. */
	if (!withinreg(preg, addr))
	{			
		/* Not a stack region. */
		if (preg != STACK(curr_proc))
			goto error1;
	
		kprintf("growing stack");
		
		/* Expand region. */
		if (growreg(curr_proc,preg,(preg->start-reg->size)-(addr&~PGTAB_MASK)))
			goto error1;
	}

	pg = (reg->flags & REGION_DOWNWARDS) ?
		&reg->pgtab[REGION_PGTABS-(PGTAB(preg->start)-PGTAB(addr))-1][PG(addr)]: 
		&reg->pgtab[PGTAB(addr) - PGTAB(preg->start)][PG(addr)];
		
	/* Clear page. */
	if (pg->zero)
	{
		if (allocupg(addr, reg->mode & MAY_WRITE))
			goto error1;
		kmemset((void *)(addr & PAGE_MASK), 0, PAGE_SIZE);
	}
		
	/* Load page from executable file. */
	else if (pg->fill)
	{
		/* Read page. */
		if (readpg(reg, addr))
			goto error1;
	}
		
	/* Swap page in. */
	else
	{
		if ((frame = allocf()) < 0)
			goto error1;
		if (swap_in(frame, addr))
			goto error2;
		clock1[frame].addr = addr & PAGE_MASK;
	}
	
	unlockreg(reg);
	return (0);

error2:
	clock1[frame].count = 0;
error1:
	unlockreg(reg);
error0:
	return (-1);
}
/**
 * @brief Handles a protection page fault.
 * 
 * @brief addr Faulting address.
 * 
 * @returns Upon successful completion, zero is returned. Upon failure, non-zero
 *          is returned instead.
 */
PUBLIC int pfault(addr_t addr)
{
	unsigned i;           /* Frame index.            */
	struct pte *pg;       /* Faulting page.          */
	struct pte new_pg;    /* New page.               */
	struct region *reg;   /* Working memory region.  */
	struct pregion *preg; /* Working process region. */

	preg = findreg(curr_proc, addr);
	
	/* Outside virtual address space. */
	if ((preg == NULL) || (!withinreg(preg, addr)))
		goto error0;
	
	lockreg(reg = preg->reg);

	pg = (reg->flags & REGION_DOWNWARDS) ?
		&reg->pgtab[REGION_PGTABS-(PGTAB(preg->start)-PGTAB(addr))-1][PG(addr)]: 
		&reg->pgtab[PGTAB(addr) - PGTAB(preg->start)][PG(addr)];

	/* Copy on write not enabled. */
	if (!pg->cow)
		goto error1;
		
	i = pg->frame - (UBASE_PHYS >> PAGE_SHIFT);

	/* Duplicate page. */
	if (clock1[i].count > 1)
	{
		if (cpypg(&new_pg, pg))
			goto error1;
		
		new_pg.cow = 0;
		new_pg.writable = 1;
		
		/* Unlik page. */
		clock1[i].count--;
		kmemcpy(pg, &new_pg, sizeof(struct pte));
	}
		
	/* Steal page. */
	else
	{
		pg->cow = 0;
		pg->writable = 1;
	}
	
	unlockreg(reg);
	return(0);

error1:
	unlockreg(reg);
error0:
	return (-1);
}