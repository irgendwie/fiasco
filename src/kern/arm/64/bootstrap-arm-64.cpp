IMPLEMENTATION [arm]:

#include "mem_layout.h"
#include "mmio_register_block.h"
#include "paging.h"
#include "config.h"

struct Bs_mem_map
{
  static Address phys_to_pmem(Address a)
  { return a; }
};

struct Bs_alloc
{
  Bs_alloc(void *base, Mword &free_map)
  : _p((Address)base), _free_map(free_map)
  {}

  static Address to_phys(void *v)
  { return reinterpret_cast<Address>(v); }

  static bool valid() { return true; }

  void *alloc(unsigned size)
  {
    (void) size;
    // assert (size == Config::PAGE_SIZE);
    // test that size is a power of two
    // assert (((size - 1) ^ size) == (size - 1));

    int x = __builtin_ffsl(_free_map);
    if (x == 0)
      return nullptr; // OOM

    _free_map &= ~(1UL << (x - 1));

    return reinterpret_cast<void *>(_p + Config::PAGE_SIZE * (x - 1));
  }

  Address _p;
  Mword &_free_map;
};

IMPLEMENTATION [arm && !cpu_virt]:

#include "cpu.h"

PUBLIC static inline NEEDS["cpu.h"]
void
Bootstrap::enable_paging(Mword)
{
  unsigned long control = Cpu::Sctlr_generic;

  Mem::dsb();
  asm volatile("tlbi vmalle1is");
  Mem::dsb();
  asm volatile("msr SCTLR_EL1, %[control]" : : [control] "r" (control));
  Mem::isb();
  asm volatile ("ic iallu; dsb nsh; isb");
}

static inline void
Bootstrap::set_mair0(Mword v)
{ asm volatile ("msr MAIR_EL1, %0" : : "r"(v)); }

