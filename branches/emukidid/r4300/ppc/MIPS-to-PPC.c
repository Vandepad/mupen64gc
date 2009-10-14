/* MIPS-to-PPC.c - convert MIPS code into PPC (take 2 1/2)
   by Mike Slegeir for Mupen64-GC
 ************************************************
   FIXME: Review all branch destinations
   TODO: Create FP register mapping and recompile those
         If possible (and not too complicated), it would be nice to leave out
           the in-place delay slot which is skipped if it is not branched to
           (That might be tricky as you don't know whether an outside branch
            or an interpreted branch might branch there)
         Optimize idle branches (generate a call to gen_interrupt)
 */

#include <string.h>
#include "MIPS-to-PPC.h"
#include "Register-Cache.h"
#include "Interpreter.h"
#include "Wrappers.h"

// Prototypes for functions used and defined in this file
static void genCallInterp(MIPS_instr);
#define JUMPTO_REG  0
#define JUMPTO_OFF  1
#define JUMPTO_ADDR 2
#define JUMPTO_REG_SIZE  2
#define JUMPTO_OFF_SIZE  3
#define JUMPTO_ADDR_SIZE 3
static void genJumpTo(unsigned int loc, unsigned int type);
static void genUpdateCount(void);
static int inline mips_is_jump(MIPS_instr);
void jump_to(unsigned int);

// Infinite loop-breaker code
static int interpretedLoop;

// Variable to indicate whether the next recompiled instruction
//   is a delay slot (which needs to have its registers flushed)
//   and the current instruction
static int delaySlotNext, isDelaySlot;
// This should be called before the jump is recompiled
static inline int check_delaySlot(void){
	interpretedLoop = 0; // Reset this variable for the next basic block
	if(peek_next_src() == 0){ // MIPS uses 0 as a NOP
		get_next_src();   // Get rid of the NOP
		return 0;
	} else {
		if(mips_is_jump(peek_next_src())) return CONVERT_WARNING;
		delaySlotNext = 1;
		convert(); // This just moves the delay slot instruction ahead of the branch
		return 1;
	}
}

#define MIPS_REG_HI 32
#define MIPS_REG_LO 33

// Initialize register mappings
void start_new_block(void){
	invalidateRegisters();
	interpretedLoop = 0;
	// Check if the previous instruction was a branch
	//   and thus whether this block begins with a delay slot
	unget_last_src();
	if(mips_is_jump(get_next_src())) delaySlotNext = 1;
}
void start_new_mapping(void){
	flushRegisters();
	// Clear the interpretedLoop flag for this new fragment
	interpretedLoop = 0;
}

static inline int signExtend(int value, int size){
	int signMask = 1 << (size-1);
	int negMask = 0xffffffff << (size-1);
	if(value & signMask) value |= negMask;
	return value;
}

typedef enum { NONE=0, EQ, NE, LT, GT, LE, GE } condition;
// Branch a certain offset (possibly conditionally, linking, or likely)
//   offset: N64 instructions from current N64 instruction to branch
//   cond: type of branch to execute depending on cr 7
//   link: if nonzero, branch and link
//   likely: if nonzero, the delay slot will only be executed when cond is true
static int branch(int offset, condition cond, int link, int likely){
	PowerPC_instr ppc;
	int likely_id;
	// Condition codes for bc (and their negations)
	int bo, bi, nbo;
	switch(cond){
		case EQ:
			bo = 0xc, nbo = 0x4, bi = 18;
			break;
		case NE:
			bo = 0x4, nbo = 0xc, bi = 18;
			break;
		case LT:
			bo = 0xc, nbo = 0x4, bi = 16;
			break;
		case GE:
			bo = 0x4, nbo = 0xc, bi = 16;
			break;
		case GT:
			bo = 0xc, nbo = 0x4, bi = 17;
			break;
		case LE:
			bo = 0x4, nbo = 0xc, bi = 17;
			break;
		default:
			bo = 0x14; nbo = 0x4; bi = 19;
			break;
	}
	
	flushRegisters();
	
	if(link){
		// Set LR to next instruction
		int lr = mapRegisterNew(MIPS_REG_LR);
		// lis	lr, pc@ha(0)
		GEN_LIS(ppc, lr, (get_src_pc()+8)>>16);
		set_next_dst(ppc);
		// la	lr, pc@l(lr)
		GEN_ORI(ppc, lr, lr, get_src_pc()+8);
		set_next_dst(ppc);
		
		flushRegisters();
	}
	
	if(likely){
		// b[!cond] <past delay to update_count>
		likely_id = add_jump_special(0);
		GEN_BC(ppc, likely_id, 0, 0, nbo, bi); 
		set_next_dst(ppc);
	}
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	if(likely) set_jump_special(likely_id, delaySlot+1);
	
	genUpdateCount(); // Sets cr2 to (next_interupt ? Count)
	
#ifndef INTERPRET_BRANCH
	// If we're jumping out, we need to trampoline using genJumpTo
	if(is_j_out(offset, 0)){
#endif // INTEPRET_BRANCH
		
		// b[!cond] <past jumpto & delay>
		//   Note: if there's a delay slot, I will branch to the branch over it
		GEN_BC(ppc, JUMPTO_OFF_SIZE+1, 0, 0, nbo, bi);
		set_next_dst(ppc);
		
		genJumpTo(offset, JUMPTO_OFF);
		
		// The branch isn't taken, but we need to check interrupts
		GEN_BGT(ppc, 2, 4, 0, 0);
		set_next_dst(ppc);
		// Load the address of the next instruction
		GEN_LIS(ppc, 3, (get_src_pc()+4)>>16);
		set_next_dst(ppc);
		GEN_ORI(ppc, 3, 3, get_src_pc()+4);
		set_next_dst(ppc);
		// If taking the interrupt, use the trampoline
#if 0
		// Branch to the jump pad
		GEN_B(ppc, add_jump(-2, 1, 1), 0, 0);
		set_next_dst(ppc);
#else
		GEN_BLR(ppc, 0);
		set_next_dst(ppc);
#endif
		
#ifndef INTERPRET_BRANCH		
	} else {
		// last_addr = naddr
		if(cond != NONE){
			GEN_BC(ppc, 4, 0, 0, bo, bi);
			set_next_dst(ppc);
			GEN_LIS(ppc, 3, (get_src_pc()+4)>>16);
			set_next_dst(ppc);
			GEN_ORI(ppc, 3, 3, get_src_pc()+4);
			set_next_dst(ppc);
			GEN_B(ppc, 3, 0, 0);
			set_next_dst(ppc);
		}
		GEN_LIS(ppc, 3, (get_src_pc() + (offset<<2))>>16);
		set_next_dst(ppc);
		GEN_ORI(ppc, 3, 3, get_src_pc() + (offset<<2));
		set_next_dst(ppc);
		GEN_STW(ppc, 3, 0, DYNAREG_LADDR);
		set_next_dst(ppc);
		
		// If not taking an interrupt, skip the branch to jump_pad
		GEN_BGT(ppc, 2, 2, 0, 0);
		set_next_dst(ppc);
		// If taking the interrupt, use the trampoline
#if 0
		// Branch to the jump pad
		GEN_B(ppc, add_jump(-2, 1, 1), 0, 0);
		set_next_dst(ppc);
#else
		GEN_BLR(ppc, 0);
		set_next_dst(ppc);
#endif
		
		// The actual branch
#if 0
		// FIXME: Reenable this when blocks are small enough to BC within
		//          Make sure that pass2 uses BD/LI as appropriate
		GEN_BC(ppc, add_jump(offset, 0, 0), 0, 0, bo, bi);
		set_next_dst(ppc);
#else
		GEN_BC(ppc, 2, 0, 0, nbo, bi);
		set_next_dst(ppc);
		GEN_B(ppc, add_jump(offset, 0, 0), 0, 0);
		set_next_dst(ppc);
#endif
		
	}
#endif // INTERPRET_BRANCH
	
	// Let's still recompile the delay slot in place in case its branched to
	// Unless the delay slot is in the next block, in which case there's nothing to skip
	//   Testing is_j_out with an offset of 0 checks whether the delay slot is out
	if(delaySlot && !is_j_out(0, 0)){
		// Step over the already executed delay slot if the branch isn't taken
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc); 
		
		unget_last_src();
		delaySlotNext = 1;
	} else nop_ignored();
	
