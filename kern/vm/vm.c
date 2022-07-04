#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <spl.h>
#include <proc.h>
#include <current.h>

/* Place your page table functions here */

/* Initialise Page Table */
int insertPageTableEntry(paddr_t ***pt, paddr_t paddr, vaddr_t address)
{
    uint32_t mostsb = get_8_mostsb(address);
    uint32_t middlesb = get_6_middlesb(address);
    uint32_t leastsb = get_6_leastsb(address);

    if (mostsb >= PTLEVEL1 || middlesb >= PTLEVEL2 || leastsb >= PTLEVEL3) {
        return EFAULT;
    }
    /* If the 2nd level page table does not exist yet, malloc it */
    if (pt[mostsb] == NULL) {
        pt[mostsb] = kmalloc(sizeof(paddr_t *) * PTLEVEL2);
        if (pt[mostsb] == NULL) {
            return ENOMEM;
        }
        for (int i = 0; i < PTLEVEL2; i++) {
            pt[mostsb][i] = NULL;
        }
    }
    /* If the 3rd level page table does not exist yet, malloc it */
    if (pt[mostsb][middlesb] == NULL) {
        pt[mostsb][middlesb] = kmalloc(sizeof(paddr_t) * PTLEVEL3);
        if (pt[mostsb][middlesb] == NULL) {
            return ENOMEM;
        }
        for (int i = 0; i < PTLEVEL3; i++) {
            pt[mostsb][middlesb][i] = 0; // Zero fill page table
        }
    }
    /* If the page table already has a value, return an error */
    if (pt[mostsb][middlesb][leastsb] != 0) {
        return EFAULT;
    }
    /* Assign the frame to the PTE */
    pt[mostsb][middlesb][leastsb] = paddr;

    return 0;
}

int checkPT(paddr_t ***pt, vaddr_t vaddress) {
    uint32_t mostsb = get_8_mostsb(vaddress);
    uint32_t middlesb = get_6_middlesb(vaddress);
    uint32_t leastsb = get_6_leastsb(vaddress);

    if (mostsb >= PTLEVEL1 || middlesb >= PTLEVEL2 || leastsb >= PTLEVEL3) {
        return 1; // Address outside of index range
    }
    if (pt == NULL) {
        return 1;
    }
    if (pt[mostsb] == NULL) {
        return 1;
    }
    if (pt[mostsb][middlesb] == NULL) {
        return 1;
    }
    if (pt[mostsb][middlesb][leastsb] == 0) {
        return 1;
    }
    return 0;
}

paddr_t page_table_lookup(paddr_t*** pt, vaddr_t faultaddress) {
    uint32_t mostsb = get_8_mostsb(faultaddress);
    uint32_t middlesb = get_6_middlesb(faultaddress);
    uint32_t leastsb = get_6_leastsb(faultaddress);

    if (checkPT(pt, faultaddress)) {
        return 1;
    }
    paddr_t ret = pt[mostsb][middlesb][leastsb];
    return ret;
}

struct as_region * addr_to_region(struct as_region *region, vaddr_t faultaddress) {
    
    struct as_region *curr = region;

    while (curr != NULL) {
        if (faultaddress >= curr->base && ((curr->base + curr->size) > faultaddress)) {
            break;
        }
        curr = curr->next;
    }
    
    return curr;
}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *  
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */
}
/*
paddr_t initPageTableEntry(struct as_region *fault_region, paddr_t new_page) {
    if (fault_region->writeable != 0) {
        new_page = new_page | TLBLO_DIRTY;
    }
    new_page = new_page | TLBLO_VALID;
    return new_page;
}
*/
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    if (faulttype == VM_FAULT_READONLY) {
        return EFAULT;
    }
    
    struct addrspace *as = proc_getas();
    if (as == NULL) {
        return EFAULT;
    }

    paddr_t pte = page_table_lookup(as->as_pt, faultaddress);   
    
    if (pte & TLBLO_VALID) {
        /* Check write permissions */
        if ((faulttype == VM_FAULT_WRITE) && ((pte & TLBLO_DIRTY) == 0)) {
            return EFAULT;
        }
        insert_into_tlb(faultaddress & PAGE_FRAME, pte);
        return 0;
    }
    /* Traverse the list of regions to find the region for the faultaddress */
    struct as_region *fault_region = addr_to_region(as->as_regions, faultaddress);

    /* If fault region is NULL, then it references memory that was not
    setup by as_define_region() */
    if (fault_region == NULL) {
        return EFAULT;
    }
    /* Check for write permissions */
    if ((faulttype == VM_FAULT_WRITE) && (fault_region->writeable == 0)) {
        return EFAULT;
    }

    /* Check for read permissions */
    if ((faulttype == VM_FAULT_READ) && (fault_region->readable == 0)) {
        return EFAULT;
    }

    /* Allocating a new page for user */
    vaddr_t new_page = alloc_kpages(1);
    if (new_page == 0) {
        return ENOMEM;
    }
    bzero((void *)new_page, PAGE_SIZE);
    paddr_t new_pte = KVADDR_TO_PADDR(new_page) & PAGE_FRAME;

    /* Construct a new page table entry */
    //paddr_t new_pte = initPageTableEntry(fault_region, paddress);
    if (fault_region->writeable != 0) {
        new_pte = new_pte | TLBLO_DIRTY;
    }
    new_pte = new_pte | TLBLO_VALID;

    /* Insert page table entry into PT */
    int ret = insertPageTableEntry(as->as_pt, new_pte, faultaddress);

    /* If there are errors */
    if (ret) {
        /* Free the new_page */
        free_kpages(new_page);
        return ret;
    }
    /* Insert into the tlb */
    insert_into_tlb(faultaddress & PAGE_FRAME, new_pte);

    /* Success */
    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

vaddr_t get_8_mostsb(vaddr_t address) {
    return address >> 24;
}

vaddr_t get_6_middlesb(vaddr_t address) {
    return (address << 8) >> 26;
}

vaddr_t get_6_leastsb(vaddr_t address) {
    return (address << 14) >> 26;
}

void insert_into_tlb(vaddr_t faultaddress, paddr_t new_pte) {
    uint32_t entry_hi = faultaddress & TLBHI_VPAGE;
    uint32_t entry_lo = new_pte;

    int spl = splhigh();
    tlb_random(entry_hi, entry_lo);
    splx(spl);
    return;
}