static inline void
switch_from_el3_to_el1()
{
  Mword pfr0;
  asm volatile ("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
  if (((pfr0 >> 8) & 0xf) != 0)
    {
      // EL2 supported, set HCR (RW and HCD)
      asm volatile ("msr HCR_EL2, %0" : : "r"((1 << 29) | (1 << 31)));
    }

  // flush all E1 TLBs
  asm volatile ("tlbi alle1");

  // drain the data and instruction caches as we might switch from
  // secure to non-secure mode and only secure mode can clear
  // caches for secure memory.
  Mmu<Bootstrap::Cache_flush_area, true>::flush_cache();

  // setup SCR (disable monitor completely)
  asm volatile ("msr scr_el3, %0"
                : :
                "r"(Cpu::Scr_ns | Cpu::Scr_rw | Cpu::Scr_smd));

  Mword sctlr_el3;
  Mword sctlr_el1;
  asm ("mrs %0, sctlr_el3" : "=r"(sctlr_el3));
  asm ("mrs %0, sctlr_el1" : "=r"(sctlr_el1));
  sctlr_el1 &= ~(Cpu::Sctlr_m | Cpu::Sctlr_a | Cpu::Sctlr_sa
                 | Cpu::Sctlr_ee | Cpu::Sctlr_wxn
                 | Cpu::Sctlr_uma);
  sctlr_el1 |= Cpu::Sctlr_i | Cpu::Sctlr_c;
  sctlr_el1 |= sctlr_el3 & Cpu::Sctlr_ee;
  asm volatile ("msr sctlr_el1, %0" : : "r"(sctlr_el1));

  Mword tmp;
  // monitor mode .. switch to el1
  asm volatile (
      "   msr spsr_el3, %[psr] \n"
      "   adr x4, 1f           \n"
      "   msr elr_el3, x4      \n"
      "   mov %[tmp], sp       \n"
      "   eret                 \n"
      "1: mov sp, %[tmp]       \n"
      : [tmp]"=&r"(tmp)
      : [psr]"r"((0xf << 6) | 5)
      : "cc", "x4");
}

static void
Bootstrap::leave_hyp_mode()
{
  Mword cel;
  asm volatile ("mrs %0, CurrentEL" : "=r"(cel));
  cel >>= 2;
  cel &= 3;
  Mword tmp;

  switch (cel)
    {
    case 3:
      switch_from_el3_to_el1();
      break;

    case 2:
      // flush all E1 TLBs
      asm volatile ("tlbi alle1");
      // set HCR (RW and HCD)
      asm volatile ("msr HCR_EL2, %0" : : "r"((1 << 29) | (1 << 31)));
      asm volatile ("   mov %[tmp], sp       \n"
                    "   msr spsr_el2, %[psr] \n"
                    "   adr x4, 1f           \n"
                    "   msr elr_el2, x4      \n"
                    "   eret                 \n"
                    "1: mov sp, %[tmp]       \n"
                    : [tmp]"=&r"(tmp)
                    : [psr]"r"((0xf << 6) | 5)
                    : "cc", "x4");
      break;
    case 1:
    default:
      break;
    }
}

constexpr unsigned l0_idx(Unsigned64 va)
{ return (va >> 39) & 0x1ff; }

constexpr unsigned l1_idx(Unsigned64 va)
{ return (va >> 30) & 0x1ff; }

PUBLIC static Bootstrap::Phys_addr
Bootstrap::init_paging()
{
  leave_hyp_mode();
  Pdir  *ud = reinterpret_cast<Pdir *>(kern_to_boot(bs_info.pi.l0_dir));
  Kpdir *kd = reinterpret_cast<Kpdir *>(kern_to_boot(bs_info.pi.l0_vdir));


  Bs_alloc alloc(kern_to_boot(bs_info.pi.scratch), bs_info.pi.free_map);
  // create mapping of RAM @ Mem_layout::Map_base in kdir
  auto p = kd->walk(::Virt_addr(Mem_layout::Map_base), 1, false, alloc, Bs_mem_map());
  p.set_page(cxx::int_value<Phys_addr>(pt_entry(Phys_addr(Mem_layout::Sdram_phys_base), true, false)));
  // force allocation of MMIO page directory
  p = kd->walk(::Virt_addr(Mem_layout::Registers_map_start), 2, false, alloc, Bs_mem_map());

  set_mair0(Page::Mair0_prrr_bits);

  // create 1:1 mapping of RAM in the idle (user) page table
  auto up = ud->walk(::Virt_addr(Mem_layout::Sdram_phys_base), 1, false, alloc, Bs_mem_map());
  up.set_page(cxx::int_value<Phys_addr>(pt_entry(Phys_addr(Mem_layout::Sdram_phys_base), true, true)));

  asm volatile (
      "msr tcr_el1, %2   \n"
      "dsb sy            \n"
      "msr ttbr0_el1, %0 \n"
      "msr ttbr1_el1, %1 \n"
      "isb               \n"
      : :
      "r"(ud), "r"(kd), "r"(Page::Ttbcr_bits));



  return Phys_addr(0);
}

//---------------------------------------------------------------------------
IMPLEMENTATION [arm]:

static inline
Bootstrap::Order
Bootstrap::map_page_order()
{ return Order(30); }

PUBLIC static inline void
Bootstrap::add_initial_pmem()
{}

asm
(
".section .text.init,#alloc,#execinstr \n"
".global _start                        \n"
"_start:                               \n"
"     ldr x9, =_stack                  \n"
"     mov sp, x9                       \n"
"     bl	bootstrap_main         \n"
".previous                             \n"
".section .bss                         \n"
".p2align 4                            \n"
"	.space	4096                   \n"
"_stack:                               \n"
".previous                             \n"
);

// -----------------------------------------------------------------
IMPLEMENTATION [arm && cpu_virt]:

#include "cpu.h"

PUBLIC static inline NEEDS["cpu.h"]
void
Bootstrap::enable_paging(Mword)
{
  unsigned long control = Cpu::Sctlr_generic;

  Mem::dsb();
  asm volatile("tlbi alle2is");
  asm volatile("tlbi vmalle1is");
  Mem::dsb();
  asm volatile("msr SCTLR_EL2, %[control]" : : [control] "r" (control));
  Mem::isb();
  asm volatile ("ic iallu; dsb nsh; isb");
}

static inline void
Bootstrap::set_mair0(Mword v)
{ asm volatile ("msr MAIR_EL2, %0" : : "r"(v)); }

static void
Bootstrap::leave_el3()
{
  Mword cel;
  asm volatile ("mrs %0, CurrentEL" : "=r"(cel));
  cel >>= 2;
  cel &= 3;

  if (cel != 3)
    return; // not on EL3 so do nothing

  Mword pfr0;
  asm volatile ("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
  if (((pfr0 >> 8) & 0xf) == 0)
    {
      // EL2 not supported, crash
      for (;;)
        ;
    }

  asm volatile ("msr HCR_EL2, %0" : : "r"(1 << 31));

  // flush all E2 TLBs
  asm volatile ("tlbi alle2is");

  // setup SCR (disable monitor completely)
  asm volatile ("msr scr_el3, %0"
                : :
                "r"(Cpu::Scr_ns | Cpu::Scr_rw | Cpu::Scr_smd));

  Mword sctlr_el3;
  Mword sctlr;
  asm ("mrs %0, sctlr_el3" : "=r"(sctlr_el3));
  sctlr = Cpu::Sctlr_generic & ~Cpu::Sctlr_m;
  sctlr |= sctlr_el3 & Cpu::Sctlr_ee;
  asm volatile ("dsb sy" : : : "memory");
  // drain the data and instrcution caches as we might switch from
  // secure to non-secure mode and only secure mode can clear
  // caches for secure memory.
  Mmu<Bootstrap::Cache_flush_area, true>::flush_cache();

  asm volatile ("msr sctlr_el2, %0" : : "r"(sctlr) : "memory");

  Mword tmp;
  // monitor mode .. switch to el2
  asm volatile (
      "   msr spsr_el3, %[psr] \n"
      "   adr x4, 1f           \n"
      "   msr elr_el3, x4      \n"
      "   mov %[tmp], sp       \n"
      "   eret                 \n"
      "1: mov sp, %[tmp]       \n"
      : [tmp]"=&r"(tmp)
      : [psr]"r"((0xf << 6) | 9)
      : "cc", "x4");
}

constexpr unsigned l0_idx(Unsigned64 va)
{ return (va >> 39) & 0x1ff; }

constexpr unsigned l1_idx(Unsigned64 va)
{ return (va >> 30) & 0x1ff; }

PUBLIC static Bootstrap::Phys_addr
Bootstrap::init_paging()
{
  leave_el3();

  Kpdir *d = reinterpret_cast<Kpdir *>(kern_to_boot(bs_info.pi.l0_dir));
  set_mair0(Page::Mair0_prrr_bits);

  Bs_alloc alloc(kern_to_boot(bs_info.pi.scratch), bs_info.pi.free_map);
  auto p = d->walk(::Virt_addr(Mem_layout::Sdram_phys_base), 1, false, alloc, Bs_mem_map());
  // assert (p.level() == 1)
  p.set_page(cxx::int_value<Phys_addr>(pt_entry(Phys_addr(Mem_layout::Sdram_phys_base), true, false)));

  p = d->walk(::Virt_addr(Mem_layout::Registers_map_start), 2, false, alloc, Bs_mem_map());

  asm volatile (
      "msr tcr_el2, %1   \n"
      "dsb sy            \n"
      "msr ttbr0_el2, %0 \n"
      "isb               \n"
      : :
      "r"(d), "r"(Page::Ttbcr_bits));

  return Phys_addr(0);
}


