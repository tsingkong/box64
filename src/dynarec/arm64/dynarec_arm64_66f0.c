#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>

#include "debug.h"
#include "box64context.h"
#include "box64cpu.h"
#include "emu/x64emu_private.h"
#include "emu/x64run_private.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "emu/x64run_private.h"
#include "x64trace.h"
#include "dynarec_native.h"

#include "arm64_printer.h"
#include "dynarec_arm64_private.h"
#include "../dynarec_helper.h"
#include "dynarec_arm64_functions.h"


uintptr_t dynarec64_66F0(dynarec_arm_t* dyn, uintptr_t addr, uintptr_t ip, int ninst, rex_t rex, int rep, int* ok, int* need_epilog)
{
    (void)ip; (void)rep; (void)need_epilog;

    uint8_t opcode = F8;
    uint8_t nextop;
    uint8_t gd, ed, u8;
    uint8_t wback, wb1, wb2, gb1, gb2;
    int16_t i16;
    int64_t i64, j64;
    int64_t fixedaddress;
    int unscaled;
    MAYUSE(gb1);
    MAYUSE(gb2);
    MAYUSE(wb1);
    MAYUSE(wb2);
    MAYUSE(j64);

    while((opcode==0xF2) || (opcode==0xF3)) {
        rep = opcode-0xF1;
        opcode = F8;
    }

    GETREX();

    switch(opcode) {

        case 0x01:
            INST_NAME("LOCK ADD Ew, Gw");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGW(x5);
            if(MODREG) {
                ed = TO_NAT((nextop & 7) + (rex.b << 3));
                UXTHw(x6, ed);
                emit_add16(dyn, ninst, x6, x5, x3, x4);
                BFIx(ed, x6, 0, 16);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                if(cpuext.atomics) {
                    UFLAG_IF {
                        LDADDALH(x5, x1, wback);
                        emit_add16(dyn, ninst, x1, x5, x3, x4);
                    } else {
                        STADDLH(x5, wback);
                    }
                } else {
                    MARKLOCK;
                    LDAXRH(x1, wback);
                    emit_add16(dyn, ninst, x1, x5, x3, x4);
                    STLXRH(x3, x1, wback);
                    CBNZx_MARKLOCK(x3);
                }
                SMDMB();
            }
            break;

        case 0x09:
            INST_NAME("LOCK OR Ew, Gw");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGW(x5);
            if(MODREG) {
                ed = TO_NAT((nextop & 7) + (rex.b << 3));
                UXTHw(x6, ed);
                emit_or16(dyn, ninst, x6, x5, x3, x4);
                BFIx(ed, x6, 0, 16);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                if(cpuext.atomics) {
                    UFLAG_IF {
                        LDSETALH(x5, x1, wback);
                        emit_or16(dyn, ninst, x1, x5, x3, x4);
                    } else {
                        STSETLH(x5, wback);
                    }
                } else {
                    MARKLOCK;
                    LDAXRH(x1, wback);
                    emit_or16(dyn, ninst, x1, x5, x3, x4);
                    STLXRH(x3, x1, wback);
                    CBNZx_MARKLOCK(x3);
                }
                SMDMB();
            }
            break;

        case 0x0F:
            opcode = F8;
            switch(opcode) {

                case 0xB1:
                    INST_NAME("LOCK CMPXCHG Ew, Gw");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    nextop = F8;
                    GETGD;
                    UXTHw(x6, xRAX);
                    if(MODREG) {
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        wback = 0;
                        UXTHw(x1, ed);
                        CMPSxw_REG(x6, x1);
                        B_MARK(cNE);
                        BFIx(ed, gd, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                        if(!ALIGNED_ATOMICH) {
                            if(cpuext.uscat) {
                                ANDx_mask(x1, wback, 1, 0, 3);  // mask = F
                                CMPSw_U12(x1, 0xF);
                                B_MARK3(cGE);
                            } else {
                                TSTx_mask(wback, 1, 0, 0);    // mask=1
                                B_MARK3(cNE);
                            }
                        }
                        // Aligned version
                        if(cpuext.atomics) {
                            MOVw_REG(x1, x6);
                            CASALH(x1, gd, wback);
                        } else {
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            CMPSw_REG(x6, x1);
                            B_MARK(cNE);
                            // EAX == Ed
                            STLXRH(x4, gd, wback);
                            CBNZx_MARKLOCK(x4);
                            // done
                        }
                        if(!ALIGNED_ATOMICH) {
                            B_MARK_nocond;
                            // Unaligned version
                            MARK3;
                            LDRH_U12(x1, wback, 0);
                            LDAXRB(x3, wback); // dummy read, to arm the write...
                            SUBw_UXTB(x3, x3, x1);
                            CBNZw_MARK3(x3);
                            CMPSw_REG(x6, x1);
                            B_MARK(cNE);
                            // EAX == Ed
                            STLXRB(x4, gd, wback);
                            CBNZx_MARK3(x4);
                            STRH_U12(gd, wback, 0);
                        }
                    }
                    MARK;
                    SMDMB();
                    // Common part (and fallback for EAX != Ed)
                    UFLAG_IF {emit_cmp16(dyn, ninst, x6, x1, x3, x4, x5);}
                    BFIx(xRAX, x1, 0, 16);
                    break;

                case 0xC1:
                    INST_NAME("LOCK XADD Gw, Ew");
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    nextop = F8;
                    gd = TO_NAT(((nextop & 0x38) >> 3) + (rex.r << 3));
                    UXTHx(x5, gd);
                    if(MODREG) {
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        BFIx(gd, ed, 0, 16);
                        emit_add16(dyn, ninst, x5, gd, x3, x4);
                        BFIx(ed, x5, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                        if(cpuext.atomics) {
                            LDADDALH(x5, x1, wback);
                        } else {
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            ADDxw_REG(x4, x1, x5);
                            STLXRH(x3, x4, wback);
                            CBNZx_MARKLOCK(x3);
                        }
                        SMDMB();
                        IFX(X_ALL|X_PEND) {
                            MOVxw_REG(x2, x1);
                            emit_add16(dyn, ninst, x2, x5, x3, x4);
                        }
                        BFIx(gd, x1, 0, 16);
                    }
                    break;

                default:
                    DEFAULT;
            }
            break;

        case 0x11:
            INST_NAME("LOCK ADC Ew, Gw");
            READFLAGS(X_CF);
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGW(x5);
            if(MODREG) {
                ed = TO_NAT((nextop & 7) + (rex.b << 3));
                UXTHw(x6, ed);
                emit_adc16(dyn, ninst, x6, x5, x3, x4);
                BFIx(ed, x6, 0, 16);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                MARKLOCK;
                LDAXRH(x1, wback);
                emit_adc16(dyn, ninst, x1, x5, x3, x4);
                STLXRH(x3, x1, wback);
                CBNZx_MARKLOCK(x3);
                SMDMB();
            }
            break;

        case 0x21:
            INST_NAME("LOCK AND Ew, Gw");
            SETFLAGS(X_ALL, SF_SET_PENDING);
            nextop = F8;
            GETGW(x5);
            if(MODREG) {
                ed = TO_NAT((nextop & 7) + (rex.b << 3));
                UXTHw(x6, ed);
                emit_and16(dyn, ninst, x6, gd, x3, x4);
                BFIx(ed, x6, 0, 16);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                if(cpuext.atomics) {
                    MVNw_REG(x3, gd);
                    UFLAG_IF {
                        LDCLRALH(x3, x1, wback);
                        emit_and16(dyn, ninst, x1, gd, x3, x4);
                    } else {
                        STCLRLH(x3, wback);
                    }
                } else {
                    MARKLOCK;
                    LDAXRH(x1, wback);
                    emit_and16(dyn, ninst, x1, gd, x3, x4);
                    STLXRH(x3, x1, wback);
                    CBNZx_MARKLOCK(x3);
                }
                SMDMB();
            }
            break;

        case 0x81:
        case 0x83:
            nextop = F8;
            switch((nextop>>3)&7) {
                case 0: //ADD
                    if(opcode==0x81) {
                        INST_NAME("LOCK ADD Ew, Iw");
                    } else {
                        INST_NAME("LOCK ADD Ew, Ib");
                    }
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        MOV32w(x5, i16);
                        UXTHw(x6, ed);
                        emit_add16(dyn, ninst, x6, x5, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        MOV32w(x5, i16);
                        if(!ALIGNED_ATOMICH) {
                            if(cpuext.uscat) {
                                ANDx_mask(x1, wback, 1, 0, 3);  // mask = F
                                CMPSw_U12(x1, 0xF);
                                B_MARK(cGE);
                            } else {
                                TSTx_mask(wback, 1, 0, 0);    // mask=1
                                B_MARK(cNE);
                            }
                        }
                        if(cpuext.atomics) {
                            UFLAG_IF {
                                LDADDALH(x5, x1, wback);
                            } else {
                                STADDLH(x5, wback);
                            }
                        } else {
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            ADDw_REG(x4, x1, x5);
                            STLXRH(x3, x4, wback);
                            CBNZx_MARKLOCK(x3);
                        }
                        SMDMB();
                        if(!ALIGNED_ATOMICH) {
                            B_MARK2_nocond;
                            MARK;   // unaligned! also, not enough
                            LDRH_U12(x1, wback, 0);
                            LDAXRB(x4, wback);
                            SUBw_UXTB(x4, x4, x1);
                            CBNZw_MARK(x4);
                            ADDw_REG(x4, x1, x5);
                            STLXRB(x3, x4, wback);
                            CBNZx_MARK(x3);
                            STRH_U12(x4, wback, 0);    // put the whole value
                            SMDMB();
                        }
                        MARK2;
                        UFLAG_IF {
                            emit_add16(dyn, ninst, x1, x5, x3, x4);
                        }
                    }
                    break;
                case 1: //OR
                    if(opcode==0x81) {INST_NAME("LOCK OR Ew, Iw");} else {INST_NAME("LOCK OR Ew, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        UXTHw(x6, ed);
                        emit_or16c(dyn, ninst, x6, i16, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        if(!i64) {MOV32w(x5, i16);}
                        if(cpuext.atomics) {
                            UFLAG_IF {
                                LDSETALH(x5, x1, wback);
                                emit_or16c(dyn, ninst, x1, i16, x3, x4);
                            } else {
                                STSETLH(x5, wback);
                            }
                        } else {
                            i64 = convert_bitmask_w(i16);
                            if(!i64) {MOV32w(x5, i16);}
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            if(i64) {
                                emit_or16c(dyn, ninst, x1, i16, x3, x4);
                            } else {
                                emit_or16(dyn, ninst, x1, x5, x3, x4);
                            }
                            STLXRH(x3, x1, wback);
                            CBNZx_MARKLOCK(x3);
                        }
                        SMDMB();
                    }
                    break;
                case 2: //ADC
                    if(opcode==0x81) {INST_NAME("LOCK ADC Ew, Iw");} else {INST_NAME("LOCK ADC Ew, Ib");}
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        MOV32w(x5, i16);
                        UXTHw(x6, ed);
                        emit_adc16(dyn, ninst, x6, x5, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        MOV32w(x5, i16);
                        MARKLOCK;
                        LDAXRH(x1, wback);
                        emit_adc16(dyn, ninst, x1, x5, x3, x4);
                        STLXRH(x3, x1, wback);
                        CBNZx_MARKLOCK(x3);
                        SMDMB();
                    }
                    break;
                case 3: //SBB
                    if(opcode==0x81) {INST_NAME("LOCK SBB Ew, Iw");} else {INST_NAME("LOCK SBB Ew, Ib");}
                    READFLAGS(X_CF);
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        MOV32w(x5, i16);
                        UXTHw(x6, ed);
                        emit_sbb16(dyn, ninst, x6, x5, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        MOV32w(x5, i16);
                        MARKLOCK;
                        LDAXRH(x1, wback);
                        emit_sbb16(dyn, ninst, x1, x5, x3, x4);
                        STLXRH(x3, x1, wback);
                        CBNZx_MARKLOCK(x3);
                        SMDMB();
                    }
                    break;
                case 4: //AND
                    if(opcode==0x81) {INST_NAME("LOCK AND Ew, Iw");} else {INST_NAME("LOCK AND Ew, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        UXTHw(x6, ed);
                        emit_and16c(dyn, ninst, x6, i16, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        i64 = convert_bitmask_w(i16);
                        if(cpuext.atomics) {
                            MOV32w(x5, ~i16);
                            UFLAG_IF {
                                LDCLRALH(x5, x1, wback);
                                if(i64) {
                                    emit_and16c(dyn, ninst, x1, i16, x3, x4);
                                } else {
                                    MVNw_REG(x5, x5);
                                    emit_and16(dyn, ninst, x1, x5, x3, x4);
                                }
                            } else {
                                STCLRLH(x5, wback);
                            }
                        } else {
                            if(!i64) {MOV32w(x5, i16);}
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            if(i64) {
                                emit_and16c(dyn, ninst, x1, i16, x3, x4);
                            } else {
                                emit_and16(dyn, ninst, x1, x5, x3, x4);
                            }
                            STLXRH(x3, x1, wback);
                            CBNZx_MARKLOCK(x3);
                            SMDMB();
                        }
                    }
                    break;
                case 5: //SUB
                    if(opcode==0x81) {INST_NAME("LOCK SUB Ew, Iw");} else {INST_NAME("LOCK SUB Ew, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        MOV32w(x5, i16);
                        UXTHw(x6, ed);
                        emit_sub16(dyn, ninst, x6, x5, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        MOV32w(x5, i16);
                        if(!ALIGNED_ATOMICH) {
                            if(cpuext.uscat) {
                                ANDx_mask(x1, wback, 1, 0, 3);  // mask = F
                                CMPSw_U12(x1, 0xF);
                                B_MARK(cGE);
                            } else {
                                TSTx_mask(wback, 1, 0, 0);    // mask=1
                                B_MARK(cNE);
                            }
                        }
                        if(cpuext.atomics) {
                            NEGw_REG(x4, x5);
                            UFLAG_IF {
                                LDADDALH(x4, x1, wback);
                            } else {
                                STADDLH(x4, wback);
                            }
                        } else {
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            SUBw_REG(x4, x1, x5);
                            STLXRH(x3, x1, wback);
                            CBNZx_MARKLOCK(x3);
                        }
                        SMDMB();
                        if(!ALIGNED_ATOMICH) {
                            B_MARK2_nocond;
                            MARK;   // unaligned! also, not enough
                            LDRH_U12(x1, wback, 0);
                            LDAXRB(x4, wback);
                            SUBw_UXTB(x4, x4, x1);
                            CBNZw_MARK(x4);
                            SUBw_REG(x4, x1, x5);
                            STLXRB(x3, x4, wback);
                            CBNZx_MARK(x3);
                            STRH_U12(x4, wback, 0);    // put the whole value
                            SMDMB();
                        }
                        MARK2;
                        UFLAG_IF {
                            emit_sub16(dyn, ninst, x1, x5, x3, x4);
                        }
                    }
                    break;
                case 6: //XOR
                    if(opcode==0x81) {INST_NAME("LOCK XOR Ew, Iw");} else {INST_NAME("LOCK XOR Ew, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    if(MODREG) {
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        ed = TO_NAT((nextop & 7) + (rex.b << 3));
                        UXTHw(x6, ed);
                        emit_xor16c(dyn, ninst, x6, i16, x3, x4);
                        BFIx(ed, x6, 0, 16);
                    } else {
                        addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, (opcode==0x81)?2:1);
                        if(opcode==0x81) i16 = F16S; else i16 = F8S;
                        i64 = convert_bitmask_w(i16);
                        if(cpuext.atomics) {
                            UFLAG_IF {
                                LDEORALH(x5, x1, wback);
                                emit_xor16c(dyn, ninst, x1, i16, x3, x4);
                            } else {
                                STEORLH(x5, wback);
                            }
                        } else {
                            if(!i64) {MOV32w(x5, i16);}
                            MARKLOCK;
                            LDAXRH(x1, wback);
                            if(i64)
                                emit_xor16c(dyn, ninst, x1, i16, x3, x4);
                            else
                                emit_xor16(dyn, ninst, x1, x5, x3, x4);
                            STLXRH(x3, x1, wback);
                            CBNZx_MARKLOCK(x3);
                            SMDMB();
                        }
                    }
                    break;
                case 7: //CMP
                    if(opcode==0x81) {INST_NAME("(LOCK) CMP Ew, Iw");} else {INST_NAME("(LOCK) CMP Ew, Ib");}
                    SETFLAGS(X_ALL, SF_SET_PENDING);
                    GETEW(x6, (opcode==0x81)?2:1);
                    (void)wb1;
                    // No need to LOCK, this is readonly
                    if(opcode==0x81) i16 = F16S; else i16 = F8S;
                    if(i16) {
                        MOV32w(x5, i16);
                        emit_cmp16(dyn, ninst, x6, x5, x3, x4, x6);
                    } else {
                        emit_cmp16_0(dyn, ninst, ed, x3, x4);
                    }
                    break;
            }
            break;

            case 0xFF:
                nextop = F8;
                switch((nextop>>3)&7)
                {
                    case 0: // INC Ew
                        INST_NAME("LOCK INC Ew");
                        SETFLAGS(X_ALL&~X_CF, SF_SUBSET);
                        if(MODREG) {
                            ed = TO_NAT((nextop & 7) + (rex.b << 3));
                            UXTHw(x6, ed);
                            emit_inc16(dyn, ninst, x6, x5, x3);
                            BFIx(ed, x6, 0, 16);
                        } else {
                            addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                            if(cpuext.atomics) {
                                MOV32w(x3, 1);
                                UFLAG_IF {
                                    LDADDALH(x3, x1, wback);
                                    emit_inc16(dyn, ninst, x1, x3, x4);
                                } else {
                                    STADDLH(x3, wback);
                                }
                            } else {
                                MARKLOCK;
                                LDAXRH(x1, wback);
                                emit_inc16(dyn, ninst, x1, x3, x4);
                                STLXRH(x3, x1, wback);
                                CBNZx_MARKLOCK(x3);
                            }
                            SMDMB();
                        }
                        break;
                    case 1: //DEC Ew
                        INST_NAME("LOCK DEC Ew");
                        SETFLAGS(X_ALL&~X_CF, SF_SUBSET);
                        if(MODREG) {
                            ed = TO_NAT((nextop & 7) + (rex.b << 3));
                            UXTHw(x6, ed);
                            emit_dec16(dyn, ninst, x6, x5, x3);
                            BFIx(ed, x6, 0, 16);
                        } else {
                            addr = geted(dyn, addr, ninst, nextop, &wback, x2, &fixedaddress, NULL, 0, 0, rex, LOCK_LOCK, 0, 0);
                            if(cpuext.atomics) {
                                MOV32w(x3, -1);
                                UFLAG_IF {
                                    LDADDALH(x3, x1, wback);
                                    emit_dec16(dyn, ninst, x1, x3, x4);
                                } else {
                                    STADDLH(x3, wback);
                                }
                            } else {
                                MARKLOCK;
                                LDAXRH(x1, wback);
                                emit_dec16(dyn, ninst, x1, x3, x4);
                                STLXRH(x3, x1, wback);
                                CBNZx_MARKLOCK(x3);
                            }
                            SMDMB();
                        }
                        break;
                    default:
                        DEFAULT;
                }
                break;

        default:
            DEFAULT;
    }
    return addr;
}
