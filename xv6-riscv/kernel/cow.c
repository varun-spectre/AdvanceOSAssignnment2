#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include <stdbool.h>

struct spinlock cow_lock;

// Max number of pages a CoW group of processes can share
#define SHMEM_MAX 100

struct cow_group
{
    int group;               // group id
    uint64 shmem[SHMEM_MAX]; // list of pages a CoW group share
    int count;               // Number of active processes
};

struct cow_group cow_group[NPROC];

struct cow_group *get_cow_group(int group)
{
    if (group == -1)
        return 0;

    for (int i = 0; i < NPROC; i++)
    {
        if (cow_group[i].group == group)
            return &cow_group[i];
    }
    return 0;
}

void cow_group_init(int groupno)
{
    for (int i = 0; i < NPROC; i++)
    {
        if (cow_group[i].group == -1)
        {
            cow_group[i].group = groupno;
            return;
        }
    }
}

int get_cow_group_count(int group)
{
    return get_cow_group(group)->count;
}
void incr_cow_group_count(int group)
{
    get_cow_group(group)->count = get_cow_group_count(group) + 1;
}
void decr_cow_group_count(int group)
{
    get_cow_group(group)->count = get_cow_group_count(group) - 1;
}

void add_shmem(int group, uint64 pa)
{
    if (group == -1)
        return;

    uint64 *shmem = get_cow_group(group)->shmem;
    int index;
    for (index = 0; index < SHMEM_MAX; index++)
    {
        // duplicate address
        if (shmem[index] == pa)
            return;
        if (shmem[index] == 0)
            break;
    }
    shmem[index] = pa;
}

int is_shmem(int group, uint64 pa)
{
    if (group == -1)
        return 0;

    uint64 *shmem = get_cow_group(group)->shmem;
    for (int i = 0; i < SHMEM_MAX; i++)
    {
        if (shmem[i] == 0)
            return 0;
        if (shmem[i] == pa)
            return 1;
    }
    return 0;
}

int rem_shem(int group, uint64 pa)
{
    if (group == -1)
        return 0;

    uint64 *shmem = get_cow_group(group)->shmem;
    for (int i = 0; i < SHMEM_MAX; i++)
    {
        if (shmem[i] == 0)
            return 0;
        if (shmem[i] == pa)
        {
            shmem[i] = 0;
            return 1;
        }
    }
    return 0;
}

// helper function for uvmunmap
// return 1 if the page is shared by only one process else zero
int remove_shmem(uint64 pa)
{
    for (int i = 0; i < NPROC; i++)
    {
        if (is_shmem(cow_group[i].group, pa))
        {
            if (get_cow_group_count(cow_group[i].group) == 1)
            {
                rem_shem(cow_group[i].group, pa);
                return 1;
            }
            else
            {
                return 0;
            }
        }
    }
    return 0;
}

// helper function for uvmunmap
// check if a physical address is shared by any process return 1 if true else 0
int is_shmem_any(uint64 pa)
{
    for (int i = 0; i < NPROC; i++)
    {
        if (is_shmem(cow_group[i].group, pa))
        {
            return 1;
        }
    }
    return 0;
}

void cow_init()
{
    for (int i = 0; i < NPROC; i++)
    {
        cow_group[i].count = 0;
        cow_group[i].group = -1;
        for (int j = 0; j < SHMEM_MAX; j++)
            cow_group[i].shmem[j] = 0;
    }
    initlock(&cow_lock, "cow_lock");
}

int uvmcopy_cow(pagetable_t old, pagetable_t new, uint64 sz)
{

    /* CSE 536: (2.6.1) Handling Copy-on-write fork() */

    // Copy user vitual memory from old(parent) to new(child) process

    // Map pages as Read-Only in both the processes
    pte_t *pte;
    uint64 pa, i;
    uint flags;

    for (i = 0; i < sz; i += PGSIZE)
    {
        if ((pte = walk(old, i, 0)) == 0)
            panic("uvmcopy: pte should exist");
        if ((*pte & PTE_V) == 0)
            panic("uvmcopy: page not present");
        *pte &= ~PTE_W;
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        // if (mappages(old, i, PGSIZE, (uint64)pa, flags) != 0)
        // {
        //     goto err;
        // }
        if (mappages(new, i, PGSIZE, (uint64)pa, flags) != 0)
        {
            goto err;
        }
        // Track the shared pages in a CoW group
        add_shmem(myproc()->cow_group, pa);
    }

    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}

void copy_on_write()
{
    /* CSE 536: (2.6.2) Handling Copy-on-write */
    struct proc *p = myproc();
    uint64 faulting_addr = r_stval();
    faulting_addr = faulting_addr >> 12;
    faulting_addr = faulting_addr << 12;
    print_copy_on_write(p, faulting_addr);

    // Allocate a new page
    char *mem;
    uint64 pa;
    pte_t *pte;
    uint flags;
    if ((mem = kalloc()) == 0)
        panic("error allocating memory in copy_on_write");

    // find the page table entry for the faulting address

    pte = walk(p->pagetable, faulting_addr, 0);
    if (pte == 0)
        panic("error finding page table entry in copy_on_write");
    if ((*pte & PTE_V) == 0)
        panic("page not present in copy_on_write");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    flags |= PTE_W;
    // Check if the page is shared
    // if (!is_shmem(p->cow_group, pa))
    //     panic("page not shared in copy_on_write");

    // Copy contents from the shared page to the new page
    memmove(mem, (char *)pa, PGSIZE);

    // Unmap the shared page from the faulting process's page table
    uvmunmap(p->pagetable, faulting_addr, 1, 0);

    // Map the new page in the faulting process's page table with write permissions
    if (mappages(p->pagetable, faulting_addr, PGSIZE, (uint64)mem, flags) != 0)
        panic("error mapping pages in copy_on_write");

    // As we are removing the shared page, decrement the count of processes sharing the page
    // decr_cow_group_count(p->cow_group);

    // If reference count is zero, remove the shared page
    // if (get_cow_group_count(p->cow_group) == 0)
    // {
    //     rem_shem(p->cow_group, pa);
    //     kfree((void *)pa);
    // }
}
