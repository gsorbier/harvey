/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * SIMD Floating Point.
 * Assembler support to get at the individual instructions
 * is in l64fpu.s.
 * There are opportunities to be lazier about saving and
 * restoring the state and allocating the storage needed.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "amd64.h"
#include "ureg.h"

enum {						/* FCW, FSW and MXCSR */
	I		= 0x00000001,		/* Invalid-Operation */
	D		= 0x00000002,		/* Denormalized-Operand */
	Z		= 0x00000004,		/* Zero-Divide */
	O		= 0x00000008,		/* Overflow */
	U		= 0x00000010,		/* Underflow */
	P		= 0x00000020,		/* Precision */
};

enum {						/* FCW */
	PCs		= 0x00000000,		/* Precision Control -Single */
	PCd		= 0x00000200,		/* -Double */
	PCde		= 0x00000300,		/* -Double Extended */
	RCn		= 0x00000000,		/* Rounding Control -Nearest */
	RCd		= 0x00000400,		/* -Down */
	RCu		= 0x00000800,		/* -Up */
	RCz		= 0x00000C00,		/* -Toward Zero */
};

enum {						/* FSW */
	Sff		= 0x00000040,		/* Stack Fault Flag */
	Es		= 0x00000080,		/* Error Summary Status */
	C0		= 0x00000100,		/* ZF - Condition Code Bits */
	C1		= 0x00000200,		/* O/U# */
	C2		= 0x00000400,		/* PF */
	C3		= 0x00004000,		/* ZF */
	B		= 0x00008000,		/* Busy */
};

enum {						/* MXCSR */
	Daz		= 0x00000040,		/* Denormals are Zeros */
	Im		= 0x00000080,		/* I Mask */
	Dm		= 0x00000100,		/* D Mask */
	Zm		= 0x00000200,		/* Z Mask */
	Om		= 0x00000400,		/* O Mask */
	Um		= 0x00000800,		/* U Mask */
	Pm		= 0x00001000,		/* P Mask */
	Rn		= 0x00000000,		/* Round to Nearest */
	Rd		= 0x00002000,		/* Round Down */
	Ru		= 0x00004000,		/* Round Up */
	Rz		= 0x00006000,		/* Round toward Zero */
	Fz		= 0x00008000,		/* Flush to Zero for Um */
};

enum {						/* PFPU.state */
	Init		= 0,			/* The FPU has not been used */
	Busy		= 1,			/* The FPU is being used */
	Idle		= 2,			/* The FPU has been used */

	Hold		= 4,			/* Handling an FPU note */
};

extern void _clts(void);
extern void _fldcw(uint16_t);
extern void _fnclex(void);
extern void _fninit(void);
extern void _fxrstor(Fxsave*);
extern void _fxsave(Fxsave*);
extern void _fwait(void);
extern void _ldmxcsr(uint32_t);
extern void _stts(void);

int
fpudevprocio(Proc* proc, void* a, int32_t n, uintptr_t offset, int write)
{
	uint8_t *p;

	/*
	 * Called from procdevtab.read and procdevtab.write
	 * allow user process access to the FPU registers.
	 * This is the only FPU routine which is called directly
	 * from the port code; it would be nice to have dynamic
	 * creation of entries in the device file trees...
	 */
	if(offset >= sizeof(Fxsave))
		return 0;
	if((p = proc->fpusave) == nil)
		return 0;
	switch(write){
	default:
		if(offset+n > sizeof(Fxsave))
			n = sizeof(Fxsave) - offset;
		memmove(p+offset, a, n);
		break;
	case 0:
		if(offset+n > sizeof(Fxsave))
			n = sizeof(Fxsave) - offset;
		memmove(a, p+offset, n);
		break;
	}

	return n;
}

void
fpunotify(Ureg* u)
{
	/*
	 * Called when a note is about to be delivered to a
	 * user process, usually at the end of a system call.
	 * Note handlers are not allowed to use the FPU so
	 * the state is marked (after saving if necessary) and
	 * checked in the Device Not Available handler.
	 */
	if(m->externup->fpustate == Busy){
		_fxsave(m->externup->fpusave);
		_stts();
		m->externup->fpustate = Idle;
	}
	m->externup->fpustate |= Hold;
}

void
fpunoted(void)
{
	/*
	 * Called from sysnoted() via the machine-dependent
	 * noted() routine.
	 * Clear the flag set above in fpunotify().
	 */
	m->externup->fpustate &= ~Hold;
}

void
fpusysrfork(Ureg* u)
{
	/*
	 * Called early in the non-interruptible path of
	 * sysrfork() via the machine-dependent syscall() routine.
	 * Save the state so that it can be easily copied
	 * to the child process later.
	 */
	if(m->externup->fpustate != Busy)
		return;

	_fxsave(m->externup->fpusave);
	_stts();
	m->externup->fpustate = Idle;
}

