/*
 * Emulation for instructions covered by User-Mode Instruction Prevention
 *
 * Copyright (C) 2019 Brendan Shanks for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * User-Mode Instruction Prevention (UMIP) is an x86 feature that prevents user-space
 * code (CPL > 0) from executing the sgdt, sidt, sldt, smsw, and str instructions.
 * If one of these instructions is executed, a general protection fault is issued.
 *
 * UMIP was first implemented by the AMD Zen2 and Intel Sunny Cove microarchitectures
 * (both released in 2019).
 *
 * On Linux:
 * 32-bit processes: The kernel traps the GPF and emulates sgdt, sidt, and smsw.
 *                   sldt and str are not emulated.
 *
 * 64-bit processes: Kernel 5.4 and newer emulate sgdt, sidt, and smsw. sldt and str are not emulated.
 *                   Kernels older than v5.4 do not emulate any instructions.
 *
 * When the kernel doesn't emulate an instruction, the process receives a SIGSEGV.
 * This file contains functions used by the 32- and 64-bit signal handlers to emulate all
 * instructions not emulated by the Linux kernel.
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "wine/asm.h"
#include "wine/debug.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

/* Dummy base addresses are the same as used by the kernel */
#define UMIP_DUMMY_GDT_BASE 0xfffffffffffe0000ULL
#define UMIP_DUMMY_IDT_BASE 0xffffffffffff0000ULL
#define UMIP_DUMMY_GDT_IDT_LIMIT 0

#define UMIP_GDT_IDT_BASE_SIZE_64BIT 8
#define UMIP_GDT_IDT_LIMIT_SIZE 2

/* Dummy LDT, matches what I see on Windows and Linux */
#define UMIP_DUMMY_LDT 0

/* Dummy task register, matches what I see on Windows and Linux */
#define UMIP_DUMMY_TR 0x40

/* Dummy MSW is same value used by kernel, defined as:
 * (X86_CR0_PE | X86_CR0_MP | X86_CR0_ET |
 *  X86_CR0_NE | X86_CR0_WP | X86_CR0_AM |
 *  X86_CR0_PG)
 */
#define UMIP_DUMMY_MSW 0x33

#if defined(__i386__) && defined(linux)

static void *get_reg_address( CONTEXT *context, BYTE rm )
{
    switch (rm & 7)
    {
    case 0: return &context->Eax;
    case 1: return &context->Ecx;
    case 2: return &context->Edx;
    case 3: return &context->Ebx;
    case 4: return &context->Esp;
    case 5: return &context->Ebp;
    case 6: return &context->Esi;
    case 7: return &context->Edi;
    }
    return NULL;
}


/***********************************************************************
 *           INSTR_GetOperandAddr
 *
 * Return the address of an instruction operand (from the mod/rm byte).
 */
