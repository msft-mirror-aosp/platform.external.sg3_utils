#define _XOPEN_SOURCE 500

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/poll.h>
#include <linux/major.h>
typedef unsigned char u_char;   /* horrible, for scsi.h */
#include "sg_include.h"
#include "sg_err.h"
#include "llseek.h"

/* A utility program for the Linux OS SCSI generic ("sg") device driver.
*  Copyright (C) 1999, 2000 D. Gilbert and P. Allworth
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.

   This program is a specialization of the Unix "dd" command in which
   one or both of the given files is a scsi generic device or a raw
   device. A block size ('bs') is assumed to be 512 if not given. This
   program complains if 'ibs' or 'obs' are given with some other value
   than 'bs'. If 'if' is not given or 'if=-' then stdin is assumed. If
   'of' is not given or 'of=-' then stdout assumed.  Multipliers:
      'c','C'  *1       'b','B' *512      'k' *1024      'K' *1000
      'm' *(1024^2)     'M' *(1000^2)     'g' *(1024^3)  'G' *(1000^3)
   
   A non-standard argument "bpt" (blocks per transfer) is added to control
   the maximum number of blocks in each transfer. The default value is 128.
   For example if "bs=512" and "bpt=32" then a maximum of 32 blocks (16KB
   in this case) are transferred to or from the sg device in a single SCSI
   command.

   This version should compile with Linux sg drivers with version numbers
   >= 30000 . This version uses posix threads.

*/

static char * version_str = "0.51 20010114";

#define DEF_BLOCK_SIZE 512
#define DEF_BLOCKS_PER_TRANSFER 128

/* #define SG_DEBUG */

#define SENSE_BUFF_LEN 32       /* Arbitrary, could be larger */
#define DEF_TIMEOUT 60000       /* 60,000 millisecs == 60 seconds */
#define S_RW_LEN 10             /* Use SCSI READ(10) and WRITE(10) */

#define SGP_READ10 0x28
#define SGP_WRITE10 0x2a
#define DEF_NUM_THREADS 4	/* actually degree of concurrency */
#define MAX_NUM_THREADS 32

#ifndef RAW_MAJOR
#define RAW_MAJOR 255   /*unlikey value */
#endif

#define FT_OTHER 0              /* filetype other than sg or raw device */
#define FT_SG 1                 /* filetype is sg char device */
#define FT_RAW 2                /* filetype is raw char device */

#define QS_IDLE 0		/* ready to start a copy cycle */
#define QS_IN_STARTED 1		/* commenced read */
#define QS_IN_FINISHED 2	/* finished read, ready for write */
#define QS_OUT_STARTED 3	/* commenced write */

#define QS_IN_POLL 11
#define QS_OUT_POLL 12

struct request_element;

typedef struct request_collection
{       /* one instance visible to all threads */
    int infd;
    int skip;
    int in_type;
    int in_scsi_type;
    int in_blk;                 /* next block address to read */
    int in_count;               /* blocks remaining for next read */
    int in_done_count;          /* count of completed in blocks */
    int in_partial;
    int outfd;
    int seek;
    int out_type;
    int out_scsi_type;
    int out_blk;                /* next block address to write */
    int out_count;              /* blocks remaining for next write */
    int out_done_count;         /* count of completed out blocks */
    int out_partial; 
    int bs;
    int bpt;
    int dio;
    int dio_incomplete; 
    int sum_of_resids;
    int coe;
    int debug;
    int num_rq_elems;
    struct request_element * req_arr;
} Rq_coll;

typedef struct request_element
{       /* one instance per worker thread */
    int qstate;			/* "QS" state */
    int infd;
    int outfd;
    int wr;
    int blk;
    int num_blks;
    unsigned char * buffp;
    unsigned char * alloc_bp;
    sg_io_hdr_t io_hdr;
    unsigned char cmd[S_RW_LEN];
    unsigned char sb[SENSE_BUFF_LEN];
    int bs;
    int dio;
    int dio_incomplete;
    int resid;
    int in_scsi_type;
    int out_scsi_type;
    int debug;
} Rq_elem;

static Rq_coll rcoll;
static struct pollfd in_pollfd_arr[MAX_NUM_THREADS];
static struct pollfd out_pollfd_arr[MAX_NUM_THREADS];
static int dd_count = -1;

int sg_fin_in_operation(Rq_coll * clp, Rq_elem * rep);
int sg_fin_out_operation(Rq_coll * clp, Rq_elem * rep);
int normal_in_operation(Rq_coll * clp, Rq_elem * rep, int blocks);
int normal_out_operation(Rq_coll * clp, Rq_elem * rep, int blocks);
int sg_start_io(Rq_elem * rep);
int sg_finish_io(int wr, Rq_elem * rep);