void
fpusysrforkchild(Proc* child, Proc* parent)
{
	/*
	 * Called later in sysrfork() via the machine-dependent
	 * sysrforkchild() routine.
	 * Copy the parent FPU state to the child.
	 */
	child->fpustate = parent->fpustate;
	child->fpusave = (void*)((PTR2UINT(m->externup->fxsave) + 15) & ~15);
	if(child->fpustate == Init)
		return;

	memmove(child->fpusave, parent->fpusave, sizeof(Fxsave));
}

void
fpuprocsave(Proc* p)
{
	/*
	 * Called from sched() and sleep() via the machine-dependent
	 * procsave() routine.
	 * About to go in to the scheduler.
	 * If the process wasn't using the FPU
	 * there's nothing to do.
	 */
	if(p->fpustate != Busy)
		return;

	/*
	 * The process is dead so clear and disable the FPU
	 * and set the state for whoever gets this proc struct
	 * next.
	 */
	if(p->state == Moribund){
		_clts();
		_fnclex();
		_stts();
		p->fpustate = Init;
		return;
	}

	/*
	 * Save the FPU state without handling pending
	 * unmasked exceptions and disable. Postnote() can't
	 * be called here as sleep() already has m->externup->rlock,
	 * so the handling of pending exceptions is delayed
	 * until the process runs again and generates a
	 * Device Not Available exception fault to activate
	 * the FPU.
	 */
	_fxsave(p->fpusave);
	_stts();
	p->fpustate = Idle;
}

void
fpuprocrestore(Proc* p)
{
	/*
	 * The process has been rescheduled and is about to run.
	 * Nothing to do here right now. If the process tries to use
	 * the FPU again it will cause a Device Not Available
	 * exception and the state will then be restored.
	 */
	USED(p);
}

void
fpusysprocsetup(Proc* p)
{
	/*
	 * Disable the FPU.
	 * Called from sysexec() via sysprocsetup() to
	 * set the FPU for the new process.
	 */
	if(p->fpustate != Init){
		_clts();
		_fnclex();
		_stts();
		p->fpustate = Init;
	}
}

void
acfpusysprocsetup(Proc *p)
{
	if(p->fpustate == Init){
		/* The FPU is initialized in the TC but we must initialize
		 * it in the AC.
		 */
		p->fpustate = Idle;
		fpusysprocsetup(p);
	}
}

static char*
fpunote(void)
{
	uint16_t fsw;
	Fxsave *fpusave;
	char *cm;

	/*
	 * The Sff bit is sticky, meaning it should be explicitly
	 * cleared or there's no way to tell if the exception was an
	 * invalid operation or a stack fault.
	 */
	fpusave = m->externup->fpusave;
	fsw = (fpusave->fsw & ~fpusave->fcw) & (Sff|P|U|O|Z|D|I);
	if(fsw & I){
		if(fsw & Sff){
			if(fsw & C1)
				cm = "Stack Overflow";
			else
				cm = "Stack Underflow";
		}
		else
			cm = "Invalid Operation";
	}
	else if(fsw & D)
		cm = "Denormal Operand";
	else if(fsw & Z)
		cm = "Divide-By-Zero";
	else if(fsw & O)
		cm = "Numeric Overflow";
	else if(fsw & U)
		cm = "Numeric Underflow";
	else if(fsw & P)
		cm = "Precision";
	else
		cm =  "Unknown";

	snprint(m->externup->genbuf, sizeof(m->externup->genbuf),
		"sys: fp: %s Exception ipo=%#llux fsw=%#ux",
		cm, fpusave->rip, fsw);
	return m->externup->genbuf;
}

char*
xfpuxf(Ureg* ureg, void* v)
{
	uint32_t mxcsr;
	Fxsave *fpusave;
	char *cm;

	/*
	 * #XF - SIMD Floating Point Exception (Vector 18).
	 */

	/*
	 * Save FPU state to check out the error.
	 */
	fpusave = m->externup->fpusave;
	_fxsave(fpusave);
	_stts();
	m->externup->fpustate = Idle;

	if(ureg->ip & KZERO)
		panic("#MF: ip=%#p", ureg->ip);

	/*
	 * Notify the user process.
	 * The path here is similar to the x87 path described
	 * in fpupostnote above but without the fpupostnote()
	 * call.
	 */
	mxcsr = fpusave->mxcsr;
	if((mxcsr & (Im|I)) == I)
		cm = "Invalid Operation";
	else if((mxcsr & (Dm|D)) == D)
		cm = "Denormal Operand";
	else if((mxcsr & (Zm|Z)) == Z)
		cm = "Divide-By-Zero";
	else if((mxcsr & (Om|O)) == O)
		cm = "Numeric Overflow";
	else if((mxcsr & (Um|U)) == U)
		cm = "Numeric Underflow";
	else if((mxcsr & (Pm|P)) == P)
		cm = "Precision";
	else
		cm =  "Unknown";

	snprint(m->externup->genbuf, sizeof(m->externup->genbuf),
		"sys: fp: %s Exception mxcsr=%#ux", cm, mxcsr);
	return m->externup->genbuf;
}

void
fpuxf(Ureg *ureg, void *p)
{
	char *n;

	n = xfpuxf(ureg, p);
	if(n != nil)
		postnote(m->externup, 1, n, NDebug);
}