static int INSTR_GetOperandAddr( CONTEXT *context, BYTE *instr, unsigned int instr_len,
                                 int long_addr, int segprefix, int *len, void **addr )
{
    int mod, rm, base = 0, index = 0, ss = 0, off;
    unsigned int i = 0;

#define GET_VAL( val, type ) \
    { if (sizeof(type) > (instr_len - i)) return 0; \
      *val = *(type *)&instr[i]; i += sizeof(type); *len += sizeof(type); }

    *len = 0;
    GET_VAL( &mod, BYTE );
    rm = mod & 7;
    mod >>= 6;

    if (mod == 3)
    {
        *addr = get_reg_address( context, rm );
        return 1;
    }

    if (long_addr)
    {
        if (rm == 4)
        {
            BYTE sib;
            GET_VAL( &sib, BYTE );
            rm = sib & 7;
            ss = sib >> 6;
            switch((sib >> 3) & 7)
            {
            case 0: index = context->Eax; break;
            case 1: index = context->Ecx; break;
            case 2: index = context->Edx; break;
            case 3: index = context->Ebx; break;
            case 4: index = 0; break;
            case 5: index = context->Ebp; break;
            case 6: index = context->Esi; break;
            case 7: index = context->Edi; break;
            }
        }

        switch(rm)
        {
        case 0: base = context->Eax; break;
        case 1: base = context->Ecx; break;
        case 2: base = context->Edx; break;
        case 3: base = context->Ebx; break;
        case 4: base = context->Esp; break;
        case 5: base = context->Ebp; break;
        case 6: base = context->Esi; break;
        case 7: base = context->Edi; break;
        }
        switch (mod)
        {
        case 0:
            if (rm == 5)  /* special case: ds:(disp32) */
            {
                GET_VAL( &base, DWORD );
            }
            break;

        case 1:  /* 8-bit disp */
            GET_VAL( &off, BYTE );
            base += (signed char)off;
            break;

        case 2:  /* 32-bit disp */
            GET_VAL( &off, DWORD );
            base += (signed long)off;
            break;
        }
    }
    else  /* short address */
    {
        switch(rm)
        {
        case 0:  /* ds:(bx,si) */
            base = LOWORD(context->Ebx) + LOWORD(context->Esi);
            break;
        case 1:  /* ds:(bx,di) */
            base = LOWORD(context->Ebx) + LOWORD(context->Edi);
            break;
        case 2:  /* ss:(bp,si) */
            base = LOWORD(context->Ebp) + LOWORD(context->Esi);
            break;
        case 3:  /* ss:(bp,di) */
            base = LOWORD(context->Ebp) + LOWORD(context->Edi);
            break;
        case 4:  /* ds:(si) */
            base = LOWORD(context->Esi);
            break;
        case 5:  /* ds:(di) */
            base = LOWORD(context->Edi);
            break;
        case 6:  /* ss:(bp) */
            base = LOWORD(context->Ebp);
            break;
        case 7:  /* ds:(bx) */
            base = LOWORD(context->Ebx);
            break;
        }

        switch(mod)
        {
        case 0:
            if (rm == 6)  /* special case: ds:(disp16) */
            {
                GET_VAL( &base, WORD );
            }
            break;

        case 1:  /* 8-bit disp */
            GET_VAL( &off, BYTE );
            base += (signed char)off;
            break;

        case 2:  /* 16-bit disp */
            GET_VAL( &off, WORD );
            base += (signed short)off;
            break;
        }
        base &= 0xffff;
    }
    /* FIXME: we assume that all segments have a base of 0 */
    *addr = (void *)(base + (index << ss));
    return 1;
#undef GET_VAL
}

/***********************************************************************
 *           emulate_instruction
 *
 * Emulate a UMIP-protected instruction.
 * Returns: 0 if no instruction emulated, 1 if instruction emulated successfully,
 *          2 if instruction's write to memory failed.
 */