#ifdef INTERPRET_BRANCH
	return INTERPRETED;
#else // INTERPRET_BRANCH
	return CONVERT_SUCCESS;
#endif
}


static int (*gen_ops[64])(MIPS_instr);

int convert(void){
	MIPS_instr mips = get_next_src();
	isDelaySlot = delaySlotNext; delaySlotNext = 0;
	int result = gen_ops[MIPS_GET_OPCODE(mips)](mips);
	/*if(isDelaySlot)*/ flushRegisters();
	return result;
}

static int NI(MIPS_instr mips){
	return CONVERT_ERROR;
}

// -- Primary Opcodes --

static int J(MIPS_instr mips){
	PowerPC_instr  ppc;
	unsigned int naddr = (MIPS_GET_LI(mips)<<2)|((get_src_pc()+4)&0xf0000000);
	
	if(naddr == get_src_pc()){
		// J_IDLE
		genCallInterp(mips);
		return INTERPRETED;
	}
	
	flushRegisters();
	reset_code_addr();
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	genUpdateCount(); // Sets cr2 to (next_interupt ? Count)
	
#ifdef INTERPRET_J
	genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
#else // INTERPRET_J
	// If we're jumping out, we can't just use a branch instruction
	if(is_j_out(MIPS_GET_LI(mips), 1)){
		genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
	} else {
		// last_addr = naddr
		GEN_LIS(ppc, 3, naddr>>16);
		set_next_dst(ppc);
		GEN_ORI(ppc, 3, 3, naddr);
		set_next_dst(ppc);
		GEN_STW(ppc, 3, 0, DYNAREG_LADDR);
		set_next_dst(ppc);
		
		// If we need to take an interrupt, don't branch in the block
		GEN_BLE(ppc, 2, 2, 0, 0);
		set_next_dst(ppc);
		
		// Even though this is an absolute branch
		//   in pass 2, we generate a relative branch
		GEN_B(ppc, add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
		set_next_dst(ppc);
		
		// If we're taking the interrupt, we have to use the trampoline
#if 0
		// Branch to the jump pad
		GEN_B(ppc, add_jump(naddr, 1, 1), 0, 0);
		set_next_dst(ppc);
#else
		GEN_BLR(ppc, 0);
		set_next_dst(ppc);
#endif
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); delaySlotNext = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_J
	return INTERPRETED;
#else // INTERPRET_J
	return CONVERT_SUCCESS;
#endif
}

static int JAL(MIPS_instr mips){
	PowerPC_instr  ppc;
	unsigned int naddr = (MIPS_GET_LI(mips)<<2)|((get_src_pc()+4)&0xf0000000);
	
	flushRegisters();
	reset_code_addr();
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	genUpdateCount(); // Sets cr2 to (next_interupt ? Count)
	
	// Set LR to next instruction
	int lr = mapRegisterNew(MIPS_REG_LR);
	// lis	lr, pc@ha(0)
	GEN_LIS(ppc, lr, (get_src_pc()+4)>>16);
	set_next_dst(ppc);
	// la	lr, pc@l(lr)
	GEN_ORI(ppc, lr, lr, get_src_pc()+4);
	set_next_dst(ppc);
	
	flushRegisters();
	
#ifdef INTERPRET_JAL
	genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
#else // INTERPRET_JAL
	// If we're jumping out, we can't just use a branch instruction
	if(is_j_out(MIPS_GET_LI(mips), 1)){
		genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
	} else {
		// last_addr = naddr
		GEN_LIS(ppc, 3, naddr>>16);
		set_next_dst(ppc);
		GEN_ORI(ppc, 3, 3, naddr);
		set_next_dst(ppc);
		GEN_STW(ppc, 3, 0, DYNAREG_LADDR);
		set_next_dst(ppc);
		
		// If we need to take an interrupt, don't branch in the block
		GEN_BLE(ppc, 2, 2, 0, 0);
		set_next_dst(ppc);
		
		// Even though this is an absolute branch
		//   in pass 2, we generate a relative branch
		// TODO: If I can figure out using the LR, use it!
		GEN_B(ppc, add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
		set_next_dst(ppc);
		
		// If we're taking the interrupt, we have to use the trampoline
#if 0
		// Branch to the jump pad
		GEN_B(ppc, add_jump(naddr, 1, 1), 0, 0);
		set_next_dst(ppc);
#else
		GEN_BLR(ppc, 0);
		set_next_dst(ppc);
#endif
	}
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		delaySlotNext = 1;
		// TODO
		// Step over the already executed delay slot if we ever
		//   actually use the real LR for JAL
		// b delaySlot+1
		//GEN_B(ppc, delaySlot+1, 0, 0);
		//set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_JAL
	return INTERPRETED;
#else // INTERPRET_JAL
	return CONVERT_SUCCESS;
#endif
}

static int BEQ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	if(MIPS_GET_IMMED(mips) == 0xffff &&
	   MIPS_GET_RA(mips) == MIPS_GET_RB(mips)){
		// BEQ_IDLE
		genCallInterp(mips);
		return INTERPRETED;
	}
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), EQ, 0, 0);
}

static int BNE(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), NE, 0, 0);
}

static int BLEZ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), LE, 0, 0);
}

static int BGTZ(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), GT, 0, 0);
}

static int ADDIU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ADDI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	return CONVERT_SUCCESS;
}

static int ADDI(MIPS_instr mips){
	return ADDIU(mips);
}

static int SLTI(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SLTI
	genCallInterp(mips);
	return INTERPRETED;
#else
	
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegisterNew( MIPS_GET_RT(mips) );
	int tmp = (rs == rt) ? mapRegisterTemp() : rt;
	
	// tmp = immed (sign extended)
	GEN_ADDI(ppc, tmp, 0, MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	// carry = rs < immed ? 0 : 1 (unsigned)
	GEN_SUBFC(ppc, 0, tmp, rs);
	set_next_dst(ppc);
	// rt = ~(rs ^ immed)
	GEN_EQV(ppc, rt, tmp, rs);
	set_next_dst(ppc);
	// rt = sign(rs) == sign(immed) ? 1 : 0
	GEN_SRWI(ppc, rt, rt, 31);
	set_next_dst(ppc);
	// rt += carry
	GEN_ADDZE(ppc, rt, rt);
	set_next_dst(ppc);
	// rt &= 1 ( = (sign(rs) == sign(immed)) xor (rs < immed (unsigned)) ) 
	GEN_RLWINM(ppc, rt, rt, 0, 31, 31);
	set_next_dst(ppc);
	
	if(rs == rt) unmapRegisterTemp(tmp);
	
	return CONVERT_SUCCESS;
#endif
}

static int SLTIU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SLTIU
	genCallInterp(mips);
	return INTERPRETED;
#else
	
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegisterNew( MIPS_GET_RT(mips) );
	
	// r0 = EXTS(immed)
	GEN_ADDI(ppc, 0, 0, MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	// carry = rs < immed ? 0 : 1
	GEN_SUBFC(ppc, rt, 0, rs);
	set_next_dst(ppc);
	// rt = carry - 1 ( = rs < immed ? -1 : 0 )
	GEN_SUBFE(ppc, rt, rt, rt);
	set_next_dst(ppc);
	// rt = !carry ( = rs < immed ? 1 : 0 )
	GEN_NEG(ppc, rt, rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int ANDI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ANDI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int ORI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ORI(ppc,
	        mapRegisterNew( MIPS_GET_RT(mips) ),
	        rs,
	        MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int XORI(MIPS_instr mips){
	PowerPC_instr ppc;
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_XORI(ppc,
	         mapRegisterNew( MIPS_GET_RT(mips) ),
	         rs,
	         MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int LUI(MIPS_instr mips){
	PowerPC_instr ppc;
	GEN_LIS(ppc,
	        mapRegisterNew( MIPS_GET_RT(mips) ),
	        MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int BEQL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), EQ, 0, 1);
}

static int BNEL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmp ra, rb
	GEN_CMP(ppc,
	        mapRegister(MIPS_GET_RA(mips)),
	        mapRegister(MIPS_GET_RB(mips)),
	        4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), NE, 0, 1);
}

static int BLEZL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), LE, 0, 1);
}

