/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <u.h>
#include <libc.h>
#include <thread.h>
#include <keyboard.h>
#include <fcall.h>
#include <plumb.h>
#include "dat.h"
#include "fns.h"

/*
 *  WASHINGTON (AP) - The Food and Drug Administration warned
 * consumers Wednesday not to use ``Rio'' hair relaxer products
 * because they may cause severe hair loss or turn hair green....
 *    The FDA urged consumers who have experienced problems with Rio
 * to notify their local FDA office, local health department or the
 * company at 1‑800‑543‑3002.
 */

void		resize(void);
void		move(void);
void		delete(void);
void		hide(void);
void		unhide(int);
void		newtile(int);
void		resized(void);
Channel	*exitchan;	/* chan(int) */
Channel	*winclosechan; /* chan(Window*); */

int		threadrforkflag = 0;	/* should be RFENVG but that hides rio from plumber */

void	mousethread(void*);
void	keyboardthread(void*);
void winclosethread(void*);
void deletethread(void*);
void	initcmd(void*);

char		*fontname;
int		mainpid;

enum
{
	New,
	Reshape,
	Move,
	Delete,
	Hide,
	Exit,
};

enum
{
	Cut,
	Paste,
	Snarf,
	Plumb,
	Send,
	Scroll,
};

int	Hidden = Exit+1;


char *rcargv[] = { "rc", "-i", nil };
char *kbdargv[] = { "rc", "-c", nil, nil };

int errorshouldabort = 0;

Console HardwareConsole = {
	.in = 0,
	.out = 1,
};

void
usage(void)
{
	print_func_entry();
	fprint(2, "usage: cmux [-k kbdcmd]\n");
	exits("usage");
	print_func_exit();
}

void
threadmain(int argc, char *argv[])
{
	char *kbdin, *s;
	char buf[256];

	kbdin = nil;
	maxtab = 0;
	ARGBEGIN{
	case 'k':
		if(kbdin != nil)
			usage();
		kbdin = ARGF();
		if(kbdin == nil)
			usage();
		break;
		break;
	}ARGEND;
	fmtinstall('W', winfmt);
	fmtinstall('f', fidfmt);
	mainpid = getpid();
	if(getwd(buf, sizeof buf) == nil)
		startdir = estrdup(".");
	else
		startdir = estrdup(buf);
	s = getenv("tabstop");
	if(s != nil)
		maxtab = strtol(s, nil, 0);
	if(maxtab == 0)
		maxtab = 4;
	free(s);

	if (0) {
		snarffd = open("/dev/snarf", OREAD|OCEXEC);
	} else {
		snarffd = -1;
	}

	if (0) {
//		mousectl = initmouse(nil, screen);
		if(mousectl == nil)
			error("can't find mouse");
		mouse = mousectl;
	}
	keyboardctl = initkeyboard(nil);
	if(keyboardctl == nil)
		error("can't find keyboard");

	exitchan = chancreate(sizeof(int), 0);
	winclosechan = chancreate(sizeof(Window*), 0);
	deletechan = chancreate(sizeof(char*), 0);

	timerinit();
	threadcreate(keyboardthread, nil, STACK);
	//threadcreate(mousethread, nil, STACK);
	filsys = filsysinit(xfidinit());

	if(filsys == nil)
		fprint(2, "cmux: can't create file system server: %r\n");
	else{
		errorshouldabort = 1;	/* suicide if there's trouble after this */
#if 0
//		proccreate(initcmd, nil, STACK);
		if(kbdin){
			kbdargv[2] = kbdin;
			//i = allocwindow(wscreen, r, Refbackup, DWhite);
			//wkeyboard = new(i, FALSE, scrolling, 0, nil, "/bin/rc", kbdargv);
			if(wkeyboard == nil)
				error("can't create keyboard window");
		}
#endif
		wkeyboard = new(&HardwareConsole, 0, nil, "/bin/rc", nil);
		if(wkeyboard == nil)
			error("can't create session leader console");
		wcurrent(wkeyboard);
		threadnotify(shutdown, 1);
		recv(exitchan, nil);
	}
	killprocs();
	threadexitsall(nil);
}

/*
 * /dev/snarf updates when the file is closed, so we must open our own
 * fd here rather than use snarffd
 */
void
putsnarf(void)
{
	print_func_entry();
	int fd, i, n;

	if(snarffd<0 || nsnarf==0) {
		print_func_exit();
		return;
	}
	fd = open("/dev/snarf", OWRITE);
	if(fd < 0) {
		print_func_exit();
		return;
	}
	/* snarf buffer could be huge, so fprint will truncate; do it in blocks */
	for(i=0; i<nsnarf; i+=n){
		n = nsnarf-i;
		if(n >= 256)
			n = 256;
		if(fprint(fd, "%.*S", n, snarf+i) < 0)
			break;
	}
	close(fd);
	print_func_exit();
}

void
getsnarf(void)
{
	print_func_entry();
	int i, n, nb, nulls;
	char *sn, buf[1024];

	if(snarffd < 0) {
		print_func_exit();
		return;
	}
	sn = nil;
	i = 0;
	seek(snarffd, 0, 0);
	while((n = read(snarffd, buf, sizeof buf)) > 0){
		sn = erealloc(sn, i+n+1);
		memmove(sn+i, buf, n);
		i += n;
		sn[i] = 0;
	}
	if(i > 0){
		snarf = runerealloc(snarf, i+1);
		cvttorunes(sn, i, snarf, &nb, &nsnarf, &nulls);
		free(sn);
	}
	print_func_exit();
}

