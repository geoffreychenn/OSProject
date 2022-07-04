/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->as_regions = NULL;

	/* Malloc and Initialise 1st level Page Table to NULL */
	as->as_pt = kmalloc(sizeof(paddr_t **) * PTLEVEL1);
	if (as->as_pt == NULL) {
		kfree(as);
		return NULL;
	}

	for (int i = 0; i < PTLEVEL1; i++) {
		as->as_pt[i] = NULL;
	}

	return as;

}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	if (old == NULL) {
		return EINVAL;
	}
	if (old->as_regions == NULL || old->as_pt == NULL) {
		return EFAULT;
	}
	struct addrspace *newas;
	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	struct as_region *old_curr = old->as_regions;	// iterate over old list
	struct as_region *new_curr = newas->as_regions;	// tail of new list
	while (old_curr != NULL) {
		
		struct as_region *temp = kmalloc(sizeof(struct as_region));
		if (temp == NULL) {
			return ENOMEM;
		}

		temp->size = old_curr->size;
		temp->base = old_curr->base;
		temp->readable = old_curr->readable;
		temp->writeable = old_curr->writeable;
		temp->executable = old_curr->executable;
		temp->old_writeable = old_curr->old_writeable;
		temp->next = NULL;

		if (new_curr != NULL) {
			new_curr->next = temp;
		} else {
			newas->as_regions = temp;
		}
		new_curr = temp;
		old_curr = old_curr->next;
	}

	/* Copy Page Table */
	int found = copy_Page_Table(old->as_pt, newas->as_pt);
	if (found) {
		as_destroy(newas);
		return found;
	}
	
	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	/* Free the page table */
	if(as->as_pt != NULL) {
		freePT(as->as_pt);
	}

	/* Free the linked list region */
	if(as != NULL) {
		struct as_region *curr = as->as_regions;
		struct as_region *temp;
		while(curr != NULL) {
			temp = curr->next;
			kfree(curr);
			curr = temp;
		}
	}
	kfree(as);
}

void
as_activate(void)
{
	// copied from dumbvm
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
	 as_activate();
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	if(as == NULL) {
		return EFAULT;
	}

	if(as->as_regions == NULL) {
		as->as_regions = kmalloc(sizeof(struct as_region));
		if(as->as_regions == NULL) {
			return ENOMEM;
		}
		as->as_regions->base = vaddr;
		as->as_regions->size = memsize;
		as->as_regions->readable = readable;
		as->as_regions->writeable = writeable;
		as->as_regions->executable = executable;
		as->as_regions->next = NULL;
		return 0;
	}

	// traverse to last node in list
	struct as_region *curr = as->as_regions;
	while(curr->next != NULL) {
		vaddr_t curr_base = curr->base;
		vaddr_t curr_top = curr->base + curr->size - 1;
		vaddr_t vaddr_top = vaddr + memsize - 1;

		// check if new vaddr intersects with any existing entries
		if((vaddr < curr_base && vaddr_top < curr_top) || 
			(vaddr > curr_base && vaddr_top < curr_top) ||
			(vaddr < curr_top && vaddr_top > curr_top)) {
				return EBADF;
			}

		curr = curr->next;
	}
	
	// if we need a new node then create a new one and insert to end of list
	if(curr->size > 0) {
		curr->next = kmalloc(sizeof(struct as_region));
		if(curr->next == NULL) {
			return EFAULT;
		}
		curr = curr->next;
	}

	curr->size = memsize;
	curr->base = vaddr;
	curr->readable = readable;
	curr->writeable = writeable;
	curr->executable = executable;
	curr->old_writeable = 0;
	curr->next = NULL;
	
	return 0;

}