static int emulate_instruction( CONTEXT *context, void **err_addr )
{
    int prefix, segprefix, prefixlen, long_op, long_addr;
    unsigned int i, len;
    BYTE instr[16];

    long_op = long_addr = 1;
    len = virtual_uninterrupted_read_memory( (BYTE *)context->Eip, instr, sizeof(instr) );

    /* First handle any possible prefix */

    segprefix = -1;  /* no prefix */
    prefix = 1;
    for (i = 0; i < len; i++)
    {
        switch(instr[i])
        {
        case 0x2e:
            segprefix = context->SegCs;
            break;
        case 0x36:
            segprefix = context->SegSs;
            break;
        case 0x3e:
            segprefix = context->SegDs;
            break;
        case 0x26:
            segprefix = context->SegEs;
            break;
        case 0x64:
            segprefix = context->SegFs;
            break;
        case 0x65:
            segprefix = context->SegGs;
            break;
        case 0x66:
            long_op = !long_op;  /* opcode size prefix */
            break;
        case 0x67:
            long_addr = !long_addr;  /* addr size prefix */
            break;
        case 0xf0:  /* lock */
	    break;
        case 0xf2:  /* repne */
	    break;
        case 0xf3:  /* repe */
            break;
        default:
            prefix = 0;  /* no more prefixes */
            break;
        }

        if (!prefix)
            break;
    }
    prefixlen = i;

    /* Now look at the actual instruction */

    if (len - i < 3)
        return 0;

    switch(instr[i++])
    {
    case 0x0f: /* extended instruction */
        switch(instr[i++])
        {
        case 0x00: /* sldt/str */
        {
            int reg = (instr[i] >> 3) & 7;
            switch (reg)
            {
            case 0: /* sldt */
            case 1: /* str */
            {
                int instr_len;
                int mod = instr[i] >> 6;
                void *data;
                UINT16 dummy_value;

                if (!INSTR_GetOperandAddr( context, &instr[i], len - i + 1, long_addr,
                                           segprefix, &instr_len, &data ))
                    return 0;

                if (reg == 0)
                {
                    /* sldt */
                    dummy_value = UMIP_DUMMY_LDT;
                    TRACE( "sldt at 0x%08x\n", context->Eip );
                }
                else
                {
                    /* str */
                    dummy_value = UMIP_DUMMY_TR;
                    TRACE( "str at 0x%08x\n", context->Eip );
                }

                if (mod == 3)
                {
                    /* Destination operand is a register.
                     * Zero-extend the dummy value and store to the register. */
                    UINT32 dummy_value_32 = dummy_value;
                    memcpy( data, &dummy_value_32, long_op ? 4 : 2 );
                }
                else
                {
                    /* Destination operand is a memory location.
                     * Only copy 16 bits regardless of operand size. */
                    if (virtual_uninterrupted_write_memory( data, &dummy_value, sizeof(dummy_value) ))
                    {
                        TRACE( "memory write by 0x%08x to %p failed\n", context->Eip, data );
                        *err_addr = data;
                        return 2;
                    }
                }
                context->Eip += prefixlen + instr_len + 2;
                return 1;
            }
            }
        }
        case 0x01: /* sgdt/sidt/smsw */
        {
            /* The Linux kernel already emulates these instructions for 32-bit processes,
             * Wine should never get an exception for them.
             * However, it may be necessary in the future to emulate these instructions
             * for other OSes if they don't do any emulation.
             */
            break;
        }
        }
        break;
    }
    return 0;  /* Unable to emulate it */
}

#elif defined(__x86_64__) && defined(linux)     /* __i386__ && linux */

#define REX_B   1
#define REX_X   2
#define REX_R   4
#define REX_W   8

#define REGMODRM_MOD( regmodrm, rex )   ((regmodrm) >> 6)
#define REGMODRM_REG( regmodrm, rex )   (((regmodrm) >> 3) & 7) | (((rex) & REX_R) ? 8 : 0)
#define REGMODRM_RM( regmodrm, rex )    (((regmodrm) & 7) | (((rex) & REX_B) ? 8 : 0))

#define SIB_SS( sib, rex )      ((sib) >> 6)
#define SIB_INDEX( sib, rex )   (((sib) >> 3) & 7) | (((rex) & REX_X) ? 8 : 0)
#define SIB_BASE( sib, rex )    (((sib) & 7) | (((rex) & REX_B) ? 8 : 0))

static inline DWORD64 *get_int_reg( CONTEXT *context, int index )
{
    return &context->Rax + index; /* index should be in range 0 .. 15 */
}

static inline int get_op_size( int long_op, int rex )
{
    if (rex & REX_W)
        return sizeof(DWORD64);
    else if (long_op)
        return sizeof(DWORD);
    else
        return sizeof(WORD);
}

/***********************************************************************
 *           INSTR_GetOperandAddr
 *
 * Return the address of an instruction operand (from the mod/rm byte).
 */
