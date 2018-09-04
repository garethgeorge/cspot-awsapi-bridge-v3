#ifndef WPCMDS_H
#define WPCMDS_H

#include <linux/limits.h>
#include <lib/wp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
	Worker process commands etc
*/
struct ResultWooF {
	char woofname[256];
	int seqno;
};

struct wpcmd_initdir_arg {
	char dir[PATH_MAX];
};

extern int wpcmd_initdir(WP *wp, WPJob* job);

struct wpcmd_woofcreate_arg {
	int el_size;
	int queue_depth;
	char woofname[PATH_MAX];
};

extern int wpcmd_woofcreate(WP *wp, WPJob* job);

struct wpcmd_woofput_arg {
	char woofname[PATH_MAX];
	char handlername[256];
	char *payload; // remember, it must be placed in shared memory!
};

extern int wpcmd_woofput(WP *wp, WPJob* job);

struct wpcmd_waitforresult_arg {
	struct ResultWooF resultwoof;
	long timeout;
}; // no result struct, the result is just a buffer

extern int wpcmd_waitforresult(WP *wp, WPJob *job);

struct wpcmd_woofgetlatestseqno_arg {
	char woofname[PATH_MAX];
};

extern int wpcmd_woofgetlatestseqno(WP *wp, WPJob *job);

union wpcmd_job_data_types {
	struct wpcmd_initdir_arg wpcmd_initdir_arg;
	struct wpcmd_woofcreate_arg wpcmd_woofcreate_arg;
	struct wpcmd_woofput_arg wpcmd_woofput_arg;
	struct wpcmd_waitforresult_arg wpcmd_waitforresult_arg;
	struct wpcmd_woofgetlatestseqno_arg wpcmd_woofgetlatestseqno_arg;
};

extern WPHandler wphandler_array[];

#ifdef __cplusplus
}
#endif

#endif 