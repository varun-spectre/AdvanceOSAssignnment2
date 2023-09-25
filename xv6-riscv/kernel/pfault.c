/* This file contains code for a generic page fault handler for processes. */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz);
int flags2perm(int flags);

/* CSE 536: (2.4) read current time. */
uint64 read_current_timestamp()
{
    uint64 curticks = 0;
    acquire(&tickslock);
    curticks = ticks;
    wakeup(&ticks);
    release(&tickslock);
    return curticks;
}

bool psa_tracker[PSASIZE];

/* All blocks are free during initialization. */
void init_psa_regions(void)
{
    for (int i = 0; i < PSASIZE; i++)
        psa_tracker[i] = false;
}

/* Evict heap page to disk when resident pages exceed limit */
void evict_page_to_disk(struct proc *p)
{
    /* Find free block */
    int blockno = 0;
    for (int i = 0; i < PSASIZE; i++)
    {
        if (psa_tracker[i] == false)
        {
            blockno = i;
            break;
        }
    }

    /* Find victim page using FIFO. */
    // find the heap page that was loaded the longest time ago using heap_tracker.
    uint64 min_time = 0xFFFFFFFFFFFFFFFF;
    int min_index = 0;
    for (int i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr != 0xFFFFFFFFFFFFFFFF)
        {
            if (p->heap_tracker[i].last_load_time < min_time)
            {
                min_time = p->heap_tracker[i].last_load_time;
                min_index = i;
            }
        }
    }

    // p->heap_tracker[min_index].loaded = false;
    // p->heap_tracker[min_index].last_load_time = 0xFFFFFFFFFFFFFFFF;
    p->heap_tracker[min_index].startblock = blockno;
    // p->heap_tracker[min_index].addr = 0xFFFFFFFFFFFFFFFF;

    /* Print statement. */
    print_evict_page(0, 0);
    /* Read memory from the user to kernel memory first. */
    char *kpage = kalloc();
    copyin(p->pagetable, kpage, p->heap_tracker[min_index].addr, PGSIZE);

    /* Write to the disk blocks. Below is a template as to how this works. There is
     * definitely a better way but this works for now. :p */
    struct buf *b;
    b = bread(1, PSASTART + (blockno));
    // Copy page contents to b.data using memmove.
    memmove(b->data, kpage, PGSIZE);
    bwrite(b);
    brelse(b);
    /*update PSA*/
    for (int i = 0; i < PGSIZE; i++)
    {
        psa_tracker[blockno + i] = true;
    }
    /* Free kernel page. */
    kfree(kpage);

    /* Unmap swapped out page */
    uvmunmap(p->pagetable, p->heap_tracker[min_index].addr, 1, 1);

    /* Update the resident heap tracker. */
    p->resident_heap_pages -= 1;
}

/* Retrieve faulted page from disk. */
void retrieve_page_from_disk(struct proc *p, uint64 uvaddr)
{
    /* Find where the page is located in disk */
    int blockno = 0;
    for (int i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr != 0xFFFFFFFFFFFFFFFF)
        {
            uint64 heap_start = p->heap_tracker[i].addr;
            uint64 heap_end = heap_start + PGSIZE;

            if (uvaddr >= heap_start && uvaddr < heap_end)
            {
                blockno = p->heap_tracker[i].startblock;
                break;
            }
        }
    }

    /* Print statement. */
    print_retrieve_page(0, 0);

    /* Create a kernel page to read memory temporarily into first. */
    char *kpage = kalloc();

    /* Read the disk block into temp kernel page. */
    struct buf *b;
    b = bread(1, PSASTART + (blockno));
    memmove(kpage, b->data, PGSIZE);
    brelse(b);

    // should I free the PSA here?

    /* Copy from temp kernel page to uvaddr (use copyout) */
    copyout(p->pagetable, uvaddr, kpage, PGSIZE);
}

