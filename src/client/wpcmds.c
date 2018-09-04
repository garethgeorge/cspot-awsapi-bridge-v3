#include <unistd.h>
#include <stdio.h>
#include <lib/wp.h>

#include "wpcmds.h"

#include "woofc.h"
#include "woofc-host.h"


/*
	Worker process commands etc
*/
extern int wpcmd_initdir(WP *wp, WPJob* job) {
	struct wpcmd_initdir_arg *arg = job->arg;
	fprintf(stdout, "Child process: chdir('%s')\n", arg->dir);
	if (chdir(arg->dir) != 0)
		return -1;
	return WooFInit();
}

extern int wpcmd_woofcreate(WP *wp, WPJob* job) {
	struct wpcmd_woofcreate_arg *arg = job->arg;
	fprintf(stdout, "Child process: WooFCreate('%s', %d, %d)\n", arg->woofname, arg->el_size, arg->queue_depth);
	int retval = WooFCreate(arg->woofname, arg->el_size, arg->queue_depth);
	fprintf(stdout, "Child process: \t woofcreate returned: %d\n", retval);
	return retval;
}

extern int wpcmd_woofput(WP *wp, WPJob* job) {
	struct wpcmd_woofput_arg *arg = job->arg;
	fprintf(stdout, "Child process: WooFPut('%s', '%s', %lx)\n", arg->woofname, arg->handlername, (unsigned long) arg->payload);
	return WooFPut(arg->woofname, arg->handlername, arg->payload);
}

extern int wpcmd_waitforresult(WP *wp, WPJob *job) {
	struct wpcmd_waitforresult_arg *arg = job->arg;
	char *resultbuffer = job->result;
	fprintf(stdout, "Child process: WaitForResult(%s)\n", arg->resultwoof.woofname);
	
	int seqno = 0;
	int startseqno = arg->resultwoof.seqno;
	long timeout = arg->timeout; // 30 seconds
	long sleep_time = 4L;

	while (timeout > 0 && (seqno = WooFGetLatestSeqno(arg->resultwoof.woofname)) == startseqno ) {
		if (nanosleep((const struct timespec[]){{0, sleep_time * 1000000L}}, NULL) != 0) {
			break ;
		}
		timeout -= sleep_time;
	}

	if (timeout < 0 || seqno == startseqno)
		return -1;

	WooFGet(arg->resultwoof.woofname, resultbuffer, seqno);
	return seqno;
}

extern int wpcmd_woofgetlatestseqno(WP *wp, WPJob *job) {
	struct wpcmd_woofgetlatestseqno_arg *arg = job->arg;
	return WooFGetLatestSeqno(arg->woofname);
}

WPHandler wphandler_array[] = {
	wpcmd_initdir,
	wpcmd_woofcreate,
	wpcmd_woofput, 
	wpcmd_waitforresult,
	wpcmd_woofgetlatestseqno,
	0
};