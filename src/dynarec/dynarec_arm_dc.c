#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynablock.h"
#include "dynablock_private.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "arm_printer.h"

#include "dynarec_arm_helper.h"


uintptr_t dynarecDC(dynarec_arm_t* dyn, uintptr_t addr, uintptr_t ip, int ninst, int* ok, int* need_epilog)
{
    uint8_t nextop = F8;
    uint8_t u8;
    uint32_t u32;
    int32_t i32;
    int16_t i16;
    uint16_t u16;
    uint8_t gd, ed;
    uint8_t wback, wb1, wb2;
    int v1, v2, v3;
    int s0, s1;
    int d0, d1;
    int fixedaddress;
    switch(nextop) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
        case 0xC3:
        case 0xC4:
        case 0xC5:
        case 0xC6:
        case 0xC7:
            INST_NAME("FADD STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VADD_F64(v1, v1, v2);
            break;
        case 0xC8:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCC:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            INST_NAME("FMUL STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VMUL_F64(v1, v1, v2);
            break;
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xD4:
        case 0xD5:
        case 0xD6:
        case 0xD7:
            INST_NAME("FCOM ST0, STx"); //yep
            v1 = x87_get_st(dyn, ninst, x1, x2, 0);
            v2 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VCMP_F64(v1, v2);
            FCOM(x1, x2);
            break;
        case 0xD8:
        case 0xD9:
        case 0xDA:
        case 0xDB:
        case 0xDC:
        case 0xDD:
        case 0xDE:
        case 0xDF:
            INST_NAME("FCOMP ST0, STx");
            v1 = x87_get_st(dyn, ninst, x1, x2, 0);
            v2 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VCMP_F64(v1, v2);
            FCOM(x1, x2);
            x87_do_pop(dyn, ninst);
            break;
        case 0xE0:
        case 0xE1:
        case 0xE2:
        case 0xE3:
        case 0xE4:
        case 0xE5:
        case 0xE6:
        case 0xE7:
            INST_NAME("FSUBR STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VSUB_F64(v1, v2, v1);
            break;
        case 0xE8:
        case 0xE9:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF:
            INST_NAME("FSUB STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VSUB_F64(v1, v1, v2);
            break;
        case 0xF0:
        case 0xF1:
        case 0xF2:
        case 0xF3:
        case 0xF4:
        case 0xF5:
        case 0xF6:
        case 0xF7:
            INST_NAME("FDIVR STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VDIV_F64(v1, v2, v1);
            break;       
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xFF:
            INST_NAME("FDIV STx, ST0");
            v2 = x87_get_st(dyn, ninst, x1, x2, 0);
            v1 = x87_get_st(dyn, ninst, x1, x2, nextop&7);
            VDIV_F64(v1, v1, v2);
            break;
        default:
            switch((nextop>>3)&7) {
                case 0:
                    INST_NAME("FADD ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VADD_F64(v1, v1, d1);
                    break;
                case 1:
                    INST_NAME("FMUL ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VMUL_F64(v1, v1, d1);
                    break;
                case 2:
                    INST_NAME("FCOM ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VCMP_F64(v1, d1);
                    FCOM(x1, x2);
                    break;
                case 3:
                    INST_NAME("FCOMP ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VCMP_F64(v1, d1);
                    FCOM(x1, x2);
                    x87_do_pop(dyn, ninst);
                    break;
                case 4:
                    INST_NAME("FSUB ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VSUB_F64(v1, v1, d1);
                    break;
                case 5:
                    INST_NAME("FSUBR ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VSUB_F64(v1, d1, v1);
                    break;
                case 6:
                    INST_NAME("FDIV ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VDIV_F64(v1, v1, d1);
                    break;
                case 7:
                    INST_NAME("FDIVR ST0, double[ED]");
                    v1 = x87_get_st(dyn, ninst, x1, x2, 0);
                    addr = geted(dyn, addr, ninst, nextop, &wback, x3, &fixedaddress);
                    d1 = x87_get_scratch_double(0);
                    VLDR_64(d1, wback, 0);
                    VDIV_F64(v1, d1, v1);
                    break;
            }
    }
    return addr;
}