void page_fault_handler(void)
{
    // printf("inside page fault handler\n");
    /* Current process struct */
    struct proc *p = myproc();

    /* Track whether the heap page should be brought back from disk or not. */
    // bool load_from_disk = false;

    /* Find faulting address. */

    uint64 faulting_addr = r_stval();
    faulting_addr = faulting_addr >> 12;
    faulting_addr = faulting_addr << 12;
    print_page_fault(p->name, faulting_addr);

    /* Check if the fault address is a heap page. Use p->heap_tracker */
    for (int i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr != 0xFFFFFFFFFFFFFFFF)
        {
            uint64 heap_start = p->heap_tracker[i].addr;
            uint64 heap_end = heap_start + PGSIZE;

            if (faulting_addr >= heap_start && faulting_addr < heap_end)
            {
                goto heap_handle;
            }
        }
    }

    /* If it came here, it is a page from the program binary that we must load. */
    // char *s, *last;
    int i, off;
    // uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    // pagetable_t pagetable = p->pagetable;

    begin_op();

    if ((ip = namei(p->name)) == 0)
    {
        end_op();
        return;
    }
    ilock(ip);

    // Check ELF header
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto out;

    if (elf.magic != ELF_MAGIC)
        goto out;

    // if ((pagetable = proc_pagetable(p)) == 0)
    //     goto out;

    // Load program into memory.
    // printf("starting iteration over program headers\n");
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
    {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            goto out;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            goto out;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto out;
        if (ph.vaddr % PGSIZE != 0)
            goto out;
        // printf("ph.vaddr: %p\n", ph.vaddr);
        // printf("ph.memsz: %p\n", ph.memsz);
        // printf("faulting_addr: %p\n", faulting_addr);
        if (ph.vaddr <= faulting_addr && faulting_addr < ph.vaddr + ph.memsz)
        {
            // printf("found the segment. loading...\n");
            uint64 offset_in_file = faulting_addr - ph.vaddr;

            uint64 sz1;
            if ((sz1 = uvmalloc(p->pagetable, faulting_addr, faulting_addr + PGSIZE, flags2perm(ph.flags))) == 0)
                goto out;

            print_load_seg(faulting_addr, ph.off + offset_in_file, PGSIZE);

            if (loadseg(p->pagetable, faulting_addr, ip, ph.off + offset_in_file, PGSIZE) < 0)
                goto out;
            // printf("loaded segment\n");
        }
    }

    iunlockput(ip);
    end_op();
    ip = 0;

    /* Go to out, since the remainder of this code is for the heap. */
    goto out;

heap_handle:
    // printf("inside heap handle\n");
    /* 2.4: Check if resident pages are more than heap pages. If yes, evict. */
    if (p->resident_heap_pages == MAXRESHEAP)
    {
        evict_page_to_disk(p);
    }

    /* 2.3: Map a heap page into the process' address space. (Hint: check growproc) */
    uint64 va = faulting_addr;
    uint64 sz = p->sz;
    if (sz >= MAXVA)
        goto out;

    if ((sz = uvmalloc(p->pagetable, va, va + PGSIZE, PTE_W)) == 0)
        goto out;

    /* 2.4: Update the last load time for the loaded heap page in p->heap_tracker. */
    int heap_tracker_block = -1;
    for (int i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr != 0xFFFFFFFFFFFFFFFF)
        {
            uint64 heap_start = p->heap_tracker[i].addr;
            uint64 heap_end = heap_start + PGSIZE;

            if (faulting_addr >= heap_start && faulting_addr < heap_end)
            {
                p->heap_tracker[i].loaded = true;
                p->heap_tracker[i].last_load_time = read_current_timestamp();
                heap_tracker_block = i;
            }
        }
    }
    /* 2.4: Heap page was swapped to disk previously. We must load it from disk. */
    if (p->heap_tracker[heap_tracker_block].startblock != -1)
    {
        retrieve_page_from_disk(p, faulting_addr);
    }

    /* Track that another heap page has been brought into memory. */
    p->resident_heap_pages++;

out:
    /* Flush stale page table entries. This is important to always do. */
    sfence_vma();
    return;
}