static void install_handler (int sig_num, void (*sig_handler) (int sig))
{
    struct sigaction sigact;
    sigaction (sig_num, NULL, &sigact);
    if (sigact.sa_handler != SIG_IGN)
    {
        sigact.sa_handler = sig_handler;
        sigemptyset (&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction (sig_num, &sigact, NULL);
    }
}

void print_stats()
{
    int infull, outfull;

    if (0 != rcoll.out_count)
        fprintf(stderr, "  remaining block count=%d\n", rcoll.out_count);
    infull = dd_count - rcoll.in_done_count - rcoll.in_partial;
    fprintf(stderr, "%d+%d records in\n", infull, rcoll.in_partial);
    outfull = dd_count - rcoll.out_done_count - rcoll.out_partial;
    fprintf(stderr, "%d+%d records out\n", outfull, rcoll.out_partial);
}

static void interrupt_handler(int sig)
{
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset (&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction (sig, &sigact, NULL);
    fprintf(stderr, "Interrupted by signal,");
    print_stats ();
    kill (getpid (), sig);
}

static void siginfo_handler(int sig)
{
    fprintf(stderr, "Progress report, continuing ...\n");
    print_stats ();
}

int dd_filetype(const char * filename)
{
    struct stat st;

    if (stat(filename, &st) < 0)
        return FT_OTHER;
    if (S_ISCHR(st.st_mode)) {
        if (RAW_MAJOR == major(st.st_rdev))
            return FT_RAW;
        else if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_SG;
    }
    return FT_OTHER;
}

void usage()
{
    fprintf(stderr, "Usage: "
           "sgq_dd  [if=<infile>] [skip=<n>] [of=<ofile>] [seek=<n>]\n"
           "               [bs=<num>] [bpt=<num>] [count=<n>]\n"
           "               [dio=<n>] [thr=<n>] [coe=<n>] [gen=<n>]\n"
           "               [deb=<n>] [--version]\n"
           "            usually either 'if' or 'of' is a sg or raw device\n"
           " 'bpt' is blocks_per_transfer (default is 128)\n"
           " 'dio' is direct IO, 1->attempt, 0->indirect IO (def)\n"
           " 'thr' is number of queues, must be > 0, default 4, max 32\n");
    fprintf(stderr, " 'coe' continue on sg error, 0->exit (def), "
    	   "1->zero + continue\n"
           " 'gen' 0-> 1 file is special(def), 1-> any files allowed\n"
           " 'deb' is debug, 0->none (def), > 0->varying degrees of debug\n");
}

/* Returns -1 for error, 0 for nothing found, QS_IN_POLL or QS_OUT_POLL */
int do_poll(Rq_coll * clp, int timeout, int * req_indexp)
{
    int k, res;

    if (FT_SG == clp->out_type) {
	while (((res = poll(out_pollfd_arr, clp->num_rq_elems, timeout)) < 0)
	       && (EINTR == errno))
	    ;
	if (res < 0) {
	    perror("poll error on output fds");
	    return -1;
	}
	else if (res > 0) {
	    for (k = 0; k < clp->num_rq_elems; ++k) {
		if (out_pollfd_arr[k].revents & POLLIN) {
		    if (req_indexp)
			*req_indexp = k;
		    return QS_OUT_POLL;
		}
	    }
	}
    }
    if (FT_SG == clp->in_type) {
	while (((res = poll(in_pollfd_arr, clp->num_rq_elems, timeout)) < 0)
	       && (EINTR == errno))
	    ;
	if (res < 0) {
	    perror("poll error on input fds");
	    return -1;
	}
	else if (res > 0) {
	    for (k = 0; k < clp->num_rq_elems; ++k) {
		if (in_pollfd_arr[k].revents & POLLIN) {
		    if (req_indexp)
			*req_indexp = k;
		    return QS_IN_POLL;
		}
	    }
	}
    }
    return 0;
}


/* Return of 0 -> success, -1 -> failure, 2 -> try again */
int read_capacity(int sg_fd, int * num_sect, int * sect_sz)
{
    int res;
    unsigned char rcCmdBlk [10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char rcBuff[64];
    unsigned char sense_b[64];
    sg_io_hdr_t io_hdr;

    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = sizeof(rcCmdBlk);
    io_hdr.mx_sb_len = sizeof(sense_b);
    io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    io_hdr.dxfer_len = sizeof(rcBuff);
    io_hdr.dxferp = rcBuff;
    io_hdr.cmdp = rcCmdBlk;
    io_hdr.sbp = sense_b;
    io_hdr.timeout = DEF_TIMEOUT;

    if (ioctl(sg_fd, SG_IO, &io_hdr) < 0) {
        perror("read_capacity (SG_IO) error");
        return -1;
    }
    res = sg_err_category3(&io_hdr);
    if (SG_ERR_CAT_MEDIA_CHANGED == res)
        return 2; /* probably have another go ... */
    else if (SG_ERR_CAT_CLEAN != res) {
        sg_chk_n_print3("read capacity", &io_hdr);
        return -1;
    }
    *num_sect = 1 + ((rcBuff[0] << 24) | (rcBuff[1] << 16) |
                (rcBuff[2] << 8) | rcBuff[3]);
    *sect_sz = (rcBuff[4] << 24) | (rcBuff[5] << 16) |
               (rcBuff[6] << 8) | rcBuff[7];
#ifdef SG_DEBUG
    fprintf(stderr, "number of sectors=%d, sector size=%d\n",
            *num_sect, *sect_sz);
#endif
    return 0;
}

/* 0 -> ok, 1 -> short read, -1 -> error */
int normal_in_operation(Rq_coll * clp, Rq_elem * rep, int blocks)
{
    int res;
    int stop_after_write = 0;

    rep->qstate = QS_IN_STARTED;
    if (rep->debug > 8) 
        fprintf(stderr, "normal_in_operation: start blk=%d num_blks=%d\n",
		rep->blk, rep->num_blks);
    while (((res = read(rep->infd, rep->buffp,
                        blocks * rep->bs)) < 0) && (EINTR == errno))
        ;
    if (res < 0) {
        fprintf(stderr, "sgq_dd: reading, in_blk=%d, errno=%d\n", rep->blk, 
		errno);
        return -1;
    }
    if (res < blocks * rep->bs) {
        int o_blocks = blocks;
        stop_after_write = 1;
        blocks = res / rep->bs;
        if ((res % rep->bs) > 0) {
            blocks++;
            clp->in_partial++;
        }
        /* Reverse out + re-apply blocks on clp */
        clp->in_blk -= o_blocks;
        clp->in_count += o_blocks;
        rep->num_blks = blocks;
        clp->in_blk += blocks;
        clp->in_count -= blocks;
    }
    clp->in_done_count -= blocks;
    rep->qstate = QS_IN_FINISHED;
    return stop_after_write;
}

/* 0 -> ok, -1 -> error */
int normal_out_operation(Rq_coll * clp, Rq_elem * rep, int blocks)
{
    int res;

    rep->qstate = QS_OUT_STARTED;
    if (rep->debug > 8) 
        fprintf(stderr, "normal_out_operation: start blk=%d num_blks=%d\n",
		rep->blk, rep->num_blks);
    while (((res = write(rep->outfd, rep->buffp,
                 rep->num_blks * rep->bs)) < 0) && (EINTR == errno))
        ;
    if (res < 0) {
        fprintf(stderr, "sgq_dd: output, out_blk=%d, errno=%d\n", rep->blk,
		errno);
        return -1;
    }
    if (res < blocks * rep->bs) {
        blocks = res / rep->bs;
        if ((res % rep->bs) > 0) {
            blocks++;
            clp->out_partial++;
        }
        rep->num_blks = blocks;
    }
    clp->out_done_count -= blocks;
    rep->qstate = QS_IDLE;
    return 0;
}

/* Returns 1 for retryable, 0 for ok, -ve for error */
int sg_fin_in_operation(Rq_coll * clp, Rq_elem * rep)
{
    int res;

    rep->qstate = QS_IN_FINISHED;
    res = sg_finish_io(rep->wr, rep);
    if (res < 0) {
	if (clp->coe) {
	    memset(rep->buffp, 0, rep->num_blks * rep->bs);
	    fprintf(stderr, ">> substituted zeros for in blk=%d for "
		    "%d bytes\n", rep->blk, rep->num_blks * rep->bs);
	    res = 0;
	}
	else {
	    fprintf(stderr, "error finishing sg in command\n");
	    return res;
	}
    }
    if (0 == res) { /* looks good, going to return */
	if (rep->dio_incomplete || rep->resid) {
	    clp->dio_incomplete += rep->dio_incomplete;
	    clp->sum_of_resids += rep->resid;
	}
	clp->in_done_count -= rep->num_blks;
    }
    return res;
}

/* Returns 1 for retryable, 0 for ok, -ve for error */
int sg_fin_out_operation(Rq_coll * clp, Rq_elem * rep)
{
    int res;

    rep->qstate = QS_IDLE;
    res = sg_finish_io(rep->wr, rep);
    if (res < 0) {
	if (clp->coe) {
	    fprintf(stderr, ">> ignored error for out blk=%d for "
		    "%d bytes\n", rep->blk, rep->num_blks * rep->bs);
	    res = 0;
	}
	else {
	    fprintf(stderr, "error finishing sg out command\n");
	    return res;
	}
    }
    if (0 == res) {
	if (rep->dio_incomplete || rep->resid) {
	    clp->dio_incomplete += rep->dio_incomplete;
	    clp->sum_of_resids += rep->resid;
	}
	clp->out_done_count -= rep->num_blks;
    }
    return res;
}

int sg_start_io(Rq_elem * rep)
{
    sg_io_hdr_t * hp = &rep->io_hdr;
    int res;

    rep->qstate = rep->wr ? QS_OUT_STARTED : QS_IN_STARTED;
    memset(rep->cmd, 0, sizeof(rep->cmd));
    rep->cmd[0] = rep->wr ? SGP_WRITE10 : SGP_READ10;
    rep->cmd[2] = (unsigned char)((rep->blk >> 24) & 0xFF);
    rep->cmd[3] = (unsigned char)((rep->blk >> 16) & 0xFF);
    rep->cmd[4] = (unsigned char)((rep->blk >> 8) & 0xFF);
    rep->cmd[5] = (unsigned char)(rep->blk & 0xFF);
    rep->cmd[7] = (unsigned char)((rep->num_blks >> 8) & 0xff);
    rep->cmd[8] = (unsigned char)(rep->num_blks & 0xff);
    memset(hp, 0, sizeof(sg_io_hdr_t));
    hp->interface_id = 'S';
    hp->cmd_len = sizeof(rep->cmd);
    hp->cmdp = rep->cmd;
    hp->dxfer_direction = rep->wr ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV;
    hp->dxfer_len = rep->bs * rep->num_blks;
    hp->dxferp = rep->buffp;
    hp->mx_sb_len = sizeof(rep->sb);
    hp->sbp = rep->sb;
    hp->timeout = DEF_TIMEOUT;
    hp->usr_ptr = rep;
    hp->pack_id = rep->blk;
    if (rep->dio)
        hp->flags |= SG_FLAG_DIRECT_IO;
    if (rep->debug > 8) {
        fprintf(stderr, "sg_start_io: SCSI %s, blk=%d num_blks=%d\n",
               rep->wr ? "WRITE" : "READ", rep->blk, rep->num_blks);
        sg_print_command(hp->cmdp);
        fprintf(stderr, "dir=%d, len=%d, dxfrp=%p, cmd_len=%d\n",
                hp->dxfer_direction, hp->dxfer_len, hp->dxferp, hp->cmd_len);
    }

    while (((res = write(rep->wr ? rep->outfd : rep->infd, hp,
                         sizeof(sg_io_hdr_t))) < 0) && (EINTR == errno))
        ;
    if (res < 0) {
        if (ENOMEM == errno)
            return 1;
        return res;
    }
    return 0;
}

/* -1 -> unrecoverable error, 0 -> successful, 1 -> try again */
int sg_finish_io(int wr, Rq_elem * rep)
{
    int res;
    sg_io_hdr_t io_hdr;
    sg_io_hdr_t * hp;
#if 0
    static int testing = 0;     /* thread dubious! */
#endif

    memset(&io_hdr, 0 , sizeof(sg_io_hdr_t));
    /* FORCE_PACK_ID active set only read packet with matching pack_id */
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = rep->wr ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV;
    io_hdr.pack_id = rep->blk;

    while (((res = read(wr ? rep->outfd : rep->infd, &io_hdr,
                        sizeof(sg_io_hdr_t))) < 0) && (EINTR == errno))
        ;
    if (res < 0) {
        perror("finishing io on sg device, error");
        return -1;
    }
    if (rep != (Rq_elem *)io_hdr.usr_ptr) {
        fprintf(stderr, 
		"sg_finish_io: bad usr_ptr, request-response mismatch\n");
	exit(1);
    }
    memcpy(&rep->io_hdr, &io_hdr, sizeof(sg_io_hdr_t));
    hp = &rep->io_hdr;

    switch (sg_err_category3(hp)) {
        case SG_ERR_CAT_CLEAN:
            break;
        case SG_ERR_CAT_RECOVERED:
            fprintf(stderr, "Recovered error on block=%d, num=%d\n",
                    rep->blk, rep->num_blks);
            break;
        case SG_ERR_CAT_MEDIA_CHANGED:
            return 1;
        default:
            {
                char ebuff[64];
                sprintf(ebuff, "%s blk=%d", rep->wr ? "writing": "reading",
                        rep->blk);
                sg_chk_n_print3(ebuff, hp);
                return -1;
            }
    }
#if 0
    if (0 == (++testing % 100)) return -1;
#endif
    if (rep->dio &&
        ((hp->info & SG_INFO_DIRECT_IO_MASK) != SG_INFO_DIRECT_IO))
        rep->dio_incomplete = 1; /* count dios done as indirect IO */
    else
        rep->dio_incomplete = 0;
    rep->resid = hp->resid;
    if (rep->debug > 8)
        fprintf(stderr, "sg_finish_io: completed %s\n", wr ? "WRITE" : "READ");
    return 0;
}

/* Returns scsi_type or -1 for error */
int sg_prepare(int fd, int sz)
{
    int res, t;
    struct sg_scsi_id info;

    res = ioctl(fd, SG_GET_VERSION_NUM, &t);
    if ((res < 0) || (t < 30000)) {
        fprintf(stderr, "sgq_dd: sg driver prior to 3.x.y\n");
        return -1;
    }
    res = ioctl(fd, SG_SET_RESERVED_SIZE, &sz);
    if (res < 0) 
        perror("sgq_dd: SG_SET_RESERVED_SIZE error");
#if 0
    t = 1;
    res = ioctl(fd, SG_SET_FORCE_PACK_ID, &t);
    if (res < 0)
        perror("sgq_dd: SG_SET_FORCE_PACK_ID error");
#endif
    res = ioctl(fd, SG_GET_SCSI_ID, &info);
    if (res < 0) {
	perror("sgq_dd: SG_SET_SCSI_ID error");
	return -1;
    }
    else
        return info.scsi_type;
}

/* Return 0 for ok, anything else for errors */
int prepare_rq_elems(Rq_coll * clp, const char * inf, const char * outf)
{
    int k;
    Rq_elem * rep;
    size_t psz;
    char ebuff[256];
    int sz = clp->bpt * clp->bs;
    int scsi_type;

    clp->req_arr = malloc(sizeof(Rq_elem) * clp->num_rq_elems);
    if (NULL == clp->req_arr)
    	return 1;
    for (k = 0; k < clp->num_rq_elems; ++k) {
    	rep = &clp->req_arr[k];
	memset(rep, 0, sizeof(Rq_elem));
	psz = getpagesize();
	if (NULL == (rep->alloc_bp = malloc(sz + psz)))
	    return 1;
	rep->buffp = (unsigned char *)
		(((unsigned long)rep->alloc_bp + psz - 1) & (~(psz - 1)));
	rep->qstate = QS_IDLE;
	rep->bs = clp->bs;
	rep->dio = clp->dio;
	rep->debug = clp->debug;
	rep->out_scsi_type = clp->out_scsi_type;
	if (FT_SG == clp->in_type) {
	    if (0 == k)
		rep->infd = clp->infd;
	    else {
		if ((rep->infd = open(inf, O_RDWR)) < 0) {
                    sprintf(ebuff, "sgq_dd: could not open %s for sg reading",
                            inf);
                    perror(ebuff);
                    return 1;
                }
	    }
	    in_pollfd_arr[k].fd = rep->infd;
	    in_pollfd_arr[k].events = POLLIN;
	    if ((scsi_type = sg_prepare(rep->infd, sz)) < 0)
		return 1;
	    if (0 == k)
		clp->in_scsi_type = scsi_type;
	    rep->in_scsi_type = clp->in_scsi_type;
	}
	else
	    rep->infd = clp->infd;

	if (FT_SG == clp->out_type) {
	    if (0 == k)
		rep->outfd = clp->outfd;
	    else {
		if ((rep->outfd = open(outf, O_RDWR)) < 0) {
                    sprintf(ebuff, "sgq_dd: could not open %s for sg writing",
                            outf);
                    perror(ebuff);
                    return 1;
                }
	    }
	    out_pollfd_arr[k].fd = rep->outfd;
	    out_pollfd_arr[k].events = POLLIN;
	    if ((scsi_type = sg_prepare(rep->outfd, sz)) < 0)
		return 1;
	    if (0 == k)
		clp->out_scsi_type = scsi_type;
	    rep->out_scsi_type = clp->out_scsi_type;
	}
	else
	    rep->outfd = clp->outfd;
    }
    return 0;
}

/* Returns a "QS" code and req index, or QS_IDLE and position of first idle
   (-1 if no idle position). Returns -1 on poll error. */
int decider(Rq_coll * clp, int first_xfer, int * req_indexp)
{
    int k, res;
    Rq_elem * rep;
    int first_idle_index = -1;
    int lowest_blk_index = -1;
    int times;
    int try_poll = 0;
    int lowest_blk = INT_MAX;

    times = first_xfer ? 1 : clp->num_rq_elems;
    for (k = 0; k < times; ++k) {
	rep = &clp->req_arr[k];
	if ((QS_IN_STARTED == rep->qstate) ||
	    (QS_OUT_STARTED == rep->qstate))
	    try_poll = 1;
	else if ((QS_IN_FINISHED == rep->qstate) && (rep->blk < lowest_blk)) {
	    lowest_blk = rep->blk;
	    lowest_blk_index = k;
	}
	else if ((QS_IDLE == rep->qstate) && (first_idle_index < 0))
	    first_idle_index = k;
    }
    if (try_poll) {
	res = do_poll(clp, 0, req_indexp);
	if (0 != res)
	    return res;
    }

    if (lowest_blk_index >= 0) {
	if (req_indexp)
	    *req_indexp = lowest_blk_index;
	return QS_IN_FINISHED;
    }
    if (req_indexp)
    	*req_indexp = first_idle_index;
    return QS_IDLE;
}

int get_num(char * buf)
{
    int res, num;
    char c;

    res = sscanf(buf, "%d%c", &num, &c);
    if (0 == res)
        return -1;
    else if (1 == res)
        return num;
    else {
        switch (c) {
        case 'c':
        case 'C':
            return num;
        case 'b':
        case 'B':
            return num * 512;
        case 'k':
            return num * 1024;
        case 'K':
            return num * 1000;
        case 'm':
            return num * 1024 * 1024;
        case 'M':
            return num * 1000000;
        case 'g':
            return num * 1024 * 1024 * 1024;
        case 'G':
            return num * 1000000000;
        default:
            fprintf(stderr, "unrecognized multiplier\n");
            return -1;
        }
    }
}


int main(int argc, char * argv[])
{
    int skip = 0;
    int seek = 0;
    int ibs = 0;
    int obs = 0;
    char str[512];
    char * key;
    char * buf;
    char inf[512];
    char outf[512];
    int res, k;
    int in_num_sect = 0;
    int out_num_sect = 0;
    int num_threads = DEF_NUM_THREADS;
    int gen = 0;
    int in_sect_sz, out_sect_sz, first_xfer, qstate, req_index, seek_skip;
    int blocks, stop_after_write, terminate;
    char ebuff[256];
    Rq_elem * rep;

    memset(&rcoll, 0, sizeof(Rq_coll));
    rcoll.bpt = DEF_BLOCKS_PER_TRANSFER;
    rcoll.in_type = FT_OTHER;
    rcoll.out_type = FT_OTHER;
    inf[0] = '\0';
    outf[0] = '\0';
    if (argc < 2) {
        usage();
        return 1;
    }

    for(k = 1; k < argc; k++) {
        if (argv[k])
            strcpy(str, argv[k]);
        else
            continue;
        for(key = str, buf = key; *buf && *buf != '=';)
            buf++;
        if (*buf)
            *buf++ = '\0';
        if (strcmp(key,"if") == 0)
            strcpy(inf, buf);
        else if (strcmp(key,"of") == 0)
            strcpy(outf, buf);
        else if (0 == strcmp(key,"ibs"))
            ibs = get_num(buf);
        else if (0 == strcmp(key,"obs"))
            obs = get_num(buf);
        else if (0 == strcmp(key,"bs"))
            rcoll.bs = get_num(buf);
        else if (0 == strcmp(key,"bpt"))
            rcoll.bpt = get_num(buf);
        else if (0 == strcmp(key,"skip"))
            skip = get_num(buf);
        else if (0 == strcmp(key,"seek"))
            seek = get_num(buf);
        else if (0 == strcmp(key,"count"))
            dd_count = get_num(buf);
        else if (0 == strcmp(key,"dio"))
            rcoll.dio = get_num(buf);
        else if (0 == strcmp(key,"thr"))
            num_threads = get_num(buf);
        else if (0 == strcmp(key,"coe"))
            rcoll.coe = get_num(buf);
        else if (0 == strcmp(key,"gen"))
            gen = get_num(buf);
        else if (0 == strncmp(key,"deb", 3))
            rcoll.debug = get_num(buf);
        else if (0 == strncmp(key, "--vers", 6)) {
            fprintf(stderr, "sgq_dd for sg version 3 driver: %s\n", 
	    	    version_str);
            return 0;
        }
        else {
            fprintf(stderr, "Unrecognized argument '%s'\n", key);
            usage();
            return 1;
        }
    }
    if (rcoll.bs <= 0) {
        rcoll.bs = DEF_BLOCK_SIZE;
        fprintf(stderr, "Assume default 'bs' (block size) of %d bytes\n",
                rcoll.bs);
    }
    if ((ibs && (ibs != rcoll.bs)) || (obs && (obs != rcoll.bs))) {
        fprintf(stderr, "If 'ibs' or 'obs' given must be same as 'bs'\n");
        usage();
        return 1;
    }
    if ((skip < 0) || (seek < 0)) {
        fprintf(stderr, "skip and seek cannot be negative\n");
        return 1;
    }
    if ((num_threads < 1) || (num_threads > MAX_NUM_THREADS)) {
        fprintf(stderr, "too few or too many threads requested\n");
        usage();
        return 1;
    }
    if (rcoll.debug)
        fprintf(stderr, "sgq_dd: if=%s skip=%d of=%s seek=%d count=%d\n",
               inf, skip, outf, seek, dd_count);
    install_handler (SIGINT, interrupt_handler);
    install_handler (SIGQUIT, interrupt_handler);
    install_handler (SIGPIPE, interrupt_handler);
    install_handler (SIGUSR1, siginfo_handler);

    rcoll.infd = STDIN_FILENO;
    rcoll.outfd = STDOUT_FILENO;
    if (inf[0] && ('-' != inf[0])) {
    	rcoll.in_type = dd_filetype(inf);

        if (FT_SG == rcoll.in_type) {
            if ((rcoll.infd = open(inf, O_RDWR)) < 0) {
                sprintf(ebuff, "sgq_dd: could not open %s for sg reading", 
			inf);
                perror(ebuff);
                return 1;
            }
        }
        if (FT_SG != rcoll.in_type) {
            if ((rcoll.infd = open(inf, O_RDONLY)) < 0) {
                sprintf(ebuff, "sgq_dd: could not open %s for reading", inf);
                perror(ebuff);
                return 1;
            }
            else if (skip > 0) {
                llse_loff_t offset = skip;

                offset *= rcoll.bs;       /* could exceed 32 here! */
                if (llse_llseek(rcoll.infd, offset, SEEK_SET) < 0) {
                    sprintf(ebuff,
                "sgq_dd: couldn't skip to required position on %s", inf);
                    perror(ebuff);
                    return 1;
                }
            }
        }
    }
    if (outf[0] && ('-' != outf[0])) {
	rcoll.out_type = dd_filetype(outf);

        if (FT_SG == rcoll.out_type) {
	    if ((rcoll.outfd = open(outf, O_RDWR)) < 0) {
                sprintf(ebuff, 
			"sgq_dd: could not open %s for sg writing", outf);
                perror(ebuff);
                return 1;
            }
        }
	else {
	    if (FT_OTHER == rcoll.out_type) {
		if ((rcoll.outfd = open(outf, O_WRONLY | O_CREAT, 0666)) < 0) {
                    sprintf(ebuff,
                            "sgq_dd: could not open %s for writing", outf);
                    perror(ebuff);
                    return 1;
                }
	    }
	    else {
		if ((rcoll.outfd = open(outf, O_WRONLY)) < 0) {
                    sprintf(ebuff,
                            "sgq_dd: could not open %s for raw writing", outf);
                    perror(ebuff);
                    return 1;
                }
            }
            if (seek > 0) {
                llse_loff_t offset = seek;

                offset *= rcoll.bs;       /* could exceed 32 bits here! */
		if (llse_llseek(rcoll.outfd, offset, SEEK_SET) < 0) {
                    sprintf(ebuff,
                "sgq_dd: couldn't seek to required position on %s", outf);
                    perror(ebuff);
                    return 1;
                }
            }
	}
    }
    if ((STDIN_FILENO == rcoll.infd) && (STDOUT_FILENO == rcoll.outfd)) {
        fprintf(stderr, "Disallow both if and of to be stdin and stdout");
        return 1;
    }
    if ((FT_OTHER == rcoll.in_type) && (FT_OTHER == rcoll.out_type) && !gen) {
        fprintf(stderr, "Either 'if' or 'of' must be a sg or raw device\n");
        return 1;
    }
    if (0 == dd_count)
        return 0;
    else if (dd_count < 0) {
        if (FT_SG == rcoll.in_type) {
            res = read_capacity(rcoll.infd, &in_num_sect, &in_sect_sz);
            if (2 == res) {
                fprintf(stderr, "Unit attention, media changed(in), repeat\n");
                res = read_capacity(rcoll.infd, &in_num_sect, &in_sect_sz);
            }
            if (0 != res) {
                fprintf(stderr, "Unable to read capacity on %s\n", inf);
                in_num_sect = -1;
            }
            else {
                if (in_num_sect > skip)
                    in_num_sect -= skip;
            }
        }
        if (FT_SG == rcoll.out_type) {
            res = read_capacity(rcoll.outfd, &out_num_sect, &out_sect_sz);
            if (2 == res) {
                fprintf(stderr, "Unit attention, media changed(out), repeat\n");
                res = read_capacity(rcoll.outfd, &out_num_sect, &out_sect_sz);
            }
            if (0 != res) {
                fprintf(stderr, "Unable to read capacity on %s\n", outf);
                out_num_sect = -1;
            }
            else {
                if (out_num_sect > seek)
                    out_num_sect -= seek;
            }
        }
        if (in_num_sect > 0) {
            if (out_num_sect > 0)
                dd_count = (in_num_sect > out_num_sect) ? out_num_sect :
                                                       in_num_sect;
            else
                dd_count = in_num_sect;
        }
        else
            dd_count = out_num_sect;
    }
    if (rcoll.debug > 1)
        fprintf(stderr, "Start of loop, count=%d, in_num_sect=%d, "
                "out_num_sect=%d\n", dd_count, in_num_sect, out_num_sect);
    if (dd_count <= 0) {
        fprintf(stderr, "Couldn't calculate count, please give one\n");
        return 1;
    }

    rcoll.in_count = dd_count;
    rcoll.in_done_count = dd_count;
    rcoll.skip = skip;
    rcoll.in_blk = skip;
    rcoll.out_count = dd_count;
    rcoll.out_done_count = dd_count;
    rcoll.seek = seek;
    rcoll.out_blk = seek;

    if ((FT_SG == rcoll.in_type) || (FT_SG == rcoll.out_type))
    	rcoll.num_rq_elems = num_threads;
    else
    	rcoll.num_rq_elems = 1;
    if (prepare_rq_elems(&rcoll, inf, outf)) {
        fprintf(stderr, "Setup failure, perhaps no memory\n");
	return 1;
    }

    first_xfer = 1;
    stop_after_write = 0;
    terminate = 0;
    seek_skip =  rcoll.seek - rcoll.skip;
    while (rcoll.out_done_count > 0) { /* >>>>>>>>> main loop */
        req_index = -1;
	qstate = decider(&rcoll, first_xfer, &req_index);
	rep = (req_index < 0) ? NULL : (rcoll.req_arr + req_index);
	switch (qstate) {
	case QS_IDLE:
	    if ((NULL == rep) || (rcoll.in_count <= 0)) {
	    	/* usleep(1000); */
		do_poll(&rcoll, 10, NULL);
		break;
	    }
	    if (rcoll.debug > 8)
	    	fprintf(stderr, "    sgq_dd: non-sleeping QS_IDLE state, "
				"req_index=%d\n", req_index);
	    if (first_xfer >= 2)
	    	first_xfer = 0;
	    else if (1 == first_xfer)
	    	++first_xfer;
	    if (stop_after_write) {
		terminate = 1;
		break;
	    }
	    blocks = (rcoll.in_count > rcoll.bpt) ? rcoll.bpt : rcoll.in_count;
	    rep->wr = 0;
	    rep->blk = rcoll.in_blk;
	    rep->num_blks = blocks;
	    rcoll.in_blk += blocks;
	    rcoll.in_count -= blocks;

	    if (FT_SG == rcoll.in_type) {
	    	res = sg_start_io(rep);
		if (0 != res) {
		    if (1 == res)
			fprintf(stderr, "Out of memory starting sg io\n");
		    terminate = 1;
		}
	    }
	    else {
		res = normal_in_operation(&rcoll, rep, blocks);
		if (res < 0)
		    terminate = 1;
		else if (res > 0)
		    stop_after_write = 1;
	    }
	    break;
	case QS_IN_FINISHED:
	    if (rcoll.debug > 8)
	    	fprintf(stderr, "    sgq_dd: state is QS_IN_FINISHED, "
				"req_index=%d\n", req_index);
	    if ((rep->blk + seek_skip) != rcoll.out_blk) {
		/* if write would be out of sequence then wait */
		usleep(1000);
		break;
	    }
            rep->wr = 1;
            rep->blk = rcoll.out_blk;
	    blocks = rep->num_blks;
            rcoll.out_blk += blocks;
            rcoll.out_count -= blocks;

	    if (FT_SG == rcoll.out_type) {
	    	res = sg_start_io(rep);
		if (0 != res) {
		    if (1 == res)
			fprintf(stderr, "Out of memory starting sg io\n");
		    terminate = 1;
		}
	    }
	    else {
		if (normal_out_operation(&rcoll, rep, blocks) < 0)
		    terminate = 1;
	    }
	    break;
	case QS_IN_POLL:
	    if (rcoll.debug > 8)
	    	fprintf(stderr, "    sgq_dd: state is QS_IN_POLL, "
				"req_index=%d\n", req_index);
	    res = sg_fin_in_operation(&rcoll, rep);
	    if (res < 0)
	    	terminate = 1;
	    else if (res > 1) {
	    	if (first_xfer) {
		    /* only retry on first xfer */
		    if (0 != sg_start_io(rep))
		    	terminate = 1;
		}
		else
		    terminate = 1;
	    }
	    break;
	case QS_OUT_POLL:
	    if (rcoll.debug > 8)
	    	fprintf(stderr, "    sgq_dd: state is QS_OUT_POLL, "
				"req_index=%d\n", req_index);
	    res = sg_fin_out_operation(&rcoll, rep);
	    if (res < 0)
	    	terminate = 1;
	    else if (res > 1) {
	    	if (first_xfer) {
		    /* only retry on first xfer */
		    if (0 != sg_start_io(rep))
		    	terminate = 1;
		}
		else
		    terminate = 1;
	    }
	    break;
	default:
	    if (rcoll.debug > 8)
		fprintf(stderr, "    sgq_dd: state is ?????\n");
	    terminate = 1;
	    break;
	}
	if (terminate)
	    break;
    } /* >>>>>>>>>>>>> end of main loop */

    if (STDIN_FILENO != rcoll.infd)
        close(rcoll.infd);
    if (STDOUT_FILENO != rcoll.outfd)
        close(rcoll.outfd);
    res = 0;
    if (0 != rcoll.out_count) {
        fprintf(stderr, ">>>> Some error occurred,\n");
	res = 2;
    }
    print_stats();
    if (rcoll.dio_incomplete)
        fprintf(stderr, ">> Direct IO requested but incomplete %d times\n",
               rcoll.dio_incomplete);
    if (rcoll.sum_of_resids)
        fprintf(stderr, ">> Non-zero sum of residual counts=%d\n",
               rcoll.sum_of_resids);
    return res;
}