static int INSTR_GetOperandAddr( CONTEXT *context, BYTE *instr, unsigned int instr_len,
                                 int long_addr, int rex, int segprefix, int *len, BYTE **addr )
{
    int mod, rm, ss = 0, off, have_sib = 0;
    unsigned int i = 0;
    DWORD64 base = 0, index = 0;

#define GET_VAL( val, type ) \
    { if (sizeof(type) > (instr_len - i)) return 0; \
      *val = *(type *)&instr[i]; i += sizeof(type); *len += sizeof(type); }

    *len = 0;
    GET_VAL( &mod, BYTE );
    rm  = REGMODRM_RM( mod, rex );
    mod = REGMODRM_MOD( mod, rex );

    if (mod == 3)
    {
        *addr = (BYTE *)get_int_reg( context, rm );
        return 1;
    }

    if ((rm & 7) == 4)
    {
        BYTE sib;
        int id;

        GET_VAL( &sib, BYTE );
        rm = SIB_BASE( sib, rex );
        id = SIB_INDEX( sib, rex );
        ss = SIB_SS( sib, rex );

        index = (id != 4) ? *get_int_reg( context, id ) : 0;
        if (!long_addr) index &= 0xffffffff;
        have_sib = 1;
    }

    base = *get_int_reg( context, rm );
    if (!long_addr) base &= 0xffffffff;

    switch (mod)
    {
    case 0:
        if (rm == 5)  /* special case */
        {
            base = have_sib ? 0 : context->Rip;
            if (!long_addr) base &= 0xffffffff;
            GET_VAL( &off, DWORD );
            base += (signed long)off;
        }
        break;

    case 1:  /* 8-bit disp */
        GET_VAL( &off, BYTE );
        base += (signed char)off;
        break;

    case 2:  /* 32-bit disp */
        GET_VAL( &off, DWORD );
        base += (signed long)off;
        break;
    }

    /* FIXME: we assume that all segments have a base of 0 */
    *addr = (BYTE *)(base + (index << ss));
    return 1;
#undef GET_VAL
}

/***********************************************************************
 *           emulate_instruction
 *
 * Emulate a UMIP-protected instruction.
 * Returns: 0 if no instruction emulated, 1 if instruction emulated successfully,
 *          2 if instruction's write to memory failed.
 */
