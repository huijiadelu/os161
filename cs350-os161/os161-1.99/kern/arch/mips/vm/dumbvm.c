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
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"
/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
#if OPT_A3
static struct spinlock cmap_lock = SPINLOCK_INITIALIZER;
static unsigned int size = 0;
static paddr_t start = 0;
static paddr_t end = 0;
static bool ok = false;
#endif 

void vm_bootstrap(void) {
#if OPT_A3
	ram_getsize(&start, &end);
	size = (end - start) / PAGE_SIZE;
	for (unsigned int i = 0; i < size; i++) {
		((int *) PADDR_TO_KVADDR(start))[i] = 0;
	}
	ok = true;
#endif
}

static paddr_t getppages(unsigned long npages) {
	paddr_t addr;
#if OPT_A3
	if (ok) {
		spinlock_acquire(&cmap_lock);
		addr = cmap_stealmem(npages);
		spinlock_release(&cmap_lock);
	} else {
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	}
#else
	spinlock_acquire(&stealmem_lock);
	addr = ram_stealmem(npages);
	spinlock_release(&stealmem_lock);
#endif
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(int npages) {
	paddr_t pa;
	pa = getppages(npages);
	if (pa == 0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr) {
#if OPT_A3
	paddr_t p_address = KVADDR_TO_PADDR(addr);

	spinlock_acquire(&cmap_lock);


	unsigned int temp = ((p_address - start) / PAGE_SIZE) - 1;
	KASSERT(temp < size);
	((int*) PADDR_TO_KVADDR(start))[temp] = 0;
	temp++;
	int current = ((int*) PADDR_TO_KVADDR(start))[temp];

	while (current != 1 && current != 0) {
		((int*) PADDR_TO_KVADDR(start))[temp] = 0;
		temp++;
		current = ((int*) PADDR_TO_KVADDR(start))[temp];
	}


	spinlock_release(&cmap_lock);
#else
	(void)addr;
#endif
}

void vm_tlbshootdown_all(void) {
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;
	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);
	switch (faulttype) {
		case VM_FAULT_READONLY:
#if OPT_A3
			return EFAULT; //bad address

#else

			panic("dumbvm: got VM_FAULT_READONLY\n");

#endif
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
			break;
		default:
			return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;


#if OPT_A3
	bool seg = false;
	bool loadelf = as->loadelf;
	
	if (faultaddress < vtop1 && faultaddress >= vbase1 ) {
		seg = true;
		paddr = (faultaddress - vbase1) + as->as_pbase1[0];
	} else if (faultaddress < stacktop && faultaddress >= stackbase ) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase[0];
	} else if (faultaddress < vtop2 && faultaddress >= vbase2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2[0];
	} else {
		return EFAULT;
	}
#else
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}
#endif
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;

#if OPT_A3
		if (seg && loadelf) {
			elo &= ~TLBLO_DIRTY;
		}
#endif

		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	ehi = faultaddress;
	elo = paddr | TLBLO_VALID | TLBLO_DIRTY;
	if (seg && loadelf) { 
		elo &= ~TLBLO_DIRTY;
	}
	tlb_random(ehi, elo);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

struct addrspace* as_create(void) {
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
	return as;
}

void as_destroy(struct addrspace *as) {
#if OPT_A3
	for (unsigned int i = 0; i < as->as_npages1; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase1[i]));
	}
	for (unsigned int i = 0; i < as->as_npages2; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase2[i]));
	}
	for (unsigned int i = 0; i < DUMBVM_STACKPAGES; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_stackpbase[i]));
	}
#endif
	kfree(as);
}

void as_activate(void) {
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
	/* Kernel threads don't have an address spaces to activate */
#endif
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
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		int readable, int writeable, int executable) {
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
#if OPT_A3
		as->as_pbase1 = kmalloc(sizeof(paddr_t) * npages);
#endif
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
#if OPT_A3
		as->as_pbase2 = kmalloc(sizeof(paddr_t) * npages);
#endif
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static void as_zero_region(paddr_t paddr, unsigned npages) {
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int as_prepare_load(struct addrspace *as) {
#if OPT_A3
	for (unsigned int i = 0; i < as->as_npages1; i++) {
		paddr_t adr = getppages(1);
		if (adr == 0) return ENOMEM;
		as->as_pbase1[i] = adr;
		as_zero_region(as->as_pbase1[i], 1);
	}
	for (unsigned int i = 0; i < as->as_npages2; i++) {
		paddr_t adr = getppages(1);
		if (adr == 0) return ENOMEM;
		as->as_pbase2[i] = adr;
		as_zero_region(as->as_pbase2[i], 1);
	}
	as->as_stackpbase = kmalloc(sizeof(paddr_t) * DUMBVM_STACKPAGES);
	if (as->as_stackpbase == NULL) {
		return ENOMEM;
	}
	for (unsigned int i = 0; i < DUMBVM_STACKPAGES; i++) {
		paddr_t adr = getppages(1);
		if (adr == 0) return ENOMEM;
		as->as_stackpbase[i] = adr;
		as_zero_region(as->as_stackpbase[i], 1);
	}
#else
	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}

	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif
	return 0;
}

int as_complete_load(struct addrspace *as) {
	(void)as;
	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new == NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

#if OPT_A3

	new->as_pbase1 = kmalloc(sizeof(paddr_t) * old->as_npages1);
	new->as_pbase2 = kmalloc(sizeof(paddr_t) * old->as_npages2);
	new->as_stackpbase = kmalloc(sizeof(paddr_t) * DUMBVM_STACKPAGES);

#endif
	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

#if OPT_A3
	for (unsigned int i = 0; i < old->as_npages1; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]),
				(const void *)PADDR_TO_KVADDR(old->as_pbase1[i]),PAGE_SIZE);
	}
	for (unsigned int i = 0; i < old->as_npages2; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[i]),
				(const void *)PADDR_TO_KVADDR(old->as_pbase2[i]),PAGE_SIZE);
	}
	for (unsigned int i = 0; i < DUMBVM_STACKPAGES; i++) {
		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[i]),
				(const void *)PADDR_TO_KVADDR(old->as_stackpbase[i]),PAGE_SIZE);
	}
#else
	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
			(const void *)PADDR_TO_KVADDR(old->as_pbase1),
			old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
			(const void *)PADDR_TO_KVADDR(old->as_pbase2),
			old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
			(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
			DUMBVM_STACKPAGES*PAGE_SIZE);
#endif
	*ret = new;
	return 0;

}
#if OPT_A3
paddr_t cmap_stealmem(unsigned long pages) {
	unsigned int temp = 0;
	while (temp < size) {
		int cur = ((int*) PADDR_TO_KVADDR(start))[temp];
		unsigned int s = temp;
		unsigned long cnt = 0;

		while (s + cnt < size && cur == 0) {
			cnt++;
			if (cnt == pages) {
				temp = s;
				for (unsigned long i = 1; i <= pages; i++) {
					((int *) PADDR_TO_KVADDR(start))[temp] = (int) i;
					temp++;
				}
				unsigned int ps = s;
				unsigned long np = pages;
				(void)ps;
				(void)np;
				return (s + 1) * PAGE_SIZE + start;
			}
			temp++;
			cur = ((int*) PADDR_TO_KVADDR(start))[temp];
		}
		temp++;
	}

	return 0;
}
#endif