void
initcmd(void *arg)
{
	print_func_entry();

	rfork(RFENVG|RFFDG|RFNOTEG|RFNAMEG);
	procexecl(nil, "/bin/rc", "rc", "-i", nil);
	fprint(2, "cmux: exec failed: %r\n");
	exits("exec");
	print_func_exit();
}

char *oknotes[] =
{
	"delete",
	"hangup",
	"kill",
	"exit",
	nil
};

int
shutdown(void * vacio, char *msg)
{
	print_func_entry();
	int i;
	static Lock shutdownlk;

	killprocs();
	for(i=0; oknotes[i]; i++)
		if(strncmp(oknotes[i], msg, strlen(oknotes[i])) == 0){
			lock(&shutdownlk);	/* only one can threadexitsall */
			threadexitsall(msg);
		}
	fprint(2, "cmux %d: abort: %s\n", getpid(), msg);
	abort();
	exits(msg);
	print_func_exit();
	return 0;
}

void
killprocs(void)
{
	print_func_entry();
	int i;

	for(i=0; i<nwindow; i++)
		postnote(PNGROUP, window[i]->pid, "hangup");
		print_func_exit();
}

void
keyboardthread(void* v)
{
	print_func_entry();
	Rune buf[2][20], *rp;
	int n, i;

	threadsetname("keyboardthread");
	n = 0;
	for(;;){
		rp = buf[n];
		n = 1-n;
		recv(keyboardctl->c, rp);
		for(i=1; i<nelem(buf[0])-1; i++)
			if(nbrecv(keyboardctl->c, rp+i) <= 0)
				break;
		rp[i] = L'\0';
		if(input != nil)
			sendp(input->ck, rp);
	}
	print_func_exit();
}

/*
 * Used by /dev/kbdin
 */
void
keyboardsend(char *s, int cnt)
{
	print_func_entry();
	Rune *r;
	int i, nb, nr;

	r = runemalloc(cnt);
	/* BUGlet: partial runes will be converted to error runes */
	cvttorunes(s, cnt, r, &nb, &nr, nil);
	for(i=0; i<nr; i++)
		send(keyboardctl->c, &r[i]);
	free(r);
	print_func_exit();
}

int
portion(int x, int lo, int hi)
{
	print_func_entry();
	x -= lo;
	hi -= lo;
	if(x < 20) {
		print_func_exit();
		return 0;
	}
	if(x > hi-20) {
		print_func_exit();
		return 2;
	}
	print_func_exit();
	return 1;
}

/* thread to allow fsysproc to synchronize window closing with main proc */
void
winclosethread(void* v)
{
	print_func_entry();
	Window *w;

	threadsetname("winclosethread");
	for(;;){
		w = recvp(winclosechan);
		wclose(w);
	}
	print_func_exit();
}

/* thread to make Deleted windows that the client still holds disappear offscreen after an interval */
void
deletethread(void* v)
{
	print_func_entry();
	char *s;


	threadsetname("deletethread");
	for(;;){
		s = recvp(deletechan);
//		i = namedimage(display, s);
//		freeimage(i);
		free(s);
	}
	print_func_exit();
}

void
deletetimeoutproc(void *v)
{
	print_func_entry();
	char *s;

	s = v;
	sleep(750);	/* remove window from screen after 3/4 of a second */
	sendp(deletechan, s);
	print_func_exit();
}

void
delete(void)
{
	print_func_entry();
//	Window *w;
	fprint(2, "can't delete!\n");
//	w = nil; //pointto(TRUE);
//	if(w)
//		wsendctlmesg(w, Deleted, ZR, nil);
	print_func_exit();
}

Window*
new(Console *i, int pid, char *dir, char *cmd, char **argv)
{
	print_func_entry();
	Window *w;
	Mousectl *mc = nil; // someday.
	Channel *cm, *ck, *cctl, *cpid;
	void **arg;

	if(i == nil) {
		print_func_exit();
		return nil;
	}
	cm = chancreate(sizeof(Mouse), 0);
	ck = chancreate(sizeof(Rune*), 0);
	cctl = chancreate(sizeof(Wctlmesg), 4);
	cpid = chancreate(sizeof(int), 0);
	if(cm==nil || ck==nil || cctl==nil)
		error("new: channel alloc failed");
	mc = emalloc(sizeof(Mousectl));
	// NO. *mc = *mousectl;
	// NO. mc->c = cm;
	w = wmk(mc, ck, cctl);
	free(mc);	/* wmk copies *mc */
	window = erealloc(window, ++nwindow*sizeof(Window*));

	window[nwindow-1] = w;
	threadcreate(winctl, w, 8192);
	if(pid == 0){
		arg = emalloc(5*sizeof(void*));
		arg[0] = w;
		arg[1] = cpid;
		arg[2] = cmd;
		if(argv == nil)
			arg[3] = rcargv;
		else
			arg[3] = argv;
		arg[4] = dir;
		proccreate(winshell, arg, 8192);
		pid = recvul(cpid);
		free(arg);
	}
	if(pid == 0){
		/* window creation failed */
		fprint(2, "not killing it\n"); //wsendctlmesg(w, Deleted, ZR, nil);
		chanfree(cpid);
		print_func_exit();
		return nil;
	}
	wsetpid(w, pid, 1);
	wsetname(w);
	if(dir)
		w->dir = estrdup(dir);
	chanfree(cpid);
	print_func_exit();
	return w;
}