char*
acfpuxf(Ureg *ureg, void *p)
{
	return xfpuxf(ureg, p);
}

static char*
xfpumf(Ureg* ureg, void* v)
{
	Fxsave *fpusave;

	/*
	 * #MF - x87 Floating Point Exception Pending (Vector 16).
	 */

	/*
	 * Save FPU state to check out the error.
	 */
	fpusave = m->externup->fpusave;
	_fxsave(fpusave);
	_stts();
	m->externup->fpustate = Idle;

	if(ureg->ip & KZERO)
		panic("#MF: ip=%#p rip=%#p", ureg->ip, fpusave->rip);

	/*
	 * Notify the user process.
	 * The path here is
	 *	call trap->fpumf->fpupostnote->postnote
	 *	return ->fpupostnote->fpumf->trap
	 *	call notify->fpunotify
	 *	return ->notify
	 * then either
	 *	call pexit
	 * or
	 *	return ->trap
	 *	return ->user note handler
	 */
	return fpunote();
}

void
fpumf(Ureg *ureg, void *p)
{
	char *n;

	n = xfpumf(ureg, p);
	if(n != nil)
		postnote(m->externup, 1, n, NDebug);
}

char*
acfpumf(Ureg *ureg, void *p)
{
	return xfpumf(ureg, p);
}

static char*
xfpunm(Ureg* ureg, void* v)
{
	Fxsave *fpusave;

	/*
	 * #NM - Device Not Available (Vector 7).
	 */
	if(m->externup == nil)
		panic("#NM: fpu in kernel: ip %#p\n", ureg->ip);

	/*
	 * Someone tried to use the FPU in a note handler.
	 * That's a no-no.
	 */
	if(m->externup->fpustate & Hold)
		return "sys: floating point in note handler";

	if(ureg->ip & KZERO)
		panic("#NM: proc %d %s state %d ip %#p\n",
			m->externup->pid, m->externup->text, m->externup->fpustate, ureg->ip);

	switch(m->externup->fpustate){
	case Busy:
	default:
		panic("#NM: state %d ip %#p\n", m->externup->fpustate, ureg->ip);
		break;
	case Init:
		/*
		 * A process tries to use the FPU for the
		 * first time and generates a 'device not available'
		 * exception.
		 * Turn the FPU on and initialise it for use.
		 * Set the precision and mask the exceptions
		 * we don't care about from the generic Mach value.
		 */
		_clts();
		_fninit();
		_fwait();
		_fldcw(m->fcw);
		_ldmxcsr(m->mxcsr);
		m->externup->fpusave = (void*)((PTR2UINT(m->externup->fxsave) + 15) & ~15);
		m->externup->fpustate = Busy;
		break;
	case Idle:
		/*
		 * Before restoring the state, check for any pending
		 * exceptions, there's no way to restore the state without
		 * generating an unmasked exception.
		 */
		fpusave = m->externup->fpusave;
		if((fpusave->fsw & ~fpusave->fcw) & (Sff|P|U|O|Z|D|I))
			return fpunote();

		/*
		 * Sff is sticky.
		 */
		fpusave->fcw &= ~Sff;
		_clts();
		_fxrstor(fpusave);
		m->externup->fpustate = Busy;
		break;
	}
	return nil;
}

void
fpunm(Ureg *ureg, void *p)
{
	char *n;

	n = xfpunm(ureg, p);
	if(n != nil)
		postnote(m->externup, 1, n, NDebug);
}

char*
acfpunm(Ureg *ureg, void *p)
{
	return xfpunm(ureg, p);
}

void
fpuinit(void)
{
	uint64_t r;
	Fxsave *fxsave;
	uint8_t buf[sizeof(Fxsave)+15];

	/*
	 * It's assumed there is an integrated FPU, so Em is cleared;
	 */
	r = cr0get();
	r &= ~(Ts|Em);
	r |= Ne|Mp;
	cr0put(r);

	r = cr4get();
	r |= Osxmmexcpt|Osfxsr;
	cr4put(r);

	_fninit();
	fxsave = (Fxsave*)((PTR2UINT(buf) + 15) & ~15);
	memset(fxsave, 0, sizeof(Fxsave));
	_fxsave(fxsave);
	m->fcw = RCn|PCd|P|U|D;
	if(fxsave->mxcsrmask == 0)
		m->mxcsrmask = 0x0000FFBF;
	else
		m->mxcsrmask = fxsave->mxcsrmask;
	m->mxcsr = (Rn|Pm|Um|Dm) & m->mxcsrmask;
	_stts();

	if(m->machno != 0)
		return;

	/*
	 * Set up the exception handlers.
	 */
	trapenable(IdtNM, fpunm, 0, "#NM");
	trapenable(IdtMF, fpumf, 0, "#MF");
	trapenable(IdtXF, fpuxf, 0, "#XF");

	/* Same thing, for the AC */
	actrapenable(IdtNM, acfpunm, 0, "#NM");
	actrapenable(IdtMF, acfpumf, 0, "#MF");
	actrapenable(IdtXF, acfpuxf, 0, "#XF");
}