int
as_prepare_load(struct addrspace *as)
{
	if (as == NULL || as->as_regions == NULL) {
		return EFAULT;
	}

	struct as_region *curr = as->as_regions;
	while (curr != NULL) {
		curr->old_writeable = curr->writeable; // Store previous writeable value
		curr->writeable = 2; // Make region writeable (writeable is 1 << 2 from elf)
		curr = curr->next;
	}
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	if (as == NULL || as->as_regions == NULL) {
		return EFAULT;
	}
	/* Flush the TLB as there may be entries that should be read-only in there */
	as_activate();
	
	/* Go through the Page Table and enforce read-only */
	paddr_t ***PT = as->as_pt;
	for (int i = 0; i < PTLEVEL1; i++) {
		if (PT[i] != NULL) {
			for (int j = 0; j < PTLEVEL2; j++) {
				if (PT[i][j] != NULL) {
					for (int k = 0; k < PTLEVEL3; k++) {
						if (PT[i][j][k] != 0) { // If there is an entry
							vaddr_t vaddr = get_address(i, j, k);
							struct as_region *region = find_region(as, vaddr);
							if(region == NULL) continue; // skip over invalid PTE's
							if (region->old_writeable == 0) {
								PT[i][j][k] = (PT[i][j][k] & PAGE_FRAME) | TLBLO_VALID;
							}
						}
					}
				}
			}
		}
	}
	
	/* Revert the permissions for the regions */
	struct as_region *curr = as->as_regions;
	while (curr != NULL) {
		curr->writeable = curr->old_writeable; // Set the previous writeable value
		curr = curr->next;
	}
	
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	if (as == NULL) {
		return EFAULT;
	}

	// get the size of the stack, assuming 16 pages
	size_t stack_size = N_STACKPAGES * PAGE_SIZE;

	// get bottom of the stack
	*stackptr = USERSTACK;

	// note read, write and execute are 4, 2 and 1 respectively in elf_load
	return as_define_region(as, *stackptr - stack_size, stack_size, 4, 2, 0);
}

void freePT(paddr_t ***PT)
{

	for (int i = 0; i < PTLEVEL1; i++) {
		if (PT[i] == NULL) {
			continue;
		}
		for (int j = 0; j < PTLEVEL2; j++) {
			if (PT[i][j] == NULL) {
				continue;
			}
			for (int k = 0; k < PTLEVEL3; k++) {
				if (PT[i][j][k] != 0) {
					free_kpages(PADDR_TO_KVADDR(PT[i][j][k] & PAGE_FRAME)); // Free the pages
				}
			}
			kfree(PT[i][j]); // Free the 3rd table
		}
		kfree(PT[i]); // Free the 2nd table
	}
	kfree(PT); // Free the 1st table
}

vaddr_t get_address(int first_level, int second_level, int third_level)
{
	vaddr_t mostsb = first_level << 24;
	vaddr_t middlesb = second_level << 18;
	vaddr_t leastsb = third_level << 12;
	vaddr_t addr = mostsb | middlesb | leastsb;
	return addr;
}

struct as_region *find_region(struct addrspace *as, vaddr_t vaddr)
{
	struct as_region *curr = as->as_regions;
	while (curr != NULL) {
		if (curr->base <= vaddr && ((curr->size + curr->base) > vaddr)) {
			break;
		}
		curr = curr->next;
	}
	return curr;
}

int copy_Page_Table(paddr_t ***oldPT, paddr_t ***newPT)
{
	for (int i = 0; i < PTLEVEL1; i++) {
		if (oldPT[i] == NULL) {
			newPT[i] = NULL;
			continue;
		}
		newPT[i] = kmalloc(sizeof(paddr_t *) * PTLEVEL2);
		if (newPT[i] == NULL) {
			return ENOMEM;
		}
		for (int j = 0; j < PTLEVEL2; j++) {
			if (oldPT[i][j] == NULL) {
				newPT[i][j] = NULL;
				continue;
			}
			newPT[i][j] = kmalloc(sizeof(paddr_t) * PTLEVEL3);
			if (newPT[i][j] == NULL) {
				return ENOMEM;
			}
			for (int k = 0; k < PTLEVEL3; k++) {
				if (oldPT[i][j][k] != 0) {
					vaddr_t fresh_frame = alloc_kpages(1);
					if (fresh_frame == 0) {
						return ENOMEM;
					}
					bzero((void *)fresh_frame, PAGE_SIZE);
					if (memcpy((void *)fresh_frame, (const void *)PADDR_TO_KVADDR(oldPT[i][j][k] & PAGE_FRAME), PAGE_SIZE) == NULL) {
						freePT(newPT);
						return ENOMEM;
					}
					uint32_t dirty_bit = oldPT[i][j][k] & TLBLO_DIRTY;
					newPT[i][j][k] = (KVADDR_TO_PADDR(fresh_frame) & PAGE_FRAME) | TLBLO_VALID | dirty_bit;
				} else {
					newPT[i][j][k] = 0;
				}
			}
		}
	}
	return 0;
}