static int emulate_instruction( CONTEXT *context, void **err_addr )
{
    int prefix, segprefix, prefixlen, long_op, long_addr, rex;
    unsigned int i, len;
    BYTE instr[16];

    long_op = long_addr = 1;
    len = virtual_uninterrupted_read_memory( (BYTE *)context->Rip, instr, sizeof(instr) );

    /* First handle any possible prefix */

    segprefix = -1;  /* no seg prefix */
    rex = 0;  /* no rex prefix */
    prefix = 1;
    for (i = 0; i < len; i++)
    {
        switch(instr[i])
        {
        case 0x2e:
            segprefix = context->SegCs;
            break;
        case 0x36:
            segprefix = context->SegSs;
            break;
        case 0x3e:
            segprefix = context->SegDs;
            break;
        case 0x26:
            segprefix = context->SegEs;
            break;
        case 0x64:
            segprefix = context->SegFs;
            break;
        case 0x65:
            segprefix = context->SegGs;
            break;
        case 0x66:
            long_op = !long_op;  /* opcode size prefix */
            break;
        case 0x67:
            long_addr = !long_addr;  /* addr size prefix */
            break;
        case 0x40:  /* rex */
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4a:
        case 0x4b:
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            rex = instr[i];
            break;
        case 0xf0:  /* lock */
            break;
        case 0xf2:  /* repne */
            break;
        case 0xf3:  /* repe */
            break;
        default:
            prefix = 0;  /* no more prefixes */
            break;
        }

        if (!prefix)
            break;
    }
    prefixlen = i;

    /* Now look at the actual instruction */

    if (len - i < 3)
        return 0;

    switch(instr[i++])
    {
    case 0x0f: /* extended instruction */
        switch(instr[i++])
        {
        case 0x00: /* sldt/str */
        {
            int reg = REGMODRM_REG( instr[i], rex );
            switch (reg)
            {
            case 0: /* sldt */
            case 1: /* str */
            {
                int instr_len;
                int rm = REGMODRM_RM( instr[i], rex );
                BYTE *data;
                UINT16 dummy_value;

                if (!INSTR_GetOperandAddr( context, &instr[i], len - i + 1, long_addr,
                                           rex, segprefix, &instr_len, &data ))
                    return 0;

                if (reg == 0)
                {
                    /* sldt */
                    dummy_value = UMIP_DUMMY_LDT;
                    TRACE( "sldt at 0x%lx\n", context->Rip );
                }
                else
                {
                    /* str */
                    dummy_value = UMIP_DUMMY_TR;
                    TRACE( "str at 0x%lx\n", context->Rip );
                }

                if (REGMODRM_MOD( instr[i], rex ) == 3)
                {
                    /* Destination operand is a register.
                     * Zero-extend the dummy LDT and store to the register. */
                    UINT64 dummy_value_64 = dummy_value;
                    memcpy( data, &dummy_value_64, get_op_size( long_op, rex ) );
                }
                else
                {
                    /* Destination operand is a memory location.
                     * Only copy 16 bits regardless of operand size. */
                    if (virtual_uninterrupted_write_memory( data, &dummy_value, sizeof(dummy_value) ))
                    {
                        TRACE( "memory write by 0x%lx to %p failed\n", context->Rip, data );
                        *err_addr = data;
                        return 2;
                    }
                }
                context->Rip += prefixlen + instr_len + 2;
                return 1;
            }
            default: break;
            }
        }
        case 0x01: /* sgdt/sidt/smsw */
        {
            int instr_len;
            int reg = REGMODRM_REG( instr[i], rex );
            switch (reg)
            {
            case 0: /* sgdt */
            case 1: /* sidt */
            {
                BYTE *data;
                UINT16 dummy_limit = UMIP_DUMMY_GDT_IDT_LIMIT;
                UINT64 dummy_base_addr;

                /* sgdt/sidt cannot use a register as the destination operand */
                if (REGMODRM_MOD( instr[i], rex ) == 3)
                    return 0;

                if (!INSTR_GetOperandAddr( context, &instr[i], len - i + 1, long_addr,
                                           rex, segprefix, &instr_len, &data ))
                    return 0;

                if (reg == 0)
                {
                    /* sgdt */
                    dummy_base_addr = UMIP_DUMMY_GDT_BASE;
                    TRACE( "sgdt at %lx\n", context->Rip );
                }
                else if (reg == 1)
                {
                    /* sidt */
                    dummy_base_addr = UMIP_DUMMY_IDT_BASE;
                    TRACE( "sidt at %lx\n", context->Rip );
                }
                if (virtual_uninterrupted_write_memory( data, &dummy_limit, UMIP_GDT_IDT_LIMIT_SIZE ) ||
                    virtual_uninterrupted_write_memory( data + UMIP_GDT_IDT_LIMIT_SIZE, &dummy_base_addr, UMIP_GDT_IDT_BASE_SIZE_64BIT ))
                {
                    TRACE( "memory write by 0x%lx to %p failed\n", context->Rip, data );
                    *err_addr = data;
                    return 2;
                }

                context->Rip += prefixlen + instr_len + 2;
                return 1;
            }
            case 4: /* smsw */
            {
                BYTE *data;
                int rm = REGMODRM_RM( instr[i], rex );

                if (!INSTR_GetOperandAddr( context, &instr[i], len - i + 1, long_addr,
                                           rex, segprefix, &instr_len, &data ))
                    return 0;

                if (REGMODRM_MOD( instr[i], rex ) == 3)
                {
                    /* Destination operand is a register.
                     * Zero-extend the dummy MSW and store to the register. */
                    UINT64 dummy_msw = UMIP_DUMMY_MSW;
                    TRACE( "smsw at %lx\n", context->Rip );
                    memcpy( data, &dummy_msw, get_op_size( long_op, rex ) );
                }
                else
                {
                    /* Destination operand is a memory location.
                     * Only copy 16 bits regardless of operand size. */
                    UINT16 dummy_msw = UMIP_DUMMY_MSW;
                    TRACE( "smsw at %lx\n", context->Rip );
                    if (virtual_uninterrupted_write_memory( data, &dummy_msw, sizeof(dummy_msw) ))
                    {
                        TRACE( "memory write by 0x%lx to %p failed\n", context->Rip, data );
                        *err_addr = data;
                        return 2;
                    }
                }
                context->Rip += prefixlen + instr_len + 2;
                return 1;
            }
            default: break;
            }
        }
        }
        break;  /* Unable to emulate it */
    }
    return 0;
}

#endif      /* __x86_64 && linux */

#if (defined(__i386__) || defined(__x86_64__)) && defined(linux)
extern int have_cpuid (void );

extern void do_cpuid_cx( unsigned int ax, unsigned int cx, unsigned int *p );
#ifdef __i386__
__ASM_GLOBAL_FUNC( do_cpuid_cx,
                   "pushl %esi\n\t"
                   "pushl %ebx\n\t"
                   "movl 12(%esp),%eax\n\t"
                   "movl 16(%esp),%ecx\n\t"
                   "movl 20(%esp),%esi\n\t"
                   "cpuid\n\t"
                   "movl %eax,(%esi)\n\t"
                   "movl %ebx,4(%esi)\n\t"
                   "movl %ecx,8(%esi)\n\t"
                   "movl %edx,12(%esi)\n\t"
                   "popl %ebx\n\t"
                   "popl %esi\n\t"
                   "ret" )
#else
__ASM_GLOBAL_FUNC( do_cpuid_cx,
                   "pushq %rbx\n\t"
                   "movq %rdx,%r8\n\t"
                   "movl %edi,%eax\n\t"
                   "movl %esi,%ecx\n\t"
                   "cpuid\n\t"
                   "movl %eax,(%r8)\n\t"
                   "movl %ebx,4(%r8)\n\t"
                   "movl %ecx,8(%r8)\n\t"
                   "movl %edx,12(%r8)\n\t"
                   "popq %rbx\n\t"
                   "ret" )
#endif

static int umip_enabled( void )
{
    /* Check cpuid to see if UMIP is supported.
     * UMIP bit is EAX=0x07,ECX=0x0, ECX bit 2
     * (CPUID.07H.0H:ECX:UMIP[bit 2] in Intel syntax)
     *
     * It would be preferable to check if UMIP is actually enabled
     * (CR4.UMIP), but that can't be done from userspace.
     */
    unsigned int regs[4];

#ifdef __i386__
    if (!have_cpuid()) return 0;
#endif

    do_cpuid_cx( 0x00000000, 0, regs );  /* get standard cpuid level and vendor name */
    if (regs[0] >= 0x00000007)   /* Check for supported cpuid version */
    {
        do_cpuid_cx( 0x00000007, 0, regs );
        if (regs[2] & (1 << 2))
            return 1;
    }

    return 0;
}
#endif      /* (__i386__ || __x86_64__) && linux */

int umip_emulate_instruction( CONTEXT *context, void **err_addr )
{
#if (defined(__i386__) || defined(__x86_64__)) && defined(linux)
    static int umip_enabled_cache = -1;
    if (umip_enabled_cache == -1) umip_enabled_cache = umip_enabled();

    if (umip_enabled_cache)
        return emulate_instruction( context, err_addr );
    else
        return 0;
#else
    return 0;
#endif
}