static int BGTZL(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16), GT, 0, 1);
}

static int DADDIU(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DADDIU)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DADDIU
	
	RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
	RegMapping rt = mapRegister64New( MIPS_GET_RT(mips) );
	
	GEN_ADDIC(ppc, rt.lo, rs.lo, MIPS_GET_IMMED(mips));
	set_next_dst(ppc);
	GEN_ADDZE(ppc, rt.hi, rs.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DADDI(MIPS_instr mips){
	// FIXME: Is there a difference?
	return DADDIU(mips);
}

static int LDL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LDL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LDL
	// TODO: ldl
	return CONVERT_ERROR;
#endif
}

static int LDR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LDR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LDR
	// TODO: ldr
	return CONVERT_ERROR;
#endif
}

static int LB(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LB
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LB
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegisterNew( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual load
		GEN_LBZ(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		// Sign extend the byte
		GEN_EXTSB(ppc, rd, rd);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int LH(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LH
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LH
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegisterNew( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual load
		GEN_LHA(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int LWL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWL
	// TODO: lwl
	return CONVERT_ERROR;
#endif
}

static int LW(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LW
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegisterNew( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual load
		GEN_LWZ(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int LBU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LBU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LBU
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegisterNew( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual load
		GEN_LBZ(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int LHU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LHU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LHU
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegisterNew( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual load
		GEN_LHZ(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int LWR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWR
	// TODO: lwr
	return CONVERT_ERROR;
#endif
}

static int LWU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWU
	// TODO: lwu
	return CONVERT_ERROR;
#endif
}

static int SB(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SB
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SB
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegister( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual save
		GEN_STB(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int SH(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SH
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SH
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegister( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual save
		GEN_STH(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int SWL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWL
	// TODO: swl
	return CONVERT_ERROR;
#endif
}

static int SW(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SW
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SW
	
	// Recompile accesses to stack pointer
	if(MIPS_GET_RS(mips) == MIPS_REG_SP){
		int base = mapRegister( MIPS_GET_RS(mips) );
		int rd   = mapRegister( MIPS_GET_RT(mips) );
		int addr = mapRegisterTemp();
		
		// If base >> 16 != 0xa400
		GEN_SRAWI(ppc, 0, base, 16);
		set_next_dst(ppc);
		GEN_CMPI(ppc, 0, 0xa400, 1);
		set_next_dst(ppc);
		GEN_BEQ(ppc, 1, 4, 0, 0);
		set_next_dst(ppc);
		
		// Use rdram
#ifdef USE_EXPANSION
		// Mask sp with 0x007FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 9, 31);
		set_next_dst(ppc);
#else
		// Mask sp with 0x003FFFFF
		GEN_RLWINM(ppc, addr, base, 0, 10, 31);
		set_next_dst(ppc);
#endif
		// Add rdram pointer
		GEN_ADD(ppc, addr, DYNAREG_RDRAM, addr);
		set_next_dst(ppc);
		// Skip over else
		GEN_B(ppc, 3, 0, 0);
		set_next_dst(ppc);
		
		// else use SP_DMEM
		// Mask sp with 0x00001FFF
		GEN_RLWINM(ppc, addr, base, 0, 19, 31);
		set_next_dst(ppc);
		// Add SP_DMEM pointer
		GEN_ADD(ppc, addr, DYNAREG_SPDMEM, addr);
		set_next_dst(ppc);
		
		// Perform the actual save
		GEN_STW(ppc, rd, MIPS_GET_IMMED(mips), addr);
		set_next_dst(ppc);
		
		unmapRegisterTemp(addr);
		
		return CONVERT_SUCCESS;
	} else {
		genCallInterp(mips);
		return INTERPRETED;
	}
#endif
}

static int SDL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDL
	// TODO: sdl
	return CONVERT_ERROR;
#endif
}

static int SDR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDR
	// TODO: sdr
	return CONVERT_ERROR;
#endif
}

static int SWR(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWR
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWR
	// TODO: swr
	return CONVERT_ERROR;
#endif
}

static int LD(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LD
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LD
	// TODO: ld
	return CONVERT_ERROR;
#endif
}

static int SD(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SD
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SD
	// TODO: sd
	return CONVERT_ERROR;
#endif
}

static int LWC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LWC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LWC1
	// TODO: lwc1
	return CONVERT_ERROR;
#endif
}

static int LDC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LDC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LDC1
	// TODO: ldc1
	return CONVERT_ERROR;
#endif
}

static int SWC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SWC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SWC1
	// TODO: swc1
	return CONVERT_ERROR;
#endif
}

static int SDC1(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SDC1
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SDC1
	// TODO: sdc1
	return CONVERT_ERROR;
#endif
}

static int CACHE(MIPS_instr mips){
	return CONVERT_ERROR;
}

static int LL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_LL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_LL
	// TODO: ll
	return CONVERT_ERROR;
#endif
}

static int SC(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SC
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SC
	// TODO: sc
	return CONVERT_ERROR;
#endif
}

// -- Special Functions --

static int SLL(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	GEN_SLWI(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         MIPS_GET_SA(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRL(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	GEN_SRWI(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         MIPS_GET_SA(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRA(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	GEN_SRAWI(ppc,
	          mapRegisterNew( MIPS_GET_RD(mips) ),
	          rt,
	          MIPS_GET_SA(mips));
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLLV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SLW(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rt,
	        rs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRLV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_RLWINM(ppc, 0, rs, 0, 27, 31); // Mask the lower 5-bits of rs
	set_next_dst(ppc);
	GEN_SRW(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rt,
	        0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SRAV(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SRAW(ppc,
	         mapRegisterNew( MIPS_GET_RD(mips) ),
	         rt,
	         rs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int JR(MIPS_instr mips){
	PowerPC_instr ppc;
	
	flushRegisters();
	reset_code_addr();
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	genUpdateCount();
	
#ifdef INTERPRET_JR
	genJumpTo(MIPS_GET_RS(mips), JUMPTO_REG);
#else // INTERPRET_JR
	// TODO: jr
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); delaySlotNext = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_JR
	return INTERPRETED;
#else // INTERPRET_JR
	return CONVER_ERROR;
#endif
}

static int JALR(MIPS_instr mips){
	PowerPC_instr  ppc;
	
	flushRegisters();
	reset_code_addr();
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
	genUpdateCount();
	
	// TODO: If I can figure out using the LR,
	//         this might only be necessary for interp
	// Set LR to next instruction
	int rd = mapRegisterNew(MIPS_GET_RD(mips));
	// lis	lr, pc@ha(0)
	GEN_LIS(ppc, rd, (get_src_pc()+4)>>16);
	set_next_dst(ppc);
	// la	lr, pc@l(lr)
	GEN_ORI(ppc, rd, rd, get_src_pc()+4);
	set_next_dst(ppc);
	
	flushRegisters();
	
#ifdef INTERPRET_JALR
	genJumpTo(MIPS_GET_RS(mips), JUMPTO_REG);
#else // INTERPRET_JALR
	// TODO: jalr
#endif
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){
		unget_last_src();
		delaySlotNext = 1;
		// TODO
		// Step over the already executed delay slot if we ever
		//   actually use the real LR for JAL
		// b delaySlot+1
		//GEN_B(ppc, delaySlot+1, 0, 0);
		//set_next_dst(ppc);
	} else nop_ignored();
	
#ifdef INTERPRET_JALR
	return INTERPRETED;
#else // INTERPRET_JALR
	return CONVERT_ERROR;
#endif
}

static int SYSCALL(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SYSCALL
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SYSCALL
	// TODO: syscall
	return CONVERT_ERROR;
#endif
}

static int BREAK(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_BREAK
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_BREAK
	return CONVERT_ERROR;
#endif
}

static int SYNC(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SYNC
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_SYNC
	return CONVERT_ERROR;
#endif
}

static int MFHI(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	RegMapping hi = mapRegister64( MIPS_REG_HI );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	// mr rd, hi
	GEN_OR(ppc, rd.lo, hi.lo, hi.lo);
	set_next_dst(ppc);
	GEN_OR(ppc, rd.hi, hi.hi, hi.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MTHI(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
	RegMapping hi = mapRegister64New( MIPS_REG_HI );
	
	// mr hi, rs
	GEN_OR(ppc, hi.lo, rs.lo, rs.lo);
	set_next_dst(ppc);
	GEN_OR(ppc, hi.hi, rs.hi, rs.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MFLO(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	RegMapping lo = mapRegister64( MIPS_REG_LO );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	// mr rd, lo
	GEN_OR(ppc, rd.lo, lo.lo, lo.lo);
	set_next_dst(ppc);
	GEN_OR(ppc, rd.hi, lo.hi, lo.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MTLO(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_HILO
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_HILO
	
	RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
	RegMapping lo = mapRegister64New( MIPS_REG_LO );
	
	// mr lo, rs
	GEN_OR(ppc, lo.lo, rs.lo, rs.lo);
	set_next_dst(ppc);
	GEN_OR(ppc, lo.hi, rs.hi, rs.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MULT(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_MULT
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_MULT
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't multiply if they're using r0
	if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
		// mullw lo, rs, rt
		GEN_MULLW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// mulhw hi, rs, rt
		GEN_MULHW(ppc, hi, rs, rt);
		set_next_dst(ppc);
	} else {
		// li lo, 0
		GEN_LI(ppc, lo, 0, 0);
		set_next_dst(ppc);
		// li hi, 0
		GEN_LI(ppc, hi, 0, 0);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int MULTU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_MULTU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_MULTU
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't multiply if they're using r0
	if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
		// mullw lo, rs, rt
		GEN_MULLW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// mulhwu hi, rs, rt
		GEN_MULHWU(ppc, hi, rs, rt);
		set_next_dst(ppc);
	} else {
		// li lo, 0
		GEN_LI(ppc, lo, 0, 0);
		set_next_dst(ppc);
		// li hi, 0
		GEN_LI(ppc, hi, 0, 0);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DIV(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DIV
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DIV
	// This instruction computes the quotient and remainder
	//   and stores the results in lo and hi respectively
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't divide if they're using r0
	if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
		// divw lo, rs, rt
		GEN_DIVW(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// This is how you perform a mod in PPC
		// divw lo, rs, rt
		// NOTE: We already did that
		// mullw hi, lo, rt
		GEN_MULLW(ppc, hi, lo, rt);
		set_next_dst(ppc);
		// subf hi, hi, rs
		GEN_SUBF(ppc, hi, hi, rs);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DIVU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_DIVU
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DIVU
	// This instruction computes the quotient and remainder
	//   and stores the results in lo and hi respectively
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int hi = mapRegisterNew( MIPS_REG_HI );
	int lo = mapRegisterNew( MIPS_REG_LO );
	
	// Don't divide if they're using r0
	if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
		// divwu lo, rs, rt
		GEN_DIVWU(ppc, lo, rs, rt);
		set_next_dst(ppc);
		// This is how you perform a mod in PPC
		// divw lo, rs, rt
		// NOTE: We already did that
		// mullw hi, lo, rt
		GEN_MULLW(ppc, hi, lo, rt);
		set_next_dst(ppc);
		// subf hi, hi, rs
		GEN_SUBF(ppc, hi, hi, rs);
		set_next_dst(ppc);
	}
	
	return CONVERT_SUCCESS;
#endif
}

static int DSLLV(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSLLV)
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW || INTERPRET_DSLLV
	
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int sa = mapRegisterTemp();
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	// Mask off the shift amount (0x3f)
	GEN_RLWINM(ppc, sa, rs, 0, 26, 31);
	set_next_dst(ppc);
	// Shift the MSW
	GEN_SLW(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	// Calculate 32-sh
	GEN_SUBFIC(ppc, 0, sa, 32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the LSW (sh < 32)
	GEN_SRW(ppc, 0, rt.lo, 0);
	set_next_dst(ppc);
	// Insert the bits into the MSW
	GEN_OR(ppc, rd.hi, rd.hi, 0);
	set_next_dst(ppc);
	// Calculate sh-32
	GEN_ADDI(ppc, 0, sa, -32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the LSW (sh > 31)
	GEN_SLW(ppc, 0, rt.lo, 0);
	set_next_dst(ppc);
	// Insert the bits into the MSW
	GEN_OR(ppc, rd.hi, rd.hi, 0);
	set_next_dst(ppc);
	// Shift the LSW
	GEN_SLW(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	
	unmapRegisterTemp(sa);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRLV(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRLV)
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW || INTERPRET_DSRLV
	
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int sa = mapRegisterTemp();
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	// Mask off the shift amount (0x3f)
	GEN_RLWINM(ppc, sa, rs, 0, 26, 31);
	set_next_dst(ppc);
	// Shift the LSW
	GEN_SRW(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	// Calculate 32-sh
	GEN_SUBFIC(ppc, 0, sa, 32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the MSW (sh < 32)
	GEN_SLW(ppc, 0, rt.hi, 0);
	set_next_dst(ppc);
	// Insert the bits into the LSW
	GEN_OR(ppc, rd.lo, rd.lo, 0);
	set_next_dst(ppc);
	// Calculate sh-32
	GEN_ADDI(ppc, 0, sa, -32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the MSW (sh > 31)
	GEN_SRW(ppc, 0, rt.hi, 0);
	set_next_dst(ppc);
	// Insert the bits into the LSW
	GEN_OR(ppc, rd.lo, rd.lo, 0);
	set_next_dst(ppc);
	// Shift the MSW
	GEN_SRW(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	
	unmapRegisterTemp(sa);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRAV(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRAV)
	genCallInterp(mips);
	return INTERPRETED;
#else  // INTERPRET_DW || INTERPRET_DSRAV
	
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int sa = mapRegisterTemp();
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	// Mask off the shift amount (0x3f)
	GEN_RLWINM(ppc, sa, rs, 0, 26, 31);
	set_next_dst(ppc);
	// Check whether the shift amount is < 32
	GEN_CMPI(ppc, sa, 32, 1);
	set_next_dst(ppc);
	// Shift the LSW
	GEN_SRW(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	// Skip over this code if sh >= 32
	GEN_BGE(ppc, 1, 5, 0, 0);
	set_next_dst(ppc);
	// Calculate 32-sh
	GEN_SUBFIC(ppc, 0, sa, 32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the MSW (sh < 32)
	GEN_SLW(ppc, 0, rt.hi, 0);
	set_next_dst(ppc);
	// Insert the bits into the LSW
	GEN_OR(ppc, rd.lo, rd.lo, 0);
	set_next_dst(ppc);
	// Skip over the else
	GEN_B(ppc, 4, 0, 0);
	set_next_dst(ppc);
	// Calculate sh-32
	GEN_ADDI(ppc, 0, sa, -32);
	set_next_dst(ppc);
	// Extract the bits that will be shifted out the MSW (sh > 31)
	GEN_SRAW(ppc, 0, rt.hi, 0);
	set_next_dst(ppc);
	// Insert the bits into the LSW
	GEN_OR(ppc, rd.lo, rd.lo, 0);
	set_next_dst(ppc);
	// Shift the MSW
	GEN_SRAW(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	
	unmapRegisterTemp(sa);
	
	return CONVERT_SUCCESS;
#endif
}

static int DMULT(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DMULT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DMULT
	// TODO: dmult
	return CONVERT_ERROR;
#endif
}

static int DMULTU(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DMULTU)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DMULTU
	// TODO: dmultu
	return CONVERT_ERROR;
#endif
}

static int DDIV(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DDIV)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DDIV
	// TODO: ddiv
	return CONVERT_ERROR;
#endif
}

static int DDIVU(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DDIVU)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DDIVU
	// TODO: ddivu
	return CONVERT_ERROR;
#endif
}

static int DADDU(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DADDU)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DADDU
	
	RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	GEN_ADDC(ppc, rd.lo, rs.lo, rt.lo);
	set_next_dst(ppc);
	GEN_ADDE(ppc, rd.hi, rs.hi, rt.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DADD(MIPS_instr mips){
	return DADDU(mips);
}

static int DSUBU(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSUBU)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSUBU
	
	RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	
	GEN_SUBFC(ppc, rd.lo, rt.lo, rs.lo);
	set_next_dst(ppc);
	GEN_SUBFE(ppc, rd.hi, rt.hi, rs.hi);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSUB(MIPS_instr mips){
	return DSUBU(mips);
}

static int DSLL(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSLL)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSLL
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift MSW left by SA
	GEN_SLWI(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	// Extract the bits shifted out of the LSW
	// FIXME: If sa is 0, this wouldn't work properly
	GEN_RLWINM(ppc, 0, rt.lo, sa, 32-sa, 31);
	set_next_dst(ppc);
	// Insert those bits into the MSW
	GEN_OR(ppc, rd.hi, rd.hi, 0);
	set_next_dst(ppc);
	// Shift LSW left by SA
	GEN_SLWI(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRL(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRL)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSRL
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift LSW right by SA
	GEN_SRWI(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	// Extract the bits shifted out of the MSW
	// FIXME: If sa is 0, this wouldn't work properly
	GEN_RLWINM(ppc, 0, rt.hi, 32-sa, 0, sa-1);
	set_next_dst(ppc);
	// Insert those bits into the LSW
	GEN_OR(ppc, rd.lo, rt.lo, 0);
	set_next_dst(ppc);
	// Shift MSW right by SA
	GEN_SRWI(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRA(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRA)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSRA
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift LSW right by SA
	GEN_SRWI(ppc, rd.lo, rt.lo, sa);
	set_next_dst(ppc);
	// Extract the bits shifted out of the MSW
	// FIXME: If sa is 0, this wouldn't work properly
	GEN_RLWINM(ppc, 0, rt.hi, 32-sa, 0, sa-1);
	set_next_dst(ppc);
	// Insert those bits into the LSW
	GEN_OR(ppc, rd.lo, rt.lo, 0);
	set_next_dst(ppc);
	// Shift (arithmetically) MSW right by SA
	GEN_SRAWI(ppc, rd.hi, rt.hi, sa);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSLL32(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSLL32)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSLL32
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift LSW into MSW and by SA
	GEN_SLWI(ppc, rd.hi, rt.lo, sa);
	set_next_dst(ppc);
	// Clear out LSW
	GEN_ADDI(ppc, rd.lo, 0, 0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRL32(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRL32)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSRL32
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift MSW into LSW and by SA
	GEN_SRWI(ppc, rd.lo, rt.hi, sa);
	set_next_dst(ppc);
	// Clear out MSW
	GEN_ADDI(ppc, rd.hi, 0, 0);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DSRA32(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_DW) || defined(INTERPRET_DSRA32)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_DW || INTERPRET_DSRA32
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
	int sa = MIPS_GET_SA(mips);
	
	// Shift (arithmetically) MSW into LSW and by SA
	GEN_SRAWI(ppc, rd.lo, rt.hi, sa);
	set_next_dst(ppc);
	// Fill MSW with sign of MSW
	GEN_SRAWI(ppc, rd.hi, rt.hi, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int ADDU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_ADD(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int ADD(MIPS_instr mips){
	return ADDU(mips);
}

static int SUBU(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_SUB(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SUB(MIPS_instr mips){
	return SUBU(mips);
}

static int AND(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_AND(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int OR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_OR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int XOR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_XOR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int NOR(MIPS_instr mips){
	PowerPC_instr ppc;
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	GEN_NOR(ppc,
	        mapRegisterNew( MIPS_GET_RD(mips) ),
	        rs,
	        rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
}

static int SLT(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SLT
	genCallInterp(mips);
	return INTERPRETED;
#else
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rd = mapRegisterNew( MIPS_GET_RD(mips) );
	// TODO: rs < r0 can be done in one instruction
	// carry = rs < rt ? 0 : 1 (unsigned)
	GEN_SUBFC(ppc, 0, rt, rs);
	set_next_dst(ppc);
	// rd = ~(rs ^ rt)
	GEN_EQV(ppc, rd, rt, rs);
	set_next_dst(ppc);
	// rd = sign(rs) == sign(rt) ? 1 : 0
	GEN_SRWI(ppc, rd, rd, 31);
	set_next_dst(ppc);
	// rd += carry
	GEN_ADDZE(ppc, rd, rd);
	set_next_dst(ppc);
	// rt &= 1 ( = (sign(rs) == sign(rt)) xor (rs < rt (unsigned)) ) 
	GEN_RLWINM(ppc, rd, rd, 0, 31, 31);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int SLTU(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_SLTU
	genCallInterp(mips);
	return INTERPRETED;
#else
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int rs = mapRegister( MIPS_GET_RS(mips) );
	int rd = mapRegisterNew( MIPS_GET_RD(mips) );
	// carry = rs < rt ? 0 : 1
	GEN_SUBFC(ppc, rd, rt, rs);
	set_next_dst(ppc);
	// rd = carry - 1 ( = rs < rt ? -1 : 0 )
	GEN_SUBFE(ppc, rd, rd, rd);
	set_next_dst(ppc);
	// rd = !carry ( = rs < rt ? 1 : 0 )
	GEN_NEG(ppc, rd, rd);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int TEQ(MIPS_instr mips){
	PowerPC_instr ppc;
#ifdef INTERPRET_TRAPS
	genCallInterp(mips);
	return INTERPRETED;
#else
	return CONVERT_ERROR;
#endif
}

static int (*gen_special[64])(MIPS_instr) =
{
   SLL , NI   , SRL , SRA , SLLV   , NI    , SRLV  , SRAV  ,
   JR  , JALR , NI  , NI  , SYSCALL, BREAK , NI    , SYNC  ,
   MFHI, MTHI , MFLO, MTLO, DSLLV  , NI    , DSRLV , DSRAV ,
   MULT, MULTU, DIV , DIVU, DMULT  , DMULTU, DDIV  , DDIVU ,
   ADD , ADDU , SUB , SUBU, AND    , OR    , XOR   , NOR   ,
   NI  , NI   , SLT , SLTU, DADD   , DADDU , DSUB  , DSUBU ,
   NI  , NI   , NI  , NI  , TEQ    , NI    , NI    , NI    ,
   DSLL, NI   , DSRL, DSRA, DSLL32 , NI    , DSRL32, DSRA32
};

static int SPECIAL(MIPS_instr mips){
	return gen_special[MIPS_GET_FUNC(mips)](mips);
}

// -- RegImmed Instructions --

// Since the RegImmed instructions are very similar:
//   BLTZ, BGEZ, BLTZL, BGEZL, BLZAL, BGEZAL, BLTZALL, BGEZALL
//   It's less work to handle them all in one function
static int REGIMM(MIPS_instr mips){
	PowerPC_instr  ppc;
	int which = MIPS_GET_RT(mips);
	int cond   = which & 1; // t = GE, f = LT
	int likely = which & 2;
	int link   = which & 16;
	
	if(MIPS_GET_IMMED(mips) == 0xffff){
		// REGIMM_IDLE
		genCallInterp(mips);
		return INTERPRETED;
	}
	
	// cmpi ra, 0
	GEN_CMPI(ppc, mapRegister(MIPS_GET_RA(mips)), 0, 4);
	set_next_dst(ppc);
	
	return branch(signExtend(MIPS_GET_IMMED(mips),16),
	              cond ? GE : LT, link, likely);
}

// -- COP0 Instructions --

/*
static int (*gen_cop0[32])(MIPS_instr) =
{
   MFC0, NI, NI, NI, MTC0, NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI,
   TLB , NI, NI, NI, NI  , NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI
};
*/

static int COP0(MIPS_instr mips){
#ifdef INTERPRET_COP0
	genCallInterp(mips);
	return INTERPRETED;
#else
	// TODO: COP0 instructions
	return CONVERT_ERROR;
#endif
}

// -- COP1 Instructions --

static int MFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_MFC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	
	int fs = MIPS_GET_FS(mips);
	int rt = mapRegisterNew( MIPS_GET_RT(mips) );
	flushFPR(fs);
	
	// rt = reg_cop1_simple[fs]
	GEN_LWZ(ppc, rt, fs*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// rt = *rt
	GEN_LWZ(ppc, rt, 0, rt);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DMFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_DMFC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	
	int fs = mapFPR( MIPS_GET_FS(mips), 1 );
	int rt = MIPS_GET_RT(mips);
	invalidateRegister(rt);
	
	GEN_STFD(ppc, fs, rt*8, DYNAREG_REG);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int CFC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_CFC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: cfc1
#endif
}

static int MTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_MTC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	
	int rt = mapRegister( MIPS_GET_RT(mips) );
	int fs = MIPS_GET_FS(mips);
	int addr = mapRegisterTemp();
	invalidateFPR(fs);
	
	// addr = reg_cop1_simple[fs]
	GEN_LWZ(ppc, addr, fs*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// *addr = rt
	GEN_STW(ppc, rt, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int DMTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_DMTC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	
	RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
	int fs = MIPS_GET_FS(mips);
	int addr = mapRegisterTemp();
	invalidateFPR(fs);
	
	GEN_LWZ(ppc, addr, fs*4, DYNAREG_FPR_64);
	set_next_dst(ppc);
	GEN_STW(ppc, rt.lo, 0, addr);
	set_next_dst(ppc);
	GEN_STW(ppc, rt.hi, 4, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int CTC1(MIPS_instr mips){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_CTC1)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP
	// TODO: ctc1
#endif
}

static int BC(MIPS_instr mips){
#if defined(INTERPRET_FP) || defined(INTERPRET_BC)
	genCallInterp(mips);
	return INTERPRETED;
#else
	PowerPC_instr ppc;
	int cond   = mips & 0x00010000;
	int likely = mips & 0x00020000;
	int likely_id = -1;
	
	flushRegisters();
	reset_code_addr();
	
	// Note: we use CR bits 20-27 (CRs 5&6) to store N64 CCs
#ifdef INTERPRET_FP
	// Load the value from FCR31 and use that to set condition
	extern long FCR31;
	// mtctr r3
	GEN_MTCTR(ppc, 3);
	set_next_dst(ppc);
	// la r3, &FCR31
	GEN_LIS(ppc, 3, ((unsigned int)&FCR31)>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, 3, 3, (unsigned int)&FCR31);
	set_next_dst(ppc);
	// lwz r3, 0(r3)
	GEN_LWZ(ppc, 3, 0, 3);
	set_next_dst(ppc);
	// The intention here is to shift the MSb
	//   to the bit that will be checked by the bc
	// srwi r3, r3, 20+MIPS_GET_CC(mips)
	GEN_SRWI(ppc, 3, 3, 20+MIPS_GET_CC(mips));
	set_next_dst(ppc);
	// FIXME: This destroys other CCs
	// mtcrf cr5 & cr6, 3
	ppc = NEW_PPC_INSTR();
	PPC_SET_OPCODE(ppc, PPC_OPCODE_X);
	PPC_SET_FUNC  (ppc, PPC_FUNC_MTCRF);
	PPC_SET_RD    (ppc, 3);
	ppc |= 0x06 << 12; // Set CRM to CR5
	set_next_dst(ppc);
	// mfctr r3
	GEN_MFCTR(ppc, 3);
	set_next_dst(ppc);
#endif // INTERPRET_FP
	
	if(likely){
		// b[!cond] <past jumpto & delay>
		likely_id = add_jump_special(0);
		GEN_BC(ppc, likely_id, 0, 0,
		       cond ? 0x4 : 0xc,
		       20+MIPS_GET_CC(mips)); 
		set_next_dst(ppc);
	}
	
	// Check the delay slot, and note how big it is
	PowerPC_instr* preDelay = get_curr_dst();
	check_delaySlot();
	int delaySlot = get_curr_dst() - preDelay;
	
#ifdef INTERPRET_BC
	if(likely)
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
	else {
		// b[!cond] <past jumpto & delay> 
		GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0,
		       cond ? 0x4 : 0xc,
		       20+MIPS_GET_CC(mips)); 
		set_next_dst(ppc);
	}
	genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
#else // INTERPRET_BC
	// If we're jumping out, we need pizza
	if(is_j_out(signExtend(MIPS_GET_IMMED(mips),16), 0)){
		if(likely)
			// Jump over the generated jump, and both delay slots
			set_jump_special(likely_id, JUMPTO_OFF_SIZE+2*delaySlot+1);
		else {
			// b[!cond] <past jumpto & delay> 
			GEN_BC(ppc, JUMPTO_OFF_SIZE+delaySlot+2, 0, 0,
			       cond ? 0x4 : 0xc,
			       20+MIPS_GET_CC(mips)); 
			set_next_dst(ppc);
		}
		genJumpTo(signExtend(MIPS_GET_IMMED(mips),16), JUMPTO_OFF);
	} else if(likely){
		// Jump over the generated jump, and both delay slots
		set_jump_special(likely_id, 1+2*delaySlot+1);
		
		// b[cond] <dest> 
		GEN_BC(ppc, 
		       add_jump(signExtend(MIPS_GET_IMMED(mips),16), 0, j_out),
		       0, 0,
		       cond ? 0xc : 0x4,
		       20+MIPS_GET_CC(mips));
		set_next_dst(ppc);
	}
#endif // INTERPRET_BC
	
	if(!likely){
		// Step over the already executed delay slot if
		//   the branch isn't taken, not necessary for
		//   a likely branch because it'll be skipped
		// b delaySlot+1
		GEN_B(ppc, delaySlot+1, 0, 0);
		set_next_dst(ppc);
	}
	
	// Let's still recompile the delay slot in place in case its branched to
	if(delaySlot){ unget_last_src(); delaySlotNext = 1; }
	else nop_ignored();
	
#ifdef INTERPRET_BC
	return INTERPRETED;
#else // INTERPRET_BC
	return CONVERT_SUCCESS;
#endif // INTERPRET_BC
#endif // INTERPRET_FP
}

// -- Floating Point Arithmetic --
static int ADD_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_ADD)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_ADD
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int ft = mapFPR( MIPS_GET_FT(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FADD(ppc, fd, fs, ft);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int SUB_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_SUB)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_SUB
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int ft = mapFPR( MIPS_GET_FT(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FSUB(ppc, fd, fs, ft);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MUL_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_MUL)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_MUL
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int ft = mapFPR( MIPS_GET_FT(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FMUL(ppc, fd, fs, ft);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int DIV_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_DIV)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_DIV
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int ft = mapFPR( MIPS_GET_FT(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FDIV(ppc, fd, fs, ft);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int SQRT_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_SQRT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_SQRT
	
	static double one = 1.0;
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	int addr = mapRegisterTemp();
	
	// li addr, &one
	GEN_LIS(ppc, addr, ((unsigned int)&one)>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, addr, addr, (unsigned int)&one);
	set_next_dst(ppc);
	// lfd f0, 0(addr)
	GEN_LFD(ppc, 0, 0, addr);
	set_next_dst(ppc);
	// frsqrte fd, rs
	GEN_FRSQRTE(ppc, fd, fs);
	set_next_dst(ppc);
	// fdiv fd, f0, fd
	GEN_FDIV(ppc, fd, 0, fd);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int ABS_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_ABS)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_ABS
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FABS(ppc, fd, fs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int MOV_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_MOV)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_MOV
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FMR(ppc, fd, fs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int NEG_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_NEG)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_NEG
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	
	GEN_FNEG(ppc, fd, fs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

// -- Floating Point Rounding/Conversion --
#define PPC_ROUNDING_NEAREST 0
#define PPC_ROUNDING_TRUNC   1
#define PPC_ROUNDING_CEIL    2
#define PPC_ROUNDING_FLOOR   3
static void set_rounding(int rounding_mode){
	PowerPC_instr ppc;
	
	GEN_MTFSFI(ppc, 7, rounding_mode);
	set_next_dst(ppc);
}

static void set_rounding_reg(int rs){
	PowerPC_instr ppc;
	
	GEN_MTFSF(ppc, 1, rs);
	set_next_dst(ppc);
}

static int ROUND_L_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_ROUND_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_ROUND_L
	// TODO: ROUND_L
	return CONVERT_ERROR;
#endif
}

static int TRUNC_L_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_TRUNC_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_TRUNC_L
	// TODO: TRUNC_L
	return CONVERT_ERROR;
#endif
}

static int CEIL_L_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CEIL_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CEIL_L
	// TODO: CEIL_L
	return CONVERT_ERROR;
#endif
}

static int FLOOR_L_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_FLOOR_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_FLOOR_L
	// TODO: FLOOR_L
	return CONVERT_ERROR;
#endif
}

static int ROUND_W_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_ROUND_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_ROUND_W
	
	set_rounding(PPC_ROUNDING_NEAREST);
	
	int fd = MIPS_GET_FD(mips);
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	invalidateFPR(fd);
	int addr = mapRegisterTemp();
	
	// fctiw f0, fs
	GEN_FCTIW(ppc, 0, fs);
	set_next_dst(ppc);
	// addr = reg_cop1_simple[fd]
	GEN_LWZ(ppc, addr, fd*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// stfiwx f0, 0, addr
	GEN_STFIWX(ppc, 0, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int TRUNC_W_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_TRUNC_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_TRUNC_W
	
	int fd = MIPS_GET_FD(mips);
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	invalidateFPR(fd);
	int addr = mapRegisterTemp();
	
	// fctiwz f0, fs
	GEN_FCTIWZ(ppc, 0, fs);
	set_next_dst(ppc);
	// addr = reg_cop1_simple[fd]
	GEN_LWZ(ppc, addr, fd*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// stfiwx f0, 0, addr
	GEN_STFIWX(ppc, 0, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int CEIL_W_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CEIL_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CEIL_W
	
	set_rounding(PPC_ROUNDING_CEIL);
	
	int fd = MIPS_GET_FD(mips);
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	invalidateFPR(fd);
	int addr = mapRegisterTemp();
	
	// fctiw f0, fs
	GEN_FCTIW(ppc, 0, fs);
	set_next_dst(ppc);
	// addr = reg_cop1_simple[fd]
	GEN_LWZ(ppc, addr, fd*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// stfiwx f0, 0, addr
	GEN_STFIWX(ppc, 0, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int FLOOR_W_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_FLOOR_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_FLOOR_W
	
	set_rounding(PPC_ROUNDING_FLOOR);
	
	int fd = MIPS_GET_FD(mips);
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	invalidateFPR(fd);
	int addr = mapRegisterTemp();
	
	// fctiw f0, fs
	GEN_FCTIW(ppc, 0, fs);
	set_next_dst(ppc);
	// addr = reg_cop1_simple[fd]
	GEN_LWZ(ppc, addr, fd*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// stfiwx f0, 0, addr
	GEN_STFIWX(ppc, 0, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int CVT_S_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CVT_S)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CVT_S
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), 0 );
	
	GEN_FMR(ppc, fd, fs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int CVT_D_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CVT_D)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CVT_D
	
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	int fd = mapFPRNew( MIPS_GET_FD(mips), 1 );
	
	GEN_FMR(ppc, fd, fs);
	set_next_dst(ppc);
	
	return CONVERT_SUCCESS;
#endif
}

static int CVT_W_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CVT_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CVT_W
	
	// TODO: Set rounding mode according to FCR31
	
	int fd = MIPS_GET_FD(mips);
	int fs = mapFPR( MIPS_GET_FS(mips), dbl );
	invalidateFPR(fd);
	int addr = mapRegisterTemp();
	
	// fctiw f0, fs
	GEN_FCTIW(ppc, 0, fs);
	set_next_dst(ppc);
	// addr = reg_cop1_simple[fd]
	GEN_LWZ(ppc, addr, fd*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// stfiwx f0, 0, addr
	GEN_STFIWX(ppc, 0, 0, addr);
	set_next_dst(ppc);
	
	unmapRegisterTemp(addr);
	
	return CONVERT_SUCCESS;
#endif
}

static int CVT_L_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_CVT_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_CVT_L
	// TODO: CVT_L
	return CONVERT_ERROR;
#endif
}

// -- Floating Point Comparisons --
static int C_F_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_F)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_F
	// TODO: C_F
	return CONVERT_ERROR;
#endif
}

static int C_UN_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_UN)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_UN
	// TODO: C_UN
	return CONVERT_ERROR;
#endif
}

static int C_EQ_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_EQ)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_EQ
	// TODO: C_EQ
	return CONVERT_ERROR;
#endif
}

static int C_UEQ_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_UEQ)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_UEQ
	// TODO: C_UEQ
	return CONVERT_ERROR;
#endif
}

static int C_OLT_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_OLT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_OLT
	// TODO: C_OLT
	return CONVERT_ERROR;
#endif
}

static int C_ULT_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_ULT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_ULT
	// TODO: C_ULT
	return CONVERT_ERROR;
#endif
}

static int C_OLE_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_OLE)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_OLE
	// TODO: C_OLE
	return CONVERT_ERROR;
#endif
}

static int C_ULE_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_ULE)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_ULE
	// TODO: C_ULE
	return CONVERT_ERROR;
#endif
}

static int C_SF_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_SF)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_SF
	// TODO: C_SF
	return CONVERT_ERROR;
#endif
}

static int C_NGLE_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_NGLE)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_NGLE
	// TODO: C_NGLE
	return CONVERT_ERROR;
#endif
}

static int C_SEQ_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_SEQ)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_SEQ
	// TODO: C_SEQ
	return CONVERT_ERROR;
#endif
}

static int C_NGL_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_NGL)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_NGL
	// TODO: C_NGL
	return CONVERT_ERROR;
#endif
}

static int C_LT_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_LT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_LT
	// TODO: C_LT
	return CONVERT_ERROR;
#endif
}

static int C_NGE_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_NGE)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_NGE
	// TODO: C_NGE
	return CONVERT_ERROR;
#endif
}

static int C_LE_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_LE)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_LE
	// TODO: C_LE
	return CONVERT_ERROR;
#endif
}

static int C_NGT_FP(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_C_NGT)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_C_NGT
	// TODO: C_NGT
	return CONVERT_ERROR;
#endif
}

static int (*gen_cop1_fp[64])(MIPS_instr, int) =
{
   ADD_FP    ,SUB_FP    ,MUL_FP   ,DIV_FP    ,SQRT_FP   ,ABS_FP    ,MOV_FP   ,NEG_FP    ,
   ROUND_L_FP,TRUNC_L_FP,CEIL_L_FP,FLOOR_L_FP,ROUND_W_FP,TRUNC_W_FP,CEIL_W_FP,FLOOR_W_FP,
   NI        ,NI        ,NI       ,NI        ,NI        ,NI        ,NI       ,NI        ,
   NI        ,NI        ,NI       ,NI        ,NI        ,NI        ,NI       ,NI        ,
   CVT_S_FP  ,CVT_D_FP  ,NI       ,NI        ,CVT_W_FP  ,CVT_L_FP  ,NI       ,NI        ,
   NI        ,NI        ,NI       ,NI        ,NI        ,NI        ,NI       ,NI        ,
   C_F_FP    ,C_UN_FP   ,C_EQ_FP  ,C_UEQ_FP  ,C_OLT_FP  ,C_ULT_FP  ,C_OLE_FP ,C_ULE_FP  ,
   C_SF_FP   ,C_NGLE_FP ,C_SEQ_FP ,C_NGL_FP  ,C_LT_FP   ,C_NGE_FP  ,C_LE_FP  ,C_NGT_FP
};

static int S(MIPS_instr mips){
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_S)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_S
	return gen_cop1_fp[ MIPS_GET_FUNC(mips) ](mips, 0);
#endif
}

static int D(MIPS_instr mips){
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_D)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_D
	return gen_cop1_fp[ MIPS_GET_FUNC(mips) ](mips, 1);
#endif
}

static int CVT_FP_W(MIPS_instr mips, int dbl){
	PowerPC_instr ppc;
	
	int fs = MIPS_GET_FS(mips);
	flushFPR(fs);
	int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
	int tmp = mapRegisterTemp();
	
	// Get the integer value into a GPR
	// tmp = fpr32[fs]
	GEN_LWZ(ppc, tmp, fs*4, DYNAREG_FPR_32);
	set_next_dst(ppc);
	// tmp = *tmp (src)
	GEN_LWZ(ppc, tmp, 0, tmp);
	set_next_dst(ppc);
	
	// lis r0, 0x4330
	GEN_LIS(ppc, 0, 0x4330);
	set_next_dst(ppc);
	// stw r0, -8(r1)
	GEN_STW(ppc, 0, -8, 1);
	set_next_dst(ppc);
	// lis r0, 0x8000
	GEN_LIS(ppc, 0, 0x8000);
	set_next_dst(ppc);
	// stw r0, -4(r1)
	GEN_STW(ppc, 0, -4, 1);
	set_next_dst(ppc);
	// xor r0, src, 0x80000000
	GEN_XOR(ppc, 0, tmp, 0);
	set_next_dst(ppc);
	// lfd f0, -8(r1)
	GEN_LFD(ppc, 0, -8, 1);
	set_next_dst(ppc);
	// stw r0 -4(r1)
	GEN_STW(ppc, 0, -4, 1);
	set_next_dst(ppc);
	// lfd fd, -8(r1)
	GEN_LFD(ppc, fd, -8, 1);
	set_next_dst(ppc);
	// fsub fd, fd, f0
	GEN_FSUB(ppc, fd, fd, 0);
	set_next_dst(ppc);
	
	unmapRegisterTemp(tmp);
	
	return CONVERT_SUCCESS;
}

static int W(MIPS_instr mips){
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_W)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_W
	
	int func = MIPS_GET_FUNC(mips);
	
	if(func == MIPS_FUNC_CVT_S_) return CVT_FP_W(mips, 0);
	if(func == MIPS_FUNC_CVT_D_) return CVT_FP_W(mips, 1);
	else return CONVERT_ERROR;
#endif
}

static int L(MIPS_instr mips){
#if defined(INTERPRET_FP) || defined(INTERPRET_FP_L)
	genCallInterp(mips);
	return INTERPRETED;
#else // INTERPRET_FP || INTERPRET_FP_L
	// TODO: long-integer FP
	return CONVERT_ERROR;
#endif
}

static int (*gen_cop1[32])(MIPS_instr) =
{
   MFC1, DMFC1, CFC1, NI, MTC1, DMTC1, CTC1, NI,
   BC  , NI   , NI  , NI, NI  , NI   , NI  , NI,
   S   , D    , NI  , NI, W   , L    , NI  , NI,
   NI  , NI   , NI  , NI, NI  , NI   , NI  , NI
};

static int COP1(MIPS_instr mips){
	return gen_cop1[MIPS_GET_RS(mips)](mips);
}

static int (*gen_ops[64])(MIPS_instr) =
{
   SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ , BGTZ ,
   ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI , LUI  ,
   COP0   , COP1  , NI  , NI   , BEQL, BNEL, BLEZL, BGTZL,
   DADDI  , DADDIU, LDL , LDR  , NI  , NI  , NI   , NI   ,
   LB     , LH    , LWL , LW   , LBU , LHU , LWR  , LWU  ,
   SB     , SH    , SWL , SW   , SDL , SDR , SWR  , CACHE,
   LL     , LWC1  , NI  , NI   , NI  , LDC1, NI   , LD   ,
   SC     , SWC1  , NI  , NI   , NI  , SDC1, NI   , SD
};



static void genCallInterp(MIPS_instr mips){
	PowerPC_instr ppc = NEW_PPC_INSTR();
	interpretedLoop = 1;
	flushRegisters();
	reset_code_addr();
	// Pass in whether this instruction is in the delay slot
	GEN_LI(ppc, 5, 0, isDelaySlot ? 1 : 0);
	set_next_dst(ppc);
	// Save the lr
	GEN_MFLR(ppc, 0);
	set_next_dst(ppc);
	GEN_STW(ppc, 0, DYNAOFF_LR, 1);
	set_next_dst(ppc);
#if 0
	// Load the address of decodeNInterpret
	GEN_LIS(ppc, 3, ((unsigned int)decodeNInterpret)>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, 3, 3, (unsigned int)decodeNInterpret);
	set_next_dst(ppc);
#endif
	// Move the address of decodeNInterpret to ctr for a bctr
	GEN_MTCTR(ppc, DYNAREG_INTERP);
	set_next_dst(ppc);
	// Load our argument into r3 (mips)
	GEN_LIS(ppc, 3, mips>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, 3, 3, mips);
	set_next_dst(ppc);
	// Load the current PC as the second arg
	GEN_LIS(ppc, 4, get_src_pc()>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, 4, 4, get_src_pc());
	set_next_dst(ppc);
	// Branch to decodeNInterpret
	GEN_BCTRL(ppc);
	set_next_dst(ppc);
	// Restore the lr
	GEN_LWZ(ppc, 0, DYNAOFF_LR, 1);
	set_next_dst(ppc);
	GEN_MTLR(ppc, 0);
	set_next_dst(ppc);
	// if decodeNInterpret returned an address
	//   jumpTo it
	GEN_CMPI(ppc, 3, 0, 6);
	set_next_dst(ppc);
#if 0
	// TODO: It would be nice to re-enable this when possible
	GEN_BNE(ppc, 6, add_jump(-1, 0, 1), 0, 0);
	set_next_dst(ppc);
#else
	GEN_BEQ(ppc, 6, 2, 0, 0);
	set_next_dst(ppc);
	/*GEN_B(ppc, add_jump(-1, 1, 1), 0, 0);
	set_next_dst(ppc);*/
	GEN_BLR(ppc, 0);
	set_next_dst(ppc);
#endif
	if(mips_is_jump(mips)) delaySlotNext = 1;
}

static void genJumpTo(unsigned int loc, unsigned int type){
	PowerPC_instr ppc = NEW_PPC_INSTR();
	
	if(type == JUMPTO_REG){
		// Load the register as the return value
		GEN_LWZ(ppc, 3, loc*8+4, DYNAREG_REG);
		set_next_dst(ppc);
	} else {
		// Calculate the destination address
		loc <<= 2;
		if(type == JUMPTO_OFF) loc += get_src_pc();
		else loc |= get_src_pc() & 0xf0000000;
		// Load the address as the return value
		GEN_LIS(ppc, 3, loc >> 16);
		set_next_dst(ppc);
		GEN_ORI(ppc, 3, 3, loc);
		set_next_dst(ppc);
	}
#if 0
	// Branch to the jump pad
	GEN_B(ppc, add_jump(loc, 1, 1), 0, 0);
	set_next_dst(ppc);
#else
	GEN_BLR(ppc, 0);
	set_next_dst(ppc);
#endif
}

// Updates Count, and sets cr2 to (next_interupt ? Count)
static void genUpdateCount(void){
	PowerPC_instr ppc = NEW_PPC_INSTR();
	// Move &dyna_update_count to ctr for call
	GEN_MTCTR(ppc, DYNAREG_UCOUNT);
	set_next_dst(ppc);
	// Save the lr
	GEN_MFLR(ppc, 0);
	set_next_dst(ppc);
	GEN_STW(ppc, 0, DYNAOFF_LR, 1);
	set_next_dst(ppc);
	// Load the current PC as the argument
	GEN_LIS(ppc, 3, (get_src_pc()+4)>>16);
	set_next_dst(ppc);
	GEN_ORI(ppc, 3, 3, get_src_pc()+4);
	set_next_dst(ppc);
	// Call dyna_update_count
	GEN_BCTRL(ppc);
	set_next_dst(ppc);
	// Load the lr
	GEN_LWZ(ppc, 0, DYNAOFF_LR, 1);
	set_next_dst(ppc);
	GEN_MTLR(ppc, 0);
	set_next_dst(ppc);
	// If next_interupt <= Count (cr2)
	GEN_CMPI(ppc, 3, 0, 2);
	set_next_dst(ppc);
}

static int mips_is_jump(MIPS_instr instr){
	int opcode = MIPS_GET_OPCODE(instr);
	int format = MIPS_GET_RS    (instr);
	int func   = MIPS_GET_FUNC  (instr);
	return (opcode == MIPS_OPCODE_J     ||
                opcode == MIPS_OPCODE_JAL   ||
                opcode == MIPS_OPCODE_BEQ   ||
                opcode == MIPS_OPCODE_BNE   ||
                opcode == MIPS_OPCODE_BLEZ  ||
                opcode == MIPS_OPCODE_BGTZ  ||
                opcode == MIPS_OPCODE_BEQL  ||
                opcode == MIPS_OPCODE_BNEL  ||
                opcode == MIPS_OPCODE_BLEZL ||
                opcode == MIPS_OPCODE_BGTZL ||
                opcode == MIPS_OPCODE_B     ||
                (opcode == MIPS_OPCODE_R    &&
                 (func  == MIPS_FUNC_JR     ||
                  func  == MIPS_FUNC_JALR)) ||
                (opcode == MIPS_OPCODE_COP1 &&
                 format == MIPS_FRMT_BC)    );
}
