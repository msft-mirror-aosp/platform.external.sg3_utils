/*
 * A utility program for copying files. Specialised for "files" that
 * represent devices that understand the SCSI command set.
 *
 * Copyright (C) 2018-2022 D. Gilbert
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is a specialisation of the Unix "dd" command in which
 * one or both of the given files is a scsi generic device.
 * A logical block size ('bs') is assumed to be 512 if not given. This
 * program complains if 'ibs' or 'obs' are given with some other value
 * than 'bs'. If 'if' is not given or 'if=-' then stdin is assumed. If
 * 'of' is not given or 'of=-' then stdout assumed.
 *
 * A non-standard argument "bpt" (blocks per transfer) is added to control
 * the maximum number of blocks in each transfer. The default value is 128.
 * For example if "bs=512" and "bpt=32" then a maximum of 32 blocks (16 KiB
 * in this case) are transferred to or from the sg device in a single SCSI
 * command.
 *
 * This version is designed for the linux kernel 2.4, 2.6, 3, 4 and 5 series.
 *
 * sgp_dd is a Posix threads specialization of the sg_dd utility. Both
 * sgp_dd and sg_dd only perform special tasks when one or both of the given
 * devices belong to the Linux sg driver.
 *
 * sgh_dd further extends sgp_dd to use the experimental kernel buffer
 * sharing feature added in 3.9.02 .
 * N.B. This utility was previously called sgs_dd but there was already an
 * archived version of a dd variant called sgs_dd so this utility name was
 * renamed [20181221]
 */

static const char * version_str = "2.22 20221020";

#define _XOPEN_SOURCE 600
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#ifndef major
#include <sys/types.h>
#endif
#include <sys/time.h>
#include <linux/major.h>        /* for MEM_MAJOR, SCSI_GENERIC_MAJOR, etc */
#include <linux/fs.h>           /* for BLKSSZGET and friends */
#include <sys/mman.h>           /* for mmap() system call */

#include <vector>
#include <array>
#include <atomic>       // C++ header replacing <stdatomic.h> also link
                        // needed '-l atomic' . Not anymore??
#include <random>
#include <thread>       // needed for std::this_thread::yield()
#include <mutex>
#include <chrono>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_GETRANDOM
#include <sys/random.h>         /* for getrandom() system call */
#endif

#ifndef HAVE_LINUX_SG_V4_HDR
/* Kernel uapi header contain __user decorations on user space pointers
 * to indicate they are unsafe in the kernel space. However glibc takes
 * all those __user decorations out from headers in /usr/include/linux .
 * So to stop compile errors when directly importing include/uapi/scsi/sg.h
 * undef __user before doing that include. */
#define __user

/* Want to block the original sg.h header from also being included. That
 * causes lots of multiple definition errors. This will only work if this
 * header is included _before_ the original sg.h header.  */
#define _SCSI_GENERIC_H         /* original kernel header guard */
#define _SCSI_SG_H              /* glibc header guard */

#include "uapi_sg.h"    /* local copy of include/uapi/scsi/sg.h */

#else
#define __user
#endif  /* end of: ifndef HAVE_LINUX_SG_V4_HDR */

#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_io_linux.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"


using namespace std;

#ifdef __GNUC__
#ifndef  __clang__
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
#endif

/* comment out following line to stop ioctl(SG_CTL_FLAGM_SNAP_DEV) */
#define SGH_DD_SNAP_DEV 1

#ifndef SGV4_FLAG_POLLED
#define SGV4_FLAG_POLLED 0x800
#endif

#define DEF_BLOCK_SIZE 512
#define DEF_BLOCKS_PER_TRANSFER 128
#define DEF_BLOCKS_PER_2048TRANSFER 32
#define DEF_SDT_ICT_MS 300
#define DEF_SDT_CRT_SEC 3
#define DEF_SCSI_CDBSZ 10
#define MAX_SCSI_CDBSZ 16
#define MAX_BPT_VALUE (1 << 24)         /* used for maximum bs as well */
#define MAX_COUNT_SKIP_SEEK (1LL << 48) /* coverity wants upper bound */

#define SENSE_BUFF_LEN 64       /* Arbitrary, could be larger */
#define READ_CAP_REPLY_LEN 8
#define RCAP16_REPLY_LEN 32

#define DEF_TIMEOUT 60000       /* 60,000 millisecs == 60 seconds */

#define SGP_READ10 0x28
#define SGP_PRE_FETCH10 0x34
#define SGP_PRE_FETCH16 0x90
#define SGP_VERIFY10 0x2f
#define SGP_WRITE10 0x2a
#define DEF_NUM_THREADS 4
#define MAX_NUM_THREADS 1024 /* was SG_MAX_QUEUE with v3 driver */
#define DEF_NUM_MRQS 0

#define FT_OTHER 1              /* filetype other than one of the following */
#define FT_SG 2                 /* filetype is sg char device */
#define FT_DEV_NULL 4           /* either /dev/null, /dev/zero or "." */
#define FT_ST 8                 /* filetype is st char device (tape) */
#define FT_CHAR 16              /* filetype is st char device (tape) */
#define FT_BLOCK 32             /* filetype is a block device */
#define FT_FIFO 64              /* fifo (named or unnamed pipe (stdout)) */
#define FT_RANDOM_0_FF 128      /* iflag=00, iflag=ff and iflag=random
                                   override if=IFILE */
#define FT_ERROR 256            /* couldn't "stat" file */

#define DEV_NULL_MINOR_NUM 3
#define DEV_ZERO_MINOR_NUM 5

#define EBUFF_SZ 768

#define PROC_SCSI_SG_VERSION "/proc/scsi/sg/version"
#define SYS_SCSI_SG_VERSION "/sys/module/sg/version"

struct flags_t {
    bool append;
    bool coe;
    bool defres;        /* without this res_sz==bs*bpt */
    bool dio;
    bool direct;
    bool dpo;
    bool dsync;
    bool excl;
    bool ff;
    bool fua;
    bool polled;	/* formerly called 'hipri' */
    bool masync;        /* more async sg v4 driver flag */
    bool mrq_immed;     /* mrq submit non-blocking */
    bool mrq_svb;       /* mrq shared_variable_block, for sg->sg copy */
    bool no_dur;
    bool nocreat;
    bool noshare;
    bool no_thresh;
    bool no_unshare;    /* leave it for driver close/release */
    bool no_waitq;      /* dummy, no longer supported, just warn */
    bool noxfer;
    bool qhead;
    bool qtail;
    bool random;
    bool mout_if;       /* META_OUT_IF flag at mrq level */
    bool same_fds;
    bool swait;         /* now ignore; kept for backward compatibility */
    bool v3;
    bool v4;
    bool v4_given;
    bool wq_excl;
    bool zero;
    int mmap;
};

struct global_collection
{       /* one instance visible to all threads */
    int infd;
    int64_t skip;
    int in_type;
    int cdbsz_in;
    int help;
    int elem_sz;
    struct flags_t in_flags;
    // int64_t in_blk;                /* -\ next block address to read */
    // int64_t in_count;              /*  | blocks remaining for next read */
    atomic<int64_t> in_rem_count;     /*  | count of remaining in blocks */
    atomic<int> in_partial;           /*  | */
    atomic<bool> in_stop;             /*  | */
    off_t in_st_size;                 /* Only for FT_OTHER (regular) file */
    pthread_mutex_t in_mutex;         /* -/ */
    int nmrqs;                        /* Number of multi-reqs for sg v4 */
    int outfd;
    int64_t seek;
    int out_type;
    int out2fd;
    int out2_type;
    int cdbsz_out;
    int aen;                          /* abort every nth command */
    int m_aen;                        /* abort mrq every nth command */
    struct flags_t out_flags;
    atomic<int64_t> out_blk;          /* -\ next block address to write */
    atomic<int64_t> out_count;        /*  | blocks remaining for next write */
    atomic<int64_t> out_rem_count;    /*  | count of remaining out blocks */
    atomic<int> out_partial;          /*  | */
    atomic<bool> out_stop;            /*  | */
    off_t out_st_size;                /* Only for FT_OTHER (regular) file */
    pthread_mutex_t out_mutex;        /*  | */
    pthread_cond_t out_sync_cv;       /*  | hold writes until "in order" */
    pthread_mutex_t out2_mutex;
    int bs;
    int bpt;
    int cmd_timeout;            /* in milliseconds */
    int outregfd;
    int outreg_type;
    int ofsplit;
    atomic<int> dio_incomplete_count;
    atomic<int> sum_of_resids;
    uint32_t sdt_ict; /* stall detection; initial check time (milliseconds) */
    uint32_t sdt_crt; /* check repetition time (seconds), after first stall */
    int fail_mask;
    int verbose;
    int dry_run;
    int chkaddr;
    bool aen_given;
    bool cdbsz_given;
    bool is_mrq_i;
    bool is_mrq_o;
    bool m_aen_given;
    bool ofile_given;
    bool ofile2_given;
    bool unit_nanosec;          /* default duration unit is millisecond */
    bool mrq_cmds;              /* mrq=<NRQS>,C  given */
    bool mrq_async;             /* mrq_immed flag given */
    bool noshare;               /* don't use request sharing */
    bool unbalanced_mrq;        /* so _not_ sg->sg request sharing sync mrq */
    bool verify;                /* don't copy, verify like Unix: cmp */
    bool prefetch;              /* for verify: do PF(b),RD(a),V(b)_a_data */
    bool unshare;               /* let close() do file unshare operation */
    const char * infp;
    const char * outfp;
    const char * out2fp;
};

typedef struct mrq_abort_info
{
    int from_tid;
    int fd;
    int mrq_id;
    int debug;
} Mrq_abort_info;

typedef struct request_element
{       /* one instance per worker thread */
    struct global_collection *clp;
    bool wr;
    bool has_share;
    bool both_sg;
    bool same_sg;
    bool only_in_sg;
    bool only_out_sg;
    // bool mrq_abort_thread_active;
    int id;
    int bs;
    int infd;
    int outfd;
    int out2fd;
    int outregfd;
    int64_t iblk;
    int64_t oblk;
    int num_blks;
    uint8_t * buffp;
    uint8_t * alloc_bp;
    struct sg_io_hdr io_hdr;
    struct sg_io_v4 io_hdr4[2];
    uint8_t cmd[MAX_SCSI_CDBSZ];
    uint8_t sb[SENSE_BUFF_LEN];
    int dio_incomplete_count;
    int mmap_active;
    int resid;
    int rd_p_id;
    int rep_count;
    int rq_id;
    int mmap_len;
    int mrq_id;
    int mrq_index;
    uint32_t in_mrq_q_blks;
    uint32_t out_mrq_q_blks;
    long seed;
#ifdef HAVE_SRAND48_R   /* gcc extension. N.B. non-reentrant version slower */
    struct drand48_data drand;/* opaque, used by srand48_r and mrand48_r */
#endif
    pthread_t mrq_abort_thread_id;
    Mrq_abort_info mai;
} Rq_elem;

typedef struct thread_info
{
    int id;
    struct global_collection * gcp;
    pthread_t a_pthr;
} Thread_info;

/* Additional parameters for sg_start_io() and sg_finish_io() */
struct sg_io_extra {
    bool is_wr2;
    bool prefetch;
    bool dout_is_split;
    int hpv4_ind;
    int blk_offset;
    int blks;
};

#define MONO_MRQ_ID_INIT 0x10000

// typedef vector< pair<int, struct sg_io_v4> > mrq_arr_t;
typedef array<uint8_t, 32> big_cdb;     /* allow up to a 32 byte cdb */
typedef pair< vector<struct sg_io_v4>, vector<big_cdb> >   mrq_arr_t;


/* Use this class to wrap C++11 <random> features to produce uniform random
 * unsigned ints in the range [lo, hi] (inclusive) given a_seed */
class Rand_uint {
public:
    Rand_uint(unsigned int lo, unsigned int hi, unsigned int a_seed)
        : uid(lo, hi), dre(a_seed) { }
    /* uid ctor takes inclusive range when integral type */

    unsigned int get() { return uid(dre); }

private:
    uniform_int_distribution<unsigned int> uid;
    default_random_engine dre;
};

static atomic<int> mono_pack_id(1);
static atomic<int> mono_mrq_id(MONO_MRQ_ID_INIT);
static atomic<long int> pos_index(0);

static atomic<int> num_ebusy(0);
static atomic<int> num_start_eagain(0);
static atomic<int> num_fin_eagain(0);
static atomic<int> num_abort_req(0);
static atomic<int> num_abort_req_success(0);
static atomic<int> num_mrq_abort_req(0);
static atomic<int> num_mrq_abort_req_success(0);
static atomic<int> num_miscompare(0);
static atomic<long> num_waiting_calls(0);
static atomic<bool> vb_first_time(true);
static atomic<bool> shutting_down(false);

static sigset_t signal_set;
static sigset_t orig_signal_set;
static pthread_t sig_listen_thread_id;

static const char * sg_allow_dio = "/sys/module/sg/parameters/allow_dio";

static void sg_in_rd_cmd(struct global_collection * clp, Rq_elem * rep,
                         mrq_arr_t & def_arr);
static void sg_out_wr_cmd(Rq_elem * rep, mrq_arr_t & def_arr, bool is_wr2,
                          bool prefetch);
static bool normal_in_rd(Rq_elem * rep, int blocks);
static void normal_out_wr(Rq_elem * rep, int blocks);
static int sg_start_io(Rq_elem * rep, mrq_arr_t & def_arr, int & pack_id,
                       struct sg_io_extra *xtrp);
static int sg_finish_io(bool wr, Rq_elem * rep, int pack_id,
                        struct sg_io_extra *xtrp);
static int sg_in_open(struct global_collection *clp, const char *inf,
                      uint8_t **mmpp, int *mmap_len);
static int sg_out_open(struct global_collection *clp, const char *outf,
                       uint8_t **mmpp, int *mmap_len);
static int sgh_do_deferred_mrq(Rq_elem * rep, mrq_arr_t & def_arr);

#define STRERR_BUFF_LEN 128

static pthread_mutex_t strerr_mut = PTHREAD_MUTEX_INITIALIZER;

static bool have_sg_version = false;
static int sg_version = 0;
static bool sg_version_lt_4 = false;
static bool sg_version_ge_40045 = false;
static bool do_sync = false;
static int do_time = 1;
static struct global_collection gcoll;
static struct timeval start_tm;
static int64_t dd_count = -1;
static int num_threads = DEF_NUM_THREADS;
static int exit_status = 0;
static bool after1 = false;

static const char * my_name = "sgh_dd: ";

static const char * mrq_blk_s = "mrq: ordinary blocking";
static const char * mrq_vb_s = "mrq: variable blocking";
static const char * mrq_svb_s = "mrq: shared variable blocking (svb)";
static const char * mrq_s_nb_s = "mrq: submit of full non-blocking";


#ifdef __GNUC__
static int pr2serr_lk(const char * fmt, ...)
        __attribute__ ((format (printf, 1, 2)));
#if 0
static void pr_errno_lk(int e_no, const char * fmt, ...)
        __attribute__ ((format (printf, 2, 3)));
#endif
#else
static int pr2serr_lk(const char * fmt, ...);
#if 0
static void pr_errno_lk(int e_no, const char * fmt, ...);
#endif
#endif


static int
pr2serr_lk(const char * fmt, ...)
{
    int n;
    va_list args;

    pthread_mutex_lock(&strerr_mut);
    va_start(args, fmt);
    n = vfprintf(stderr, fmt, args);
    va_end(args);
    pthread_mutex_unlock(&strerr_mut);
    return n;
}

static void
usage(int pg_num)
{
    if (pg_num > 3)
        goto page4;
    else if (pg_num > 2)
        goto page3;
    else if (pg_num > 1)
        goto page2;

    pr2serr("Usage: sgh_dd  [bs=BS] [conv=CONVS] [count=COUNT] [ibs=BS] "
            "[if=IFILE]\n"
            "               [iflag=FLAGS] [obs=BS] [of=OFILE] [oflag=FLAGS] "
            "[seek=SEEK]\n"
            "               [skip=SKIP] [--help] [--version]\n\n");
    pr2serr("               [ae=AEN[,MAEN]] [bpt=BPT] [cdbsz=6|10|12|16] "
            "[coe=0|1]\n"
            "               [dio=0|1] [elemsz_kb=EKB] [fail_mask=FM] "
            "[fua=0|1|2|3]\n"
            "               [mrq=[I|O,]NRQS[,C]] [noshare=0|1] "
            "[of2=OFILE2]\n"
            "               [ofreg=OFREG] [ofsplit=OSP] [sdt=SDT] "
            "[sync=0|1]\n"
            "               [thr=THR] [time=0|1|2[,TO]] [unshare=1|0] "
            "[verbose=VERB]\n"
            "               [--dry-run] [--prefetch] [-v|-vv|-vvv] "
            "[--verbose]\n"
            "               [--verify] [--version]\n\n"
            "  where the main options (shown in first group above) are:\n"
            "    bs          must be device logical block size (default "
            "512)\n"
            "    conv        comma separated list from: [nocreat,noerror,"
            "notrunc,\n"
            "                null,sync]\n"
            "    count       number of blocks to copy (def: device size)\n"
            "    if          file or device to read from (def: stdin)\n"
            "    iflag       comma separated list from: [00,coe,defres,dio,"
            "direct,dpo,\n"
            "                dsync,excl,ff,fua,masync,mmap,mout_if,"
            "mrq_immed,mrq_svb,\n"
            "                nocreat,nodur,noxfer,null,polled,qhead,"
            "qtail,\n"
            "                random,same_fds,v3,v4,wq_excl]\n"
            "    of          file or device to write to (def: /dev/null "
            "N.B. different\n"
            "                from dd it defaults to stdout). If 'of=.' "
            "uses /dev/null\n"
            "    of2         second file or device to write to (def: "
            "/dev/null)\n"
            "    oflag       comma separated list from: [append,<<list from "
            "iflag>>]\n"
            "    seek        block position to start writing to OFILE\n"
            "    skip        block position to start reading from IFILE\n"
            "    --help|-h      output this usage message then exit\n"
            "    --verify|-x    do a verify (compare) operation [def: do a "
            "copy]\n"
            "    --version|-V   output version string then exit\n\n"
            "Copy IFILE to OFILE, similar to dd command. This utility is "
            "specialized for\nSCSI devices and uses multiple POSIX threads. "
            "It expects one or both IFILE\nand OFILE to be sg devices. With "
            "--verify option does a verify/compare\noperation instead of a "
            "copy. This utility is Linux specific and uses the\nv4 sg "
            "driver 'share' capability if available. Use '-hh', '-hhh' or "
            "'-hhhh'\nfor more information.\n"
           );
    return;
page2:
    pr2serr("Syntax:  sgh_dd [operands] [options]\n\n"
            "  where: operands have the form name=value and are pecular to "
            "'dd'\n"
            "         style commands, and options start with one or "
            "two hyphens;\n"
            "         the lesser used operands and option are:\n\n"
            "    ae          AEN: abort every n commands (def: 0 --> don't "
            "abort any)\n"
            "                MAEN: abort every n mrq commands (def: 0 --> "
            "don't)\n"
            "                [requires commands with > 1 ms duration]\n"
            "    bpt         is blocks_per_transfer (default is 128)\n"
            "    cdbsz       size of SCSI READ, WRITE or VERIFY cdb_s "
            "(default is 10)\n"
            "    coe         continue on error, 0->exit (def), "
            "1->zero + continue\n"
            "    dio         is direct IO, 1->attempt, 0->indirect IO (def)\n"
            "    elemsz_kb    scatter gather list element size in kilobytes "
            "(def: 32[KB])\n"
            "    fail_mask    1: misuse KEEP_SHARE flag; 0: nothing (def)\n"
            "    fua         force unit access: 0->don't(def), 1->OFILE, "
            "2->IFILE,\n"
            "                3->OFILE+IFILE\n"
            "    mrq         number of cmds placed in each sg call "
            "(def: 0);\n"
            "                may have trailing ',C', to send bulk cdb_s; "
            "if preceded\n"
            "                by 'I' then mrq only on IFILE, likewise 'O' "
            "for OFILE\n"
            "    noshare     0->use request sharing(def), 1->don't\n"
            "    ofreg       OFREG is regular file or pipe to send what is "
            "read from\n"
            "                IFILE in the first half of each shared element\n"
            "    ofsplit     split ofile write in two at block OSP (def: 0 "
            "(no split))\n"
            "    sdt         stall detection times: CRT[,ICT]. CRT: check "
            "repetition\n"
            "                time (after first) in seconds; ICT: initial "
            "check time\n"
            "                in milliseconds. Default: 3,300 . Use CRT=0 "
            "to disable\n"
            "    sync        0->no sync(def), 1->SYNCHRONIZE CACHE on OFILE "
            "after copy\n"
            "    thr         is number of threads, must be > 0, default 4, "
            "max 1024\n"
            "    time        0->no timing, 1->calc throughput(def), "
            "2->nanosec\n"
            "                precision; TO is command timeout in seconds "
            "(def: 60)\n"
            "    unshare     0->don't explicitly unshare after share; 1->let "
            "close do\n"
            "                file unshare (default)\n"
            "    verbose     increase verbosity\n"
            "    --chkaddr|-c    exits if read block does not contain "
            "32 bit block\n"
            "                    address, used once only checks first "
            "address in block\n"
            "    --dry-run|-d    prepare but bypass copy/read\n"
            "    --prefetch|-p    with verify: do pre-fetch first\n"
            "    --verbose|-v   increase verbosity of utility\n\n"
            "Use '-hhh' or '-hhhh' for more information about flags.\n"
           );
    return;
page3:
    pr2serr("Syntax:  sgh_dd [operands] [options]\n\n"
            "  where: 'iflag=<arg>' and 'oflag=<arg>' arguments are listed "
            "below:\n\n"
            "    00          use all zeros instead of if=IFILE (only in "
            "iflags)\n"
            "    00,ff       generates blocks that contain own (32 bit be) "
            "blk address\n"
            "    append      append output to OFILE (assumes OFILE is "
            "regular file)\n"
            "    coe         continue of error (reading, fills with zeros)\n"
            "    defres      keep default reserve buffer size (else its "
            "bs*bpt)\n"
            "    dio         sets the SG_FLAG_DIRECT_IO in sg requests\n"
            "    direct      sets the O_DIRECT flag on open()\n"
            "    dpo         sets the DPO (disable page out) in SCSI READs "
            "and WRITEs\n"
            "    dsync       sets the O_SYNC flag on open()\n"
            "    excl        sets the O_EXCL flag on open()\n"
            "    ff          use all 0xff bytes instead of if=IFILE (only in "
            "iflags)\n"
            "    fua         sets the FUA (force unit access) in SCSI READs "
            "and WRITEs\n"
            "    hipri       same as 'polled'; 'hipri' name is deprecated\n"
            "    masync      set 'more async' flag on this sg device\n"
            "    mmap        setup mmap IO on IFILE or OFILE; OFILE only "
            "with noshare\n"
            "    mmap,mmap    when used twice, doesn't call munmap()\n"
            "    mout_if     set META_OUT_IF flag on each request\n"
            "    mrq_immed    if mrq active, do submit non-blocking (def: "
            "ordered\n"
            "                 blocking)\n"
            "    mrq_svb     if mrq and sg->sg copy, do shared_variable_"
            "blocking\n"
            "    nocreat     will fail rather than create OFILE\n"
            "    nodur       turns off command duration calculations\n"
            "    noxfer      no transfer to/from the user space\n"
            "    no_thresh   skip checking per fd max data xfer\n"
            "    null        does nothing, placeholder\n"
            "    polled      set POLLED flag on command, uses blk_poll() to "
            "complete\n"
            "    qhead       queue new request at head of block queue\n"
            "    qtail       queue new request at tail of block queue (def: "
            "q at head)\n"
            "    random      use random data instead of if=IFILE (only in "
            "iflags)\n"
            "    same_fds    each thread uses the same IFILE and OFILE(2) "
            "file\n"
            "                descriptors (def: each threads has own file "
            "descriptors)\n"
            "    swait       this option is now ignored\n"
            "    v3          use v3 sg interface (def: v3 unless sg driver "
            "is v4)\n"
            "    v4          use v4 sg interface (def: v3 unless sg driver "
            "is v4)\n"
            "    wq_excl     set SG_CTL_FLAGM_EXCL_WAITQ on this sg fd\n"
            "\n"
            "Copies IFILE to OFILE (and to OFILE2 if given). If IFILE and "
            "OFILE are sg\ndevices 'shared' mode is selected unless "
            "'noshare' is given to 'iflag=' or\n'oflag='. of2=OFILE2 uses "
            "'oflag=FLAGS'. When sharing, the data stays in a\nsingle "
            "in-kernel buffer which is copied (or mmap-ed) to the user "
            "space\nif the 'ofreg=OFREG' is given. Use '-hhhh' for more "
            "information.\n"
           );
    return;
page4:
    pr2serr("pack_id:\n"
            "These are ascending integers, starting at 1, associated with "
            "each issued\nSCSI command. When both IFILE and OFILE are sg "
            "devices, then the READ in\neach read-write pair is issued an "
            "even pack_id and its WRITE pair is\ngiven the pack_id one "
            "higher (i.e. an odd number). This enables a\n'dmesg -w' "
            "user to see that progress is being "
            "made.\n\n");
    pr2serr("Debugging:\n"
            "Apart from using one or more '--verbose' options which gets a "
            "bit noisy\n'dmesg -w' can give a good overview "
            "of what is happening.\nThat does a sg driver object tree "
            "traversal that does minimal locking\nto make sure that each "
            "traversal is 'safe'. So it is important to note\nthe whole "
            "tree is not locked. This means for fast devices the overall\n"
            "tree state may change while the traversal is occurring. For "
            "example,\nit has been observed that both the read- and write- "
            "sides of a request\nshare show they are in 'active' state "
            "which should not be possible.\nIt occurs because the read-"
            "side probably jumped out of active state and\nthe write-side "
            "request entered it while some other nodes were being "
            "printed.\n\n");
    pr2serr("Busy state:\n"
            "Busy state (abbreviated to 'bsy' in the dmesg "
            "output)\nis entered during request setup and completion. It "
            "is intended to be\na temporary state. It should not block "
            "but does sometimes (e.g. in\nblock_get_request()). Even so "
            "that blockage should be short and if not\nthere is a "
            "problem.\n\n");
    pr2serr("--verify :\n"
            "For comparing IFILE with OFILE. Does repeated sequences of: "
            "READ(ifile)\nand uses data returned to send to VERIFY(ofile, "
            "BYTCHK=1). So the OFILE\ndevice/disk is doing the actual "
            "comparison. Stops on first miscompare.\n\n");
    pr2serr("--prefetch :\n"
            "Used with --verify option. Prepends a PRE-FETCH(ofile, IMMED) "
            "to verify\nsequence. This should speed the trailing VERIFY by "
            "making sure that\nthe data it needs for the comparison is "
            "already in its cache.\n");
    return;
}

static void
lk_print_command_len(const char *prefix, uint8_t * cmdp, int len, bool lock)
{
    if (lock)
        pthread_mutex_lock(&strerr_mut);
    if (prefix && *prefix)
        fputs(prefix, stderr);
    sg_print_command_len(cmdp, len);
    if (lock)
        pthread_mutex_unlock(&strerr_mut);
}

static void
lk_chk_n_print3(const char * leadin, struct sg_io_hdr * hp, bool raw_sinfo)
{
    pthread_mutex_lock(&strerr_mut);
    sg_chk_n_print3(leadin, hp, raw_sinfo);
    pthread_mutex_unlock(&strerr_mut);
}

static void
lk_chk_n_print4(const char * leadin, const struct sg_io_v4 * h4p,
                bool raw_sinfo)
{
    pthread_mutex_lock(&strerr_mut);
    sg_linux_sense_print(leadin, h4p->device_status, h4p->transport_status,
                         h4p->driver_status, (const uint8_t *)h4p->response,
                         h4p->response_len, raw_sinfo);
    pthread_mutex_unlock(&strerr_mut);
}

static void
hex2stderr_lk(const uint8_t * b_str, int len, int no_ascii)
{
    pthread_mutex_lock(&strerr_mut);
    hex2stderr(b_str, len, no_ascii);
    pthread_mutex_unlock(&strerr_mut);
}

/* Flags decoded into abbreviations for those that are set, separated by
 * '|' . */
static char *
sg_flags_str(int flags, int b_len, char * b)
{
    int n = 0;

    if ((b_len < 1) || (! b))
        return b;
    b[0] = '\0';
    if (SG_FLAG_DIRECT_IO & flags) {            /* 0x1 */
        n += sg_scnpr(b + n, b_len - n, "DIO|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_FLAG_MMAP_IO & flags) {              /* 0x4 */
        n += sg_scnpr(b + n, b_len - n, "MMAP|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_YIELD_TAG & flags) {          /* 0x8 */
        n += sg_scnpr(b + n, b_len - n, "YTAG|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_FLAG_Q_AT_TAIL & flags) {            /* 0x10 */
        n += sg_scnpr(b + n, b_len - n, "QTAI|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_FLAG_Q_AT_HEAD & flags) {            /* 0x20 */
        n += sg_scnpr(b + n, b_len - n, "QHEA|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_DOUT_OFFSET & flags) {        /* 0x40 */
        n += sg_scnpr(b + n, b_len - n, "DOFF|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_EVENTFD & flags) {           /* 0x80 */
        n += sg_scnpr(b + n, b_len - n, "EVFD|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_COMPLETE_B4 & flags) {        /* 0x100 */
        n += sg_scnpr(b + n, b_len - n, "CPL_B4|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_SIGNAL & flags) {       /* 0x200 */
        n += sg_scnpr(b + n, b_len - n, "SIGNAL|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_IMMED & flags) {              /* 0x400 */
        n += sg_scnpr(b + n, b_len - n, "IMM|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_POLLED & flags) {             /* 0x800 */
        n += sg_scnpr(b + n, b_len - n, "POLLED|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_STOP_IF & flags) {            /* 0x1000 */
        n += sg_scnpr(b + n, b_len - n, "STOPIF|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_DEV_SCOPE & flags) {          /* 0x2000 */
        n += sg_scnpr(b + n, b_len - n, "DEV_SC|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_SHARE & flags) {              /* 0x4000 */
        n += sg_scnpr(b + n, b_len - n, "SHARE|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_DO_ON_OTHER & flags) {        /* 0x8000 */
        n += sg_scnpr(b + n, b_len - n, "DO_OTH|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_NO_DXFER & flags) {          /* 0x10000 */
        n += sg_scnpr(b + n, b_len - n, "NOXFER|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_KEEP_SHARE & flags) {        /* 0x20000 */
        n += sg_scnpr(b + n, b_len - n, "KEEP_SH|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_MULTIPLE_REQS & flags) {     /* 0x40000 */
        n += sg_scnpr(b + n, b_len - n, "MRQS|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_ORDERED_WR & flags) {        /* 0x80000 */
        n += sg_scnpr(b + n, b_len - n, "OWR|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_REC_ORDER & flags) {         /* 0x100000 */
        n += sg_scnpr(b + n, b_len - n, "REC_O|");
        if (n >= b_len)
            goto fini;
    }
    if (SGV4_FLAG_META_OUT_IF & flags) {       /* 0x200000 */
        n += sg_scnpr(b + n, b_len - n, "MOUT_IF|");
        if (n >= b_len)
            goto fini;
    }
    if (0 == n)
        n += sg_scnpr(b + n, b_len - n, "<none>");
fini:
    if (n < b_len) {    /* trim trailing '\' */
        if ('|' == b[n - 1])
            b[n - 1] = '\0';
    } else if ('|' == b[b_len - 1])
        b[b_len - 1] = '\0';
    return b;
}

/* Info field decoded into abbreviations for those bits that are set,
 * separated by '|' . */
static char *
sg_info_str(int info, int b_len, char * b)
{
    int n = 0;

    if ((b_len < 1) || (! b))
        return b;
    b[0] = '\0';
    if (SG_INFO_CHECK & info) {               /* 0x1 */
        n += sg_scnpr(b + n, b_len - n, "CHK|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_INFO_DIRECT_IO & info) {           /* 0x2 */
        n += sg_scnpr(b + n, b_len - n, "DIO|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_INFO_MIXED_IO & info) {            /* 0x4 */
        n += sg_scnpr(b + n, b_len - n, "MIO|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_INFO_DEVICE_DETACHING & info) {    /* 0x8 */
        n += sg_scnpr(b + n, b_len - n, "DETA|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_INFO_ABORTED & info) {             /* 0x10 */
        n += sg_scnpr(b + n, b_len - n, "ABRT|");
        if (n >= b_len)
            goto fini;
    }
    if (SG_INFO_MRQ_FINI & info) {            /* 0x20 */
        n += sg_scnpr(b + n, b_len - n, "MRQF|");
        if (n >= b_len)
            goto fini;
    }
fini:
    if (n < b_len) {    /* trim trailing '\' */
        if ('|' == b[n - 1])
            b[n - 1] = '\0';
    } else if ('|' == b[b_len - 1])
        b[b_len - 1] = '\0';
    return b;
}

static void
v4hdr_out_lk(const char * leadin, const sg_io_v4 * h4p, int id)
{
    char b[80];

    pthread_mutex_lock(&strerr_mut);
    if (leadin)
        pr2serr("%s [id=%d]:\n", leadin, id);
    if (('Q' != h4p->guard) || (0 != h4p->protocol) ||
        (0 != h4p->subprotocol))
        pr2serr("  <<<sg_io_v4 _NOT_ properly set>>>\n");
    pr2serr("  pointers: cdb=%s  sense=%s  din=%p  dout=%p\n",
            (h4p->request ? "y" : "NULL"), (h4p->response ? "y" : "NULL"),
            (void *)h4p->din_xferp, (void *)h4p->dout_xferp);
    pr2serr("  lengths: cdb=%u  sense=%u  din=%u  dout=%u\n",
            h4p->request_len, h4p->max_response_len, h4p->din_xfer_len,
             h4p->dout_xfer_len);
    pr2serr("  flags=0x%x  request_extra{pack_id}=%d\n",
            h4p->flags, h4p->request_extra);
    pr2serr("  flags set: %s\n", sg_flags_str(h4p->flags, sizeof(b), b));
    pr2serr(" %s OUT fields:\n", leadin);
    pr2serr("  response_len=%d driver/transport/device_status="
            "0x%x/0x%x/0x%x\n", h4p->response_len, h4p->driver_status,
            h4p->transport_status, h4p->device_status);
    pr2serr("  info=0x%x  din_resid=%u  dout_resid=%u  spare_out=%u  "
            "dur=%u\n",
            h4p->info, h4p->din_resid, h4p->dout_resid, h4p->spare_out,
            h4p->duration);
    pthread_mutex_unlock(&strerr_mut);
}

static void
fetch_sg_version(void)
{
    FILE * fp;
    char b[96];

    have_sg_version = false;
    sg_version = 0;
    fp = fopen(PROC_SCSI_SG_VERSION, "r");
    if (fp && fgets(b, sizeof(b) - 1, fp)) {
        if (1 == sscanf(b, "%d", &sg_version))
            have_sg_version = !!sg_version;
    } else {
        int j, k, l;

        if (fp)
            fclose(fp);
        fp = fopen(SYS_SCSI_SG_VERSION, "r");
        if (fp && fgets(b, sizeof(b) - 1, fp)) {
            if (3 == sscanf(b, "%d.%d.%d", &j, &k, &l)) {
                sg_version = (j * 10000) + (k * 100) + l;
                have_sg_version = !!sg_version;
            }
        }
    }
    if (fp)
        fclose(fp);
}

static void
calc_duration_throughput(int contin)
{
    struct timeval end_tm, res_tm;
    double a, b;

    gettimeofday(&end_tm, NULL);
    res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
    res_tm.tv_usec = end_tm.tv_usec - start_tm.tv_usec;
    if (res_tm.tv_usec < 0) {
        --res_tm.tv_sec;
        res_tm.tv_usec += 1000000;
    }
    a = res_tm.tv_sec;
    a += (0.000001 * res_tm.tv_usec);
    b = (double)gcoll.bs * (dd_count - gcoll.out_rem_count.load());
    pr2serr("time to %s data %s %d.%06d secs",
            (gcoll.verify ? "verify" : "copy"), (contin ? "so far" : "was"),
            (int)res_tm.tv_sec, (int)res_tm.tv_usec);
    if ((a > 0.00001) && (b > 511))
        pr2serr(", %.2f MB/sec\n", b / (a * 1000000.0));
    else
        pr2serr("\n");
}

static void
print_stats(const char * str)
{
    int64_t infull;

    if (0 != gcoll.out_rem_count.load())
        pr2serr("  remaining block count=%" PRId64 "\n",
                gcoll.out_rem_count.load());
    infull = dd_count - gcoll.in_rem_count.load();
    pr2serr("%s%" PRId64 "+%d records in\n", str,
            infull - gcoll.in_partial.load(), gcoll.in_partial.load());

    if (gcoll.out_type == FT_DEV_NULL)
        pr2serr("%s0+0 records out\n", str);
    else {
        int64_t outfull = dd_count - gcoll.out_rem_count.load();

        pr2serr("%s%" PRId64 "+%d records %s\n", str,
                outfull - gcoll.out_partial.load(), gcoll.out_partial.load(),
                (gcoll.verify ? "verified" : "out"));
    }
}

static void
interrupt_handler(int sig)
{
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(sig, &sigact, NULL);
    pr2serr("Interrupted by signal,");
    if (do_time > 0)
        calc_duration_throughput(0);
    print_stats("");
    kill(getpid (), sig);
}

static void
siginfo_handler(int sig)
{
    if (sig) { ; }      /* unused, dummy to suppress warning */
    pr2serr("Progress report, continuing ...\n");
    if (do_time > 0)
        calc_duration_throughput(1);
    print_stats("  ");
}

static void
siginfo2_handler(int sig)
{
    struct global_collection * clp = &gcoll;

    if (sig) { ; }      /* unused, dummy to suppress warning */
    pr2serr("Progress report, continuing ...\n");
    if (do_time > 0)
        calc_duration_throughput(1);
    print_stats("  ");
    pr2serr("Send broadcast on out_sync_cv condition variable\n");
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void
install_handler(int sig_num, void (*sig_handler) (int sig))
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

#ifdef SG_LIB_ANDROID
static void
thread_exit_handler(int sig)
{
    pthread_exit(0);
}
#endif

/* Make safe_strerror() thread safe */
static char *
tsafe_strerror(int code, char * ebp)
{
    char * cp;

    pthread_mutex_lock(&strerr_mut);
    cp = safe_strerror(code);
    strncpy(ebp, cp, STRERR_BUFF_LEN);
    pthread_mutex_unlock(&strerr_mut);

    ebp[strlen(ebp)] = '\0';
    return ebp;
}


/* Following macro from D.R. Butenhof's POSIX threads book:
 * ISBN 0-201-63392-2 . [Highly recommended book.] Changed __FILE__
 * to __func__ */
#define err_exit(code,text) do { \
    char strerr_buff[STRERR_BUFF_LEN + 1]; \
    pr2serr("%s at \"%s\":%d: %s\n", \
        text, __func__, __LINE__, tsafe_strerror(code, strerr_buff)); \
    exit(1); \
    } while (0)


static int
dd_filetype(const char * filename, off_t & st_size)
{
    struct stat st;
    size_t len = strlen(filename);

    if ((1 == len) && ('.' == filename[0]))
        return FT_DEV_NULL;
    if (stat(filename, &st) < 0)
        return FT_ERROR;
    if (S_ISCHR(st.st_mode)) {
        if ((MEM_MAJOR == major(st.st_rdev)) &&
            ((DEV_NULL_MINOR_NUM == minor(st.st_rdev)) ||
             (DEV_ZERO_MINOR_NUM == minor(st.st_rdev))))
            return FT_DEV_NULL; /* treat /dev/null + /dev/zero the same */
        if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_SG;
        if (SCSI_TAPE_MAJOR == major(st.st_rdev))
            return FT_ST;
        return FT_CHAR;
    } else if (S_ISBLK(st.st_mode))
        return FT_BLOCK;
    else if (S_ISFIFO(st.st_mode))
        return FT_FIFO;
    st_size = st.st_size;
    return FT_OTHER;
}

static inline void
stop_both(struct global_collection * clp)
{
    clp->in_stop = true;
    clp->out_stop = true;
}

/* Return of 0 -> success, see sg_ll_read_capacity*() otherwise */
static int
scsi_read_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
    int res;
    uint8_t rcBuff[RCAP16_REPLY_LEN] = {};

    res = sg_ll_readcap_10(sg_fd, 0, 0, rcBuff, READ_CAP_REPLY_LEN, false, 0);
    if (0 != res)
        return res;

    if ((0xff == rcBuff[0]) && (0xff == rcBuff[1]) && (0xff == rcBuff[2]) &&
        (0xff == rcBuff[3])) {

        res = sg_ll_readcap_16(sg_fd, 0, 0, rcBuff, RCAP16_REPLY_LEN, false,
                               0);
        if (0 != res)
            return res;
        *num_sect = sg_get_unaligned_be64(rcBuff + 0) + 1;
        *sect_sz = sg_get_unaligned_be32(rcBuff + 8);
    } else {
        /* take care not to sign extend values > 0x7fffffff */
        *num_sect = (int64_t)sg_get_unaligned_be32(rcBuff + 0) + 1;
        *sect_sz = sg_get_unaligned_be32(rcBuff + 4);
    }
    return 0;
}

/* Return of 0 -> success, -1 -> failure. BLKGETSIZE64, BLKGETSIZE and */
/* BLKSSZGET macros problematic (from <linux/fs.h> or <sys/mount.h>). */
static int
read_blkdev_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
#ifdef BLKSSZGET
    if ((ioctl(sg_fd, BLKSSZGET, sect_sz) < 0) && (*sect_sz > 0)) {
        perror("BLKSSZGET ioctl error");
        return -1;
    } else {
 #ifdef BLKGETSIZE64
        uint64_t ull;

        if (ioctl(sg_fd, BLKGETSIZE64, &ull) < 0) {

            perror("BLKGETSIZE64 ioctl error");
            return -1;
        }
        *num_sect = ((int64_t)ull / (int64_t)*sect_sz);
 #else
        unsigned long ul;

        if (ioctl(sg_fd, BLKGETSIZE, &ul) < 0) {
            perror("BLKGETSIZE ioctl error");
            return -1;
        }
        *num_sect = (int64_t)ul;
 #endif
    }
    return 0;
#else
    *num_sect = 0;
    *sect_sz = 0;
    return -1;
#endif
}

static int
system_wrapper(const char * cmd)
{
    int res;

    res = system(cmd);
    if (WIFSIGNALED(res) &&
        (WTERMSIG(res) == SIGINT || WTERMSIG(res) == SIGQUIT))
        raise(WTERMSIG(res));
    return WEXITSTATUS(res);
}

/* Has an infinite loop doing a timed wait for any signals in sig_set. After
 * each timeout (300 ms) checks if the most_recent_pack_id atomic integer
 * has changed. If not after another two timeouts announces a stall has
 * been detected. If shutting down atomic is true breaks out of loop and
 * shuts down this thread. Other than that, this thread is normally cancelled
 * by the main thread, after other threads have exited. */
static void *
sig_listen_thread(void * v_clp)
{
    bool stall_reported = false;
    int prev_pack_id = 0;
    struct timespec ts;
    struct timespec * tsp = &ts;
    struct global_collection * clp = (struct global_collection *)v_clp;
    uint32_t ict_ms = (clp->sdt_ict ? clp->sdt_ict : DEF_SDT_ICT_MS);

    tsp->tv_sec = ict_ms / 1000;
    tsp->tv_nsec = (ict_ms % 1000) * 1000 * 1000;   /* DEF_SDT_ICT_MS */
    while (1) {
        int sig_number = sigtimedwait(&signal_set, NULL, tsp);

        if (sig_number < 0) {
            int err = errno;

            /* EAGAIN implies a timeout */
            if ((EAGAIN == err) && (clp->sdt_crt > 0)) {
                int pack_id = mono_pack_id.load();

                if ((pack_id > 0) && (pack_id == prev_pack_id)) {
                    if (! stall_reported) {
                        stall_reported = true;
                        tsp->tv_sec = clp->sdt_crt;
                        tsp->tv_nsec = 0;
                        pr2serr_lk("%s: first stall at pack_id=%d detected\n",
                                   __func__, pack_id);
                    } else
                        pr2serr_lk("%s: subsequent stall at pack_id=%d\n",
                               __func__, pack_id);
                    // following command assumes linux bash or similar shell
                    system_wrapper("cat /proc/scsi/sg/debug >> /dev/stderr\n");
                    // system_wrapper("/usr/bin/dmesg\n");
                } else
                    prev_pack_id = pack_id;
            } else if (EAGAIN != err)
                pr2serr_lk("%s: sigtimedwait() errno=%d\n", __func__, err);
        }
        if (SIGINT == sig_number) {
            pr2serr_lk("%sinterrupted by SIGINT\n", my_name);
            stop_both(clp);
            pthread_cond_broadcast(&clp->out_sync_cv);
            sigprocmask(SIG_SETMASK, &orig_signal_set, NULL);
            raise(SIGINT);
            break;
        }
        if (SIGUSR2 == sig_number) {
            if (clp->verbose > 2)
                pr2serr_lk("%s: interrupted by SIGUSR2\n", __func__);
            break;
        }
        if (shutting_down)
            break;
    }           /* end of while loop */
    if (clp->verbose > 3)
        pr2serr_lk("%s: exiting\n", __func__);

    return NULL;
}

static void *
mrq_abort_thread(void * v_maip)
{
    int res, err;
    int n = 0;
    int seed;
    unsigned int rn;
    Mrq_abort_info l_mai = *(Mrq_abort_info *)v_maip;
    struct sg_io_v4 ctl_v4 {};

#ifdef HAVE_GETRANDOM
    {
        ssize_t ssz = getrandom(&seed, sizeof(seed), GRND_NONBLOCK);

        if (ssz < (ssize_t)sizeof(seed)) {
            pr2serr("getrandom() failed, ret=%d\n", (int)ssz);
            seed = (int)time(NULL);
        }
    }
#else
    seed = (int)time(NULL);    /* use seconds since epoch as proxy */
#endif
    if (l_mai.debug)
        pr2serr_lk("%s: from_id=%d: to abort mrq_pack_id=%d\n", __func__,
                   l_mai.from_tid, l_mai.mrq_id);
    res = ioctl(l_mai.fd, SG_GET_NUM_WAITING, &n);
    ++num_waiting_calls;
    if (res < 0) {
        err = errno;
        pr2serr_lk("%s: ioctl(SG_GET_NUM_WAITING) failed: %s [%d]\n",
                   __func__, safe_strerror(err), err);
    } else if (l_mai.debug)
        pr2serr_lk("%s: num_waiting=%d\n", __func__, n);

    Rand_uint * ruip = new Rand_uint(5, 500, seed);
    struct timespec tspec = {0, 4000 /* 4 usecs */};
    rn = ruip->get();
    tspec.tv_nsec = rn * 1000;
    if (l_mai.debug > 1)
        pr2serr_lk("%s: /dev/urandom seed=0x%x delay=%u microsecs\n",
                   __func__, seed, rn);
    if (rn >= 20)
        nanosleep(&tspec, NULL);
    else if (l_mai.debug > 1)
        pr2serr_lk("%s: skipping nanosleep cause delay < 20 usecs\n",
                   __func__);

    ctl_v4.guard = 'Q';
    ctl_v4.flags = SGV4_FLAG_MULTIPLE_REQS;
    ctl_v4.request_extra = l_mai.mrq_id;
    ++num_mrq_abort_req;
    res = ioctl(l_mai.fd, SG_IOABORT, &ctl_v4);
    if (res < 0) {
        err = errno;
        if (ENODATA == err)
            pr2serr_lk("%s: ioctl(SG_IOABORT) no match on "
                       "MRQ pack_id=%d\n", __func__, l_mai.mrq_id);
        else
            pr2serr_lk("%s: MRQ ioctl(SG_IOABORT) failed: %s [%d]\n",
                       __func__, safe_strerror(err), err);
    } else {
        ++num_mrq_abort_req_success;
        if (l_mai.debug > 1)
            pr2serr_lk("%s: from_id=%d sent ioctl(SG_IOABORT) on MRQ rq_id="
                       "%d, success\n", __func__, l_mai.from_tid,
                       l_mai.mrq_id);
    }
    delete ruip;
    return NULL;
}

static bool
sg_share_prepare(int write_side_fd, int read_side_fd, int id, bool vb_b)
{
    struct sg_extended_info sei {};
    struct sg_extended_info * seip = &sei;

    seip->sei_wr_mask |= SG_SEIM_SHARE_FD;
    seip->sei_rd_mask |= SG_SEIM_SHARE_FD;
    seip->share_fd = read_side_fd;
    if (ioctl(write_side_fd, SG_SET_GET_EXTENDED, seip) < 0) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(shared_fd=%d), failed "
                   "errno=%d %s\n", id, read_side_fd, errno,
                   strerror(errno));
        return false;
    }
    if (vb_b)
        pr2serr_lk("%s: tid=%d: ioctl(EXTENDED(shared_fd)) ok, "
                   "read_side_fd=%d, write_side_fd=%d\n", __func__,
                   id, read_side_fd, write_side_fd);
    return true;
}

static void
sg_unshare(int sg_fd, int id, bool vb_b)
{
    struct sg_extended_info sei {};
    struct sg_extended_info * seip = &sei;

    seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
    seip->sei_rd_mask |= SG_SEIM_CTL_FLAGS;
    seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_UNSHARE;
    seip->ctl_flags |= SG_CTL_FLAGM_UNSHARE; /* needs to be set to unshare */
    if (ioctl(sg_fd, SG_SET_GET_EXTENDED, seip) < 0) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(UNSHARE), failed errno=%d %s\n",
                   id,  errno, strerror(errno));
        return;
    }
    if (vb_b)
        pr2serr_lk("tid=%d: ioctl(UNSHARE) ok\n", id);
}

static void
sg_noshare_enlarge(int sg_fd, bool vb_b)
{
    if (sg_version_ge_40045) {
        struct sg_extended_info sei {};
        struct sg_extended_info * seip = &sei;

        sei.sei_wr_mask |= SG_SEIM_TOT_FD_THRESH;
        seip->tot_fd_thresh = 96 * 1024 * 1024;
        if (ioctl(sg_fd, SG_SET_GET_EXTENDED, seip) < 0) {
            pr2serr_lk("%s: ioctl(EXTENDED(TOT_FD_THRESH), failed errno=%d "
                       "%s\n", __func__, errno, strerror(errno));
            return;
        }
        if (vb_b)
            pr2serr_lk("ioctl(TOT_FD_THRESH) ok\n");
    }
}

static void
sg_take_snap(int sg_fd, int id, bool vb_b)
{
    struct sg_extended_info sei {};
    struct sg_extended_info * seip = &sei;

    seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
    seip->sei_rd_mask |= SG_SEIM_CTL_FLAGS;
    seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_SNAP_DEV;
    seip->ctl_flags &= ~SG_CTL_FLAGM_SNAP_DEV;   /* 0 --> append */
    if (ioctl(sg_fd, SG_SET_GET_EXTENDED, seip) < 0) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(SNAP_DEV), failed errno=%d %s\n",
                   id,  errno, strerror(errno));
        return;
    }
    if (vb_b)
        pr2serr_lk("tid=%d: ioctl(SNAP_DEV) ok\n", id);
}

static void
cleanup_in(void * v_clp)
{
    struct global_collection * clp = (struct global_collection *)v_clp;

    pr2serr("thread cancelled while in mutex held\n");
    stop_both(clp);
    pthread_mutex_unlock(&clp->in_mutex);
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void
cleanup_out(void * v_clp)
{
    struct global_collection * clp = (struct global_collection *)v_clp;

    pr2serr("thread cancelled while out_mutex held\n");
    stop_both(clp);
    pthread_mutex_unlock(&clp->out_mutex);
    pthread_cond_broadcast(&clp->out_sync_cv);
}

static void inline buffp_onto_next(Rq_elem * rep)
{
    struct global_collection * clp = rep->clp;

    if ((clp->nmrqs > 0) && clp->unbalanced_mrq) {
        ++rep->mrq_index;
        if (rep->mrq_index >= clp->nmrqs)
            rep->mrq_index = 0;         /* wrap */
    }
}

static inline uint8_t *
get_buffp(Rq_elem * rep)
{
    struct global_collection * clp = rep->clp;

    if ((clp->nmrqs > 0) && clp->unbalanced_mrq && (rep->mrq_index > 0))
        return rep->buffp + (rep->mrq_index * clp->bs * clp->bpt);
    else
        return rep->buffp;
}

static void *
read_write_thread(void * v_tip)
{
    Thread_info * tip;
    struct global_collection * clp;
    Rq_elem rel {};
    Rq_elem * rep = &rel;
    int n, sz, blocks, status, vb, err, res, wr_blks, c_addr;
    int num_sg = 0;
    int64_t my_index;
    volatile bool stop_after_write = false;
    bool own_infd = false;
    bool in_is_sg, in_mmap, out_is_sg, out_mmap;
    bool own_outfd = false;
    bool own_out2fd = false;
    bool share_and_ofreg;
    mrq_arr_t deferred_arr;  /* MRQ deferred array (vector) */

    tip = (Thread_info *)v_tip;
    clp = tip->gcp;
    vb = clp->verbose;
    rep->bs = clp->bs;
    sz = clp->bpt * rep->bs;
    c_addr = clp->chkaddr;
    in_is_sg = (FT_SG == clp->in_type);
    in_mmap = (in_is_sg && (clp->in_flags.mmap > 0));
    out_is_sg = (FT_SG == clp->out_type);
    out_mmap = (out_is_sg && (clp->out_flags.mmap > 0));
    /* Following clp members are constant during lifetime of thread */
    rep->clp = clp;
    rep->id = tip->id;
    if (vb > 2)
        pr2serr_lk("%d <-- Starting worker thread\n", rep->id);
    if (! (in_mmap || out_mmap)) {
        n = sz;
        if (clp->unbalanced_mrq)
            n *= clp->nmrqs;
        rep->buffp = sg_memalign(n, 0 /* page align */, &rep->alloc_bp,
                                 false);
        if (NULL == rep->buffp)
            err_exit(ENOMEM, "out of memory creating user buffers\n");
    }
    rep->infd = clp->infd;
    rep->outfd = clp->outfd;
    rep->out2fd = clp->out2fd;
    rep->outregfd = clp->outregfd;
    rep->rep_count = 0;
    if (clp->unbalanced_mrq && (clp->nmrqs > 0))
        rep->mrq_index = clp->nmrqs - 1;

    if (rep->infd == rep->outfd) {
        if (in_is_sg)
            rep->same_sg = true;
    } else if (in_is_sg && out_is_sg)
        rep->both_sg = true;
    else if (in_is_sg)
        rep->only_in_sg = true;
    else if (out_is_sg)
        rep->only_out_sg = true;

    if (clp->in_flags.random) {
#ifdef HAVE_GETRANDOM
        ssize_t ssz = getrandom(&rep->seed, sizeof(rep->seed), GRND_NONBLOCK);

        if (ssz < (ssize_t)sizeof(rep->seed)) {
            pr2serr_lk("thread=%d: getrandom() failed, ret=%d\n",
                       rep->id, (int)ssz);
            rep->seed = (long)time(NULL);
        }
#else
        rep->seed = (long)time(NULL);    /* use seconds since epoch as proxy */
#endif
        if (vb > 1)
            pr2serr_lk("thread=%d: seed=%ld\n", rep->id, rep->seed);
#ifdef HAVE_SRAND48_R
        srand48_r(rep->seed, &rep->drand);
#else
        srand48(rep->seed);
#endif
    }
    if (clp->in_flags.same_fds || clp->out_flags.same_fds)
        ;
    else {
        int fd;

        if (in_is_sg && clp->infp) {
            fd = sg_in_open(clp, clp->infp, (in_mmap ? &rep->buffp : NULL),
                            (in_mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->infd = fd;
            rep->mmap_active = in_mmap ? clp->in_flags.mmap : 0;
            if (in_mmap && (vb > 4))
                pr2serr_lk("thread=%d: mmap buffp=%p\n", rep->id, rep->buffp);
            own_infd = true;
            ++num_sg;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg IFILE\n", rep->id);
        }
        if (out_is_sg && clp->outfp) {
            fd = sg_out_open(clp, clp->outfp, (out_mmap ? &rep->buffp : NULL),
                             (out_mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->outfd = fd;
            if (! rep->mmap_active)
                rep->mmap_active = out_mmap ? clp->out_flags.mmap : 0;
            if (out_mmap && (vb > 4))
                pr2serr_lk("thread=%d: mmap buffp=%p\n", rep->id, rep->buffp);
            own_outfd = true;
            ++num_sg;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg OFILE\n", rep->id);
        }
        if ((FT_SG == clp->out2_type) && clp->out2fp) {
            fd = sg_out_open(clp, clp->out2fp,
                             (out_mmap ? &rep->buffp : NULL),
                             (out_mmap ? &rep->mmap_len : NULL));
            if (fd < 0)
                goto fini;
            rep->out2fd = fd;
            own_out2fd = true;
            if (vb > 2)
                pr2serr_lk("thread=%d: opened local sg OFILE2\n", rep->id);
        }
    }
    if (vb > 2) {
        if (in_is_sg && (! own_infd))
            pr2serr_lk("thread=%d: using global sg IFILE, fd=%d\n", rep->id,
                       rep->infd);
        if (out_is_sg && (! own_outfd))
            pr2serr_lk("thread=%d: using global sg OFILE, fd=%d\n", rep->id,
                       rep->outfd);
        if ((FT_SG == clp->out2_type) && (! own_out2fd))
            pr2serr_lk("thread=%d: using global sg OFILE2, fd=%d\n", rep->id,
                       rep->out2fd);
    }
    if (!sg_version_ge_40045) {
        if (vb > 4)
            pr2serr_lk("thread=%d: Skipping share because driver too old\n",
                       rep->id);
    } else if (clp->noshare) {
        if (vb > 4)
            pr2serr_lk("thread=%d: Skipping IFILE share with OFILE due to "
                       "noshare=1\n", rep->id);
    } else if (sg_version_ge_40045 && in_is_sg && out_is_sg)
        rep->has_share = sg_share_prepare(rep->outfd, rep->infd, rep->id,
                                          vb > 9);
    if (vb > 9)
        pr2serr_lk("tid=%d, has_share=%s\n", rep->id,
                   (rep->has_share ? "true" : "false"));
    share_and_ofreg = (rep->has_share && (rep->outregfd >= 0));

    /* vvvvvvvvvvvvvv  Main segment copy loop  vvvvvvvvvvvvvvvvvvvvvvv */
    while (1) {
        rep->wr = false;
        my_index = atomic_fetch_add(&pos_index, (long int)clp->bpt);
        /* Start of READ half of a segment */
        buffp_onto_next(rep);
        status = pthread_mutex_lock(&clp->in_mutex);
        if (0 != status) err_exit(status, "lock in_mutex");

        if (dd_count >= 0) {
            if (my_index >= dd_count) {
                status = pthread_mutex_unlock(&clp->in_mutex);
                if (0 != status) err_exit(status, "unlock in_mutex");
                if ((clp->nmrqs > 0) && (deferred_arr.first.size() > 0)) {
                    if (vb > 2)
                        pr2serr_lk("thread=%d: tail-end my_index>=dd_count, "
                                   "to_do=%u\n", rep->id,
                                   (uint32_t)deferred_arr.first.size());
                    res = sgh_do_deferred_mrq(rep, deferred_arr);
                    if (res)
                        pr2serr_lk("%s tid=%d: sgh_do_deferred_mrq failed\n",
                                   __func__, rep->id);
                }
                break;  /* at or beyond end, so leave loop >>>>>>>>>>  */
            } else if ((my_index + clp->bpt) > dd_count)
                blocks = dd_count - my_index;
            else
                blocks = clp->bpt;
        } else
            blocks = clp->bpt;

        rep->iblk = clp->skip + my_index;
        rep->oblk = clp->seek + my_index;
        rep->num_blks = blocks;

        // clp->in_blk += blocks;
        // clp->in_count -= blocks;

        pthread_cleanup_push(cleanup_in, (void *)clp);
        if (in_is_sg)
            sg_in_rd_cmd(clp, rep, deferred_arr);
        else {
            stop_after_write = normal_in_rd(rep, blocks);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
        }
        if (c_addr && (rep->bs > 3)) {
            int k, j, off, num;
            uint32_t addr = (uint32_t)rep->iblk;

            num = (1 == c_addr) ? 4 : (rep->bs - 3);
            for (k = 0, off = 0; k < blocks; ++k, ++addr, off += rep->bs) {
                for (j = 0; j < num; j += 4) {
                    if (addr != sg_get_unaligned_be32(rep->buffp + off + j))
                        break;
                }
                if (j < num)
                    break;
            }
            if (k < blocks) {
                pr2serr("%s: chkaddr failure at addr=0x%x\n", __func__, addr);
                exit_status = SG_LIB_CAT_MISCOMPARE;
                ++num_miscompare;
                stop_both(clp);
            }
        }
        pthread_cleanup_pop(0);
        ++rep->rep_count;

        /* Start of WRITE part of a segment */
        rep->wr = true;
        status = pthread_mutex_lock(&clp->out_mutex);
        if (0 != status) err_exit(status, "lock out_mutex");

        /* Make sure the OFILE (+ OFREG) are in same sequence as IFILE */
        if (clp->in_flags.random)
            goto skip_force_out_sequence;
        if ((rep->outregfd < 0) && in_is_sg && out_is_sg)
            goto skip_force_out_sequence;
        if (share_and_ofreg || (FT_DEV_NULL != clp->out_type)) {
            while ((! clp->out_stop.load()) &&
                   (rep->oblk != clp->out_blk.load())) {
                /* if write would be out of sequence then wait */
                pthread_cleanup_push(cleanup_out, (void *)clp);
                status = pthread_cond_wait(&clp->out_sync_cv, &clp->out_mutex);
                if (0 != status) err_exit(status, "cond out_sync_cv");
                pthread_cleanup_pop(0);
            }
        }

skip_force_out_sequence:
        if (clp->out_stop.load() || (clp->out_count.load() <= 0)) {
            if (! clp->out_stop.load())
                clp->out_stop = true;
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
            break;      /* stop requested so leave loop >>>>>>>>>>  */
        }
        if (stop_after_write)
            clp->out_stop = true;

        clp->out_count -= blocks;
        clp->out_blk += blocks;

        pthread_cleanup_push(cleanup_out, (void *)clp);
        if (rep->outregfd >= 0) {
            res = write(rep->outregfd, get_buffp(rep),
                        rep->bs * rep->num_blks);
            err = errno;
            if (res < 0)
                pr2serr_lk("%s: tid=%d: write(outregfd) failed: %s\n",
                           __func__, rep->id, strerror(err));
            else if (vb > 9)
                pr2serr_lk("%s: tid=%d: write(outregfd), fd=%d, num_blks=%d"
                           "\n", __func__, rep->id, rep->outregfd,
                           rep->num_blks);
        }
        /* Output to OFILE */
        wr_blks = rep->num_blks;
        if (out_is_sg) {
            sg_out_wr_cmd(rep, deferred_arr, false, clp->prefetch);
            ++rep->rep_count;
        } else if (FT_DEV_NULL == clp->out_type) {
            /* skip actual write operation */
            wr_blks = 0;
            clp->out_rem_count -= blocks;
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
        } else {
            normal_out_wr(rep, blocks);
            status = pthread_mutex_unlock(&clp->out_mutex);
            if (0 != status) err_exit(status, "unlock out_mutex");
            ++rep->rep_count;
        }
        pthread_cleanup_pop(0);

        /* Output to OFILE2 if sg device */
        if ((clp->out2fd >= 0) && (FT_SG == clp->out2_type)) {
            pthread_cleanup_push(cleanup_out, (void *)clp);
            status = pthread_mutex_lock(&clp->out2_mutex);
            if (0 != status) err_exit(status, "lock out2_mutex");
            /* releases out2_mutex mid operation */
            sg_out_wr_cmd(rep, deferred_arr, true, false);

            pthread_cleanup_pop(0);
        }
        if (0 == rep->num_blks) {
            if ((clp->nmrqs > 0) && (deferred_arr.first.size() > 0)) {
                if (wr_blks > 0)
                    rep->out_mrq_q_blks += wr_blks;
                if (vb > 2)
                    pr2serr_lk("thread=%d: tail-end, to_do=%u\n", rep->id,
                               (uint32_t)deferred_arr.first.size());
                res = sgh_do_deferred_mrq(rep, deferred_arr);
                if (res)
                    pr2serr_lk("%s tid=%d: sgh_do_deferred_mrq failed\n",
                               __func__, rep->id);
            }
            clp->out_stop = true;
            stop_after_write = true;
            break;      /* read nothing so leave loop >>>>>>>>>>  */
        }
        // if ((! rep->has_share) && (FT_DEV_NULL != clp->out_type))
        pthread_cond_broadcast(&clp->out_sync_cv);
        if (stop_after_write)
            break;      /* leaving main loop >>>>>>>>> */
    }   /* ^^^^^^^^^^ end of main while loop which copies segments ^^^^^^ */

    status = pthread_mutex_lock(&clp->in_mutex);
    if (0 != status) err_exit(status, "lock in_mutex");
    if (! clp->in_stop.load())
        clp->in_stop = true;  /* flag other workers to stop */
    status = pthread_mutex_unlock(&clp->in_mutex);
    if (0 != status) err_exit(status, "unlock in_mutex");

fini:
    if ((1 == rep->mmap_active) && (rep->mmap_len > 0)) {
        if (munmap(rep->buffp, rep->mmap_len) < 0) {
            err = errno;
            char bb[STRERR_BUFF_LEN + 1];

            pr2serr_lk("thread=%d: munmap() failed: %s\n", rep->id,
                       tsafe_strerror(err, bb));
        }
        if (vb > 4)
            pr2serr_lk("thread=%d: munmap(%p, %d)\n", rep->id, rep->buffp,
                       rep->mmap_len);
        rep->mmap_active = 0;
    }
    if (rep->alloc_bp) {
        free(rep->alloc_bp);
        rep->alloc_bp = NULL;
        rep->buffp = NULL;
    }

    if (sg_version_ge_40045) {
        if (clp->noshare) {
            if ((clp->nmrqs > 0) && clp->unshare)
                sg_unshare(rep->infd, rep->id, vb > 9);
        } else if (in_is_sg && out_is_sg)
            if (clp->unshare)
            sg_unshare(rep->infd, rep->id, vb > 9);
    }
    if (own_infd && (rep->infd >= 0)) {
        if (vb && in_is_sg) {
            ++num_waiting_calls;
            if (ioctl(rep->infd, SG_GET_NUM_WAITING, &n) >= 0) {
                if (n > 0)
                    pr2serr_lk("%s: tid=%d: num_waiting=%d prior close(in)\n",
                               __func__, rep->id, n);
            } else {
                err = errno;
                pr2serr_lk("%s: [%d] ioctl(SG_GET_NUM_WAITING) errno=%d: "
                           "%s\n", __func__, rep->id, err, strerror(err));
            }
        }
        close(rep->infd);
    }
    if (own_outfd && (rep->outfd >= 0)) {
        if (vb && out_is_sg) {
            ++num_waiting_calls;
            if (ioctl(rep->outfd, SG_GET_NUM_WAITING, &n) >= 0) {
                if (n > 0)
                    pr2serr_lk("%s: tid=%d: num_waiting=%d prior "
                               "close(out)\n", __func__, rep->id, n);
            } else {
                err = errno;
                pr2serr_lk("%s: [%d] ioctl(SG_GET_NUM_WAITING) errno=%d: "
                           "%s\n", __func__, rep->id, err, strerror(err));
            }
        }
        close(rep->outfd);
    }
    if (own_out2fd && (rep->out2fd >= 0))
        close(rep->out2fd);
    pthread_cond_broadcast(&clp->out_sync_cv);
    return stop_after_write ? NULL : clp;
}

/* N.B. A return of true means it wants to stop the copy. So false is the
 * 'good' reply (i.e. keep going). */
static bool
normal_in_rd(Rq_elem * rep, int blocks)
{
    struct global_collection * clp = rep->clp;
    bool stop_after_write = false;
    bool same_fds = clp->in_flags.same_fds || clp->out_flags.same_fds;
    int res;

    if (clp->verbose > 4)
        pr2serr_lk("%s: tid=%d: iblk=%" PRIu64 ", blocks=%d\n", __func__,
                   rep->id, rep->iblk, blocks);
    if (FT_RANDOM_0_FF == clp->in_type) {
        int k, j;
        const int jbump = sizeof(uint32_t);
        long rn;
        uint8_t * bp;

        if (clp->in_flags.zero && clp->in_flags.ff && (rep->bs >= 4)) {
            uint32_t pos = (uint32_t)rep->iblk;
            uint32_t off;

            for (k = 0, off = 0; k < blocks; ++k, off += rep->bs, ++pos) {
                for (j = 0; j < (rep->bs - 3); j += 4)
                    sg_put_unaligned_be32(pos, rep->buffp + off + j);
            }
        } else if (clp->in_flags.zero)
            memset(rep->buffp, 0, blocks * rep->bs);
        else if (clp->in_flags.ff)
            memset(rep->buffp, 0xff, blocks * rep->bs);
        else {
            for (k = 0, bp = rep->buffp; k < blocks; ++k, bp += rep->bs) {
                for (j = 0; j < rep->bs; j += jbump) {
                    /* mrand48 takes uniformly from [-2^31, 2^31) */
#ifdef HAVE_SRAND48_R
                    mrand48_r(&rep->drand, &rn);
#else
                    rn = mrand48();
#endif
                    *((uint32_t *)(bp + j)) = (uint32_t)rn;
                }
            }
        }
        clp->in_rem_count -= blocks;
        return stop_after_write;
    }
    if (! same_fds) {   /* each has own file pointer, so we need to move it */
        int64_t pos = rep->iblk * rep->bs;

        if (lseek64(rep->infd, pos, SEEK_SET) < 0) {    /* problem if pipe! */
            pr2serr_lk("%s: tid=%d: >> lseek64(%" PRId64 "): %s\n", __func__,
                       rep->id, pos, safe_strerror(errno));
            stop_both(clp);
            return true;
        }
    }
    /* enters holding in_mutex */
    while (((res = read(clp->infd, rep->buffp, blocks * rep->bs)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno)))
        std::this_thread::yield();/* another thread may be able to progress */
    if (res < 0) {
        char strerr_buff[STRERR_BUFF_LEN + 1];

        if (clp->in_flags.coe) {
            memset(rep->buffp, 0, rep->num_blks * rep->bs);
            pr2serr_lk("tid=%d: >> substituted zeros for in blk=%" PRId64
                      " for %d bytes, %s\n", rep->id, rep->iblk,
                       rep->num_blks * rep->bs,
                       tsafe_strerror(errno, strerr_buff));
            res = rep->num_blks * rep->bs;
        }
        else {
            pr2serr_lk("tid=%d: error in normal read, %s\n", rep->id,
                       tsafe_strerror(errno, strerr_buff));
            stop_both(clp);
            return true;
        }
    }
    if (res < blocks * rep->bs) {
        // int o_blocks = blocks;

        stop_after_write = true;
        blocks = res / rep->bs;
        if ((res % rep->bs) > 0) {
            blocks++;
            clp->in_partial++;
        }
        /* Reverse out + re-apply blocks on clp */
        // clp->in_blk -= o_blocks;
        // clp->in_count += o_blocks;
        rep->num_blks = blocks;
        // clp->in_blk += blocks;
        // clp->in_count -= blocks;
    }
    clp->in_rem_count -= blocks;
    return stop_after_write;
}

static void
normal_out_wr(Rq_elem * rep, int blocks)
{
    int res;
    struct global_collection * clp = rep->clp;

    /* enters holding out_mutex */
    if (clp->verbose > 4)
        pr2serr_lk("%s: tid=%d: oblk=%" PRIu64 ", blocks=%d\n", __func__,
                   rep->id, rep->oblk, blocks);
    while (((res = write(clp->outfd, rep->buffp, blocks * rep->bs))
            < 0) && ((EINTR == errno) || (EAGAIN == errno)))
        std::this_thread::yield();/* another thread may be able to progress */
    if (res < 0) {
        char strerr_buff[STRERR_BUFF_LEN + 1];

        if (clp->out_flags.coe) {
            pr2serr_lk("tid=%d: >> ignored error for out blk=%" PRId64
                       " for %d bytes, %s\n", rep->id, rep->oblk,
                       rep->num_blks * rep->bs,
                       tsafe_strerror(errno, strerr_buff));
            res = rep->num_blks * rep->bs;
        }
        else {
            pr2serr_lk("tid=%d: error normal write, %s\n", rep->id,
                       tsafe_strerror(errno, strerr_buff));
            stop_both(clp);
            return;
        }
    }
    if (res < blocks * rep->bs) {
        blocks = res / rep->bs;
        if ((res % rep->bs) > 0) {
            blocks++;
            clp->out_partial++;
        }
        rep->num_blks = blocks;
    }
    clp->out_rem_count -= blocks;
}

static int
sg_build_scsi_cdb(uint8_t * cdbp, int cdb_sz, unsigned int blocks,
                  int64_t start_block, bool ver_true, bool write_true,
                  bool fua, bool dpo)
{
    int rd_opcode[] = {0x8, 0x28, 0xa8, 0x88};
    int ve_opcode[] = {0xff /* no VER(6) */, 0x2f, 0xaf, 0x8f};
    int wr_opcode[] = {0xa, 0x2a, 0xaa, 0x8a};
    int sz_ind;

    memset(cdbp, 0, cdb_sz);
    if (ver_true) {     /* only support VERIFY(10) */
        if (cdb_sz < 10) {
            pr2serr_lk("%sonly support VERIFY(10)\n", my_name);
            return 1;
        }
        cdb_sz = 10;
        fua = false;
        cdbp[1] |= 0x2; /* BYTCHK=1 --> sending dout for comparison */
        cdbp[0] = ve_opcode[1];
    }
    if (dpo)
        cdbp[1] |= 0x10;
    if (fua)
        cdbp[1] |= 0x8;
    switch (cdb_sz) {
    case 6:
        sz_ind = 0;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                         rd_opcode[sz_ind]);
        sg_put_unaligned_be24(0x1fffff & start_block, cdbp + 1);
        cdbp[4] = (256 == blocks) ? 0 : (uint8_t)blocks;
        if (blocks > 256) {
            pr2serr_lk("%sfor 6 byte commands, maximum number of blocks is "
                       "256\n", my_name);
            return 1;
        }
        if ((start_block + blocks - 1) & (~0x1fffff)) {
            pr2serr_lk("%sfor 6 byte commands, can't address blocks beyond "
                       "%d\n", my_name, 0x1fffff);
            return 1;
        }
        if (dpo || fua) {
            pr2serr_lk("%sfor 6 byte commands, neither dpo nor fua bits "
                       "supported\n", my_name);
            return 1;
        }
        break;
    case 10:
        if (! ver_true) {
            sz_ind = 1;
            cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                             rd_opcode[sz_ind]);
        }
        sg_put_unaligned_be32((uint32_t)start_block, cdbp + 2);
        sg_put_unaligned_be16((uint16_t)blocks, cdbp + 7);
        if (blocks & (~0xffff)) {
            pr2serr_lk("%sfor 10 byte commands, maximum number of blocks is "
                       "%d\n", my_name, 0xffff);
            return 1;
        }
        break;
    case 12:
        sz_ind = 2;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                         rd_opcode[sz_ind]);
        sg_put_unaligned_be32((uint32_t)start_block, cdbp + 2);
        sg_put_unaligned_be32((uint32_t)blocks, cdbp + 6);
        break;
    case 16:
        sz_ind = 3;
        cdbp[0] = (uint8_t)(write_true ? wr_opcode[sz_ind] :
                                         rd_opcode[sz_ind]);
        sg_put_unaligned_be64((uint64_t)start_block, cdbp + 2);
        sg_put_unaligned_be32((uint32_t)blocks, cdbp + 10);
        break;
    default:
        pr2serr_lk("%sexpected cdb size of 6, 10, 12, or 16 but got %d\n",
                   my_name, cdb_sz);
        return 1;
    }
    return 0;
}

/* Enters this function holding in_mutex */
static void
sg_in_rd_cmd(struct global_collection * clp, Rq_elem * rep,
             mrq_arr_t & def_arr)
{
    int status, pack_id;

    while (1) {
        int res = sg_start_io(rep, def_arr, pack_id, NULL);

        if (1 == res)
            err_exit(ENOMEM, "sg starting in command");
        else if (res < 0) {
            pr2serr_lk("tid=%d: inputting to sg failed, blk=%" PRId64 "\n",
                       rep->id, rep->iblk);
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            stop_both(clp);
            return;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(&clp->in_mutex);
        if (0 != status) err_exit(status, "unlock in_mutex");

        res = sg_finish_io(rep->wr, rep, pack_id, NULL);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire in mutex for balance */
            /* N.B. This re-read could now be out of read sequence */
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            break;      /* will loop again */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->in_flags.coe) {
                pr2serr_lk("error finishing sg in command (medium)\n");
                if (exit_status <= 0)
                    exit_status = res;
                stop_both(clp);
                return;
            } else {
                memset(get_buffp(rep), 0, rep->num_blks * rep->bs);
                pr2serr_lk("tid=%d: >> substituted zeros for in blk=%" PRId64
                           " for %d bytes\n", rep->id, rep->iblk,
                           rep->num_blks * rep->bs);
            }
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case 0:
            status = pthread_mutex_lock(&clp->in_mutex);
            if (0 != status) err_exit(status, "lock in_mutex");
            if (rep->dio_incomplete_count || rep->resid) {
                clp->dio_incomplete_count += rep->dio_incomplete_count;
                clp->sum_of_resids += rep->resid;
            }
            clp->in_rem_count -= rep->num_blks;
            status = pthread_mutex_unlock(&clp->in_mutex);
            if (0 != status) err_exit(status, "unlock in_mutex");
            return;
        default:
            pr2serr_lk("tid=%d: error finishing sg in command (%d)\n",
                       rep->id, res);
            if (exit_status <= 0)
                exit_status = res;
            stop_both(clp);
            return;
        }
    }           /* end of while (1) loop */
}

static bool
sg_wr_swap_share(Rq_elem * rep, int to_fd, bool before)
{
    bool not_first = false;
    int err = 0;
    int k;
    int read_side_fd = rep->infd;
    struct global_collection * clp = rep->clp;
    struct sg_extended_info sei {};
    struct sg_extended_info * seip = &sei;

    if (rep->clp->verbose > 2)
        pr2serr_lk("%s: tid=%d: to_fd=%d, before=%d\n", __func__, rep->id,
                   to_fd, (int)before);
    seip->sei_wr_mask |= SG_SEIM_CHG_SHARE_FD;
    seip->sei_rd_mask |= SG_SEIM_CHG_SHARE_FD;
    seip->share_fd = to_fd;
    if (before) {
        /* clear READ_SIDE_FINI bit: puts read-side in SG_RQ_SHR_SWAP state */
        seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
        seip->sei_rd_mask |= SG_SEIM_CTL_FLAGS;
        seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_READ_SIDE_FINI;
        seip->ctl_flags &= ~SG_CTL_FLAGM_READ_SIDE_FINI;/* would be 0 anyway */
    }
    for (k = 0; (ioctl(read_side_fd, SG_SET_GET_EXTENDED, seip) < 0) &&
                 (EBUSY == errno); ++k) {
        err = errno;
        if (k > 10000)
            break;
        if (! not_first) {
            if (clp->verbose > 3)
                pr2serr_lk("tid=%d: ioctl(EXTENDED(change_shared_fd=%d), "
                           "failed errno=%d %s\n", rep->id, read_side_fd, err,
                           strerror(err));
            not_first = true;
        }
        err = 0;
        std::this_thread::yield();/* another thread may be able to progress */
    }
    if (err) {
        pr2serr_lk("tid=%d: ioctl(EXTENDED(change_shared_fd=%d), failed "
                   "errno=%d %s\n", rep->id, read_side_fd, err, strerror(err));
        return false;
    }
    if (clp->verbose > 15)
        pr2serr_lk("%s: tid=%d: ioctl(EXTENDED(change_shared_fd)) ok, "
                   "read_side_fd=%d, to_write_side_fd=%d\n", __func__, rep->id,
                   read_side_fd, to_fd);
    return true;
}

/* Enters this function holding out_mutex */
static void
sg_out_wr_cmd(Rq_elem * rep, mrq_arr_t & def_arr, bool is_wr2, bool prefetch)
{
    int res, status, pack_id, nblks;
    struct global_collection * clp = rep->clp;
    uint32_t ofsplit = clp->ofsplit;
    pthread_mutex_t * mutexp = is_wr2 ? &clp->out2_mutex : &clp->out_mutex;
    struct sg_io_extra xtr {};
    struct sg_io_extra * xtrp = &xtr;
    const char * wr_or_ver = clp->verify ? "verify" : "out";

    xtrp->is_wr2 = is_wr2;
    xtrp->prefetch = prefetch;
    nblks = rep->num_blks;
    if (rep->has_share && is_wr2)
        sg_wr_swap_share(rep, rep->out2fd, true);

    if (prefetch) {
again:
        res = sg_start_io(rep, def_arr, pack_id, xtrp);
        if (1 == res)
            err_exit(ENOMEM, "sg starting out command");
        else if (res < 0) {
            pr2serr_lk("%ssg %s failed, blk=%" PRId64 "\n",
                       my_name, wr_or_ver, rep->oblk);
            status = pthread_mutex_unlock(mutexp);
            if (0 != status) err_exit(status, "unlock out_mutex");
            stop_both(clp);
            goto fini;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(mutexp);
        if (0 != status) err_exit(status, "unlock out_mutex");

        res = sg_finish_io(rep->wr, rep, pack_id, xtrp);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire out mutex for balance */
            /* N.B. This re-write could now be out of write sequence */
            status = pthread_mutex_lock(mutexp);
            if (0 != status) err_exit(status, "lock out_mutex");
            goto again;
        case SG_LIB_CAT_CONDITION_MET:
        case 0:
            status = pthread_mutex_lock(mutexp);
            if (0 != status) err_exit(status, "unlock out_mutex");
            break;
        default:
            pr2serr_lk("error finishing sg prefetch command (%d)\n", res);
            if (exit_status <= 0)
                exit_status = res;
            stop_both(clp);
            goto fini;
        }
    }

    /* start write (or verify) on current segment on sg device */
    xtrp->prefetch = false;
    if ((ofsplit > 0) && (rep->num_blks > (int)ofsplit)) {
        xtrp->dout_is_split = true;
        xtrp->blk_offset = 0;
        xtrp->blks = ofsplit;
        nblks = ofsplit;
        xtrp->hpv4_ind = 0;
    }
split_upper:
    while (1) {
        res = sg_start_io(rep, def_arr, pack_id, xtrp);
        if (1 == res)
            err_exit(ENOMEM, "sg starting out command");
        else if (res < 0) {
            pr2serr_lk("%ssg %s failed, blk=%" PRId64 "\n", my_name,
                       wr_or_ver, rep->oblk);
            status = pthread_mutex_unlock(mutexp);
            if (0 != status) err_exit(status, "unlock out_mutex");
            stop_both(clp);
            goto fini;
        }
        /* Now release in mutex to let other reads run in parallel */
        status = pthread_mutex_unlock(mutexp);
        if (0 != status) err_exit(status, "unlock out_mutex");

        res = sg_finish_io(rep->wr, rep, pack_id, xtrp);
        switch (res) {
        case SG_LIB_CAT_ABORTED_COMMAND:
        case SG_LIB_CAT_UNIT_ATTENTION:
            /* try again with same addr, count info */
            /* now re-acquire out mutex for balance */
            /* N.B. This re-write could now be out of write sequence */
            status = pthread_mutex_lock(mutexp);
            if (0 != status) err_exit(status, "lock out_mutex");
            break;      /* loops around */
        case SG_LIB_CAT_MEDIUM_HARD:
            if (0 == clp->out_flags.coe) {
                pr2serr_lk("error finishing sg %s command (medium)\n",
                           wr_or_ver);
                if (exit_status <= 0)
                    exit_status = res;
                stop_both(clp);
                goto fini;
            } else
                pr2serr_lk(">> ignored error for %s blk=%" PRId64 " for %d "
                           "bytes\n", wr_or_ver, rep->oblk, nblks * rep->bs);
#if defined(__GNUC__)
#if (__GNUC__ >= 7)
            __attribute__((fallthrough));
            /* FALL THROUGH */
#endif
#endif
        case SG_LIB_CAT_CONDITION_MET:
        case 0:
            if (! is_wr2) {
                status = pthread_mutex_lock(mutexp);
                if (0 != status) err_exit(status, "lock out_mutex");
                if (rep->dio_incomplete_count || rep->resid) {
                    clp->dio_incomplete_count += rep->dio_incomplete_count;
                    clp->sum_of_resids += rep->resid;
                }
                clp->out_rem_count -= nblks;
                status = pthread_mutex_unlock(mutexp);
                if (0 != status) err_exit(status, "unlock out_mutex");
            }
            goto fini;
        case SG_LIB_CAT_MISCOMPARE:
            ++num_miscompare;
            // fall through
        default:
            pr2serr_lk("error finishing sg %s command (%d)\n", wr_or_ver,
                       res);
            if (exit_status <= 0)
                exit_status = res;
            stop_both(clp);
            goto fini;
        }
    }           /* end of while (1) loop */
fini:
    if (xtrp->dout_is_split) {  /* set up upper half of split */
        if ((0 == xtrp->hpv4_ind) && (rep->num_blks > (int)ofsplit)) {
            xtrp->hpv4_ind = 1;
            xtrp->blk_offset = ofsplit;
            xtrp->blks = rep->num_blks - ofsplit;
            nblks = xtrp->blks;
            status = pthread_mutex_lock(mutexp);
            if (0 != status) err_exit(status, "lock out_mutex");
            goto split_upper;
        }
    }
    if (rep->has_share && is_wr2)
        sg_wr_swap_share(rep, rep->outfd, false);
}

static int
process_mrq_response(Rq_elem * rep, const struct sg_io_v4 * ctl_v4p,
                     const struct sg_io_v4 * a_v4p, int num_mrq,
                     uint32_t & good_inblks, uint32_t & good_outblks)
{
    struct global_collection * clp = rep->clp;
    bool ok, all_good;
    bool sb_in_co = !!(ctl_v4p->response);
    int id = rep->id;
    int resid = ctl_v4p->din_resid;
    int sres = ctl_v4p->spare_out;
    int n_subm = num_mrq - ctl_v4p->dout_resid;
    int n_cmpl = ctl_v4p->info;
    int n_good = 0;
    int hole_count = 0;
    int vb = clp->verbose;
    int k, j, f1, slen, blen;
    char b[160];

    blen = sizeof(b);
    good_inblks = 0;
    good_outblks = 0;
    if (vb > 2)
        pr2serr_lk("[thread_id=%d] %s: num_mrq=%d, n_subm=%d, n_cmpl=%d\n",
                   id, __func__, num_mrq, n_subm, n_cmpl);
    if (n_subm < 0) {
        pr2serr_lk("[%d] co.dout_resid(%d) > num_mrq(%d)\n", id,
                   ctl_v4p->dout_resid, num_mrq);
        return -1;
    }
    if (n_cmpl != (num_mrq - resid))
        pr2serr_lk("[%d] co.info(%d) != (num_mrq(%d) - co.din_resid(%d))\n"
                   "will use co.info\n", id, n_cmpl, num_mrq, resid);
    if (n_cmpl > n_subm) {
        pr2serr_lk("[%d] n_cmpl(%d) > n_subm(%d), use n_subm for both\n",
                   id, n_cmpl, n_subm);
        n_cmpl = n_subm;
    }
    if (sres) {
        pr2serr_lk("[%d] secondary error: %s [%d], info=0x%x\n", id,
                   strerror(sres), sres, ctl_v4p->info);
        if (E2BIG == sres) {
            sg_take_snap(rep->infd, id, true);
            sg_take_snap(rep->outfd, id, true);
        }
    }
    /* Check if those submitted have finished or not. N.B. If there has been
     * an error then there may be "holes" (i.e. info=0x0) in the array due
     * to completions being out-of-order. */
    for (k = 0, j = 0; ((k < num_mrq) && (j < n_subm));
         ++k, j += f1, ++a_v4p) {
        slen = a_v4p->response_len;
        if (! (SG_INFO_MRQ_FINI & a_v4p->info))
            ++hole_count;
        ok = true;
        f1 = !!(a_v4p->info);   /* want to skip n_subm count if info is 0x0 */
        if (SG_INFO_CHECK & a_v4p->info) {
            ok = false;
            pr2serr_lk("[%d] a_v4[%d]: SG_INFO_CHECK set [%s]\n", id, k,
                       sg_info_str(a_v4p->info, sizeof(b), b));
        }
        if (sg_scsi_status_is_bad(a_v4p->device_status) ||
            a_v4p->transport_status || a_v4p->driver_status) {
            ok = false;
            if (SAM_STAT_CHECK_CONDITION != a_v4p->device_status) {
                pr2serr_lk("[%d] a_v4[%d]:\n", id, k);
                if (vb)
                    lk_chk_n_print4("  >>", a_v4p, vb > 4);
            }
        }
        if (slen > 0) {
            struct sg_scsi_sense_hdr ssh;
            const uint8_t *sbp = (const uint8_t *)
                        (sb_in_co ? ctl_v4p->response : a_v4p->response);

            if (sg_scsi_normalize_sense(sbp, slen, &ssh) &&
                (ssh.response_code >= 0x70)) {
                if (ssh.response_code & 0x1)
                    ok = true;
                if (vb) {
                    sg_get_sense_str("  ", sbp, slen, false, blen, b);
                    pr2serr_lk("[%d] a_v4[%d]:\n%s\n", id, k, b);
                }
            }
        }
        if (ok && f1) {
            ++n_good;
            if (a_v4p->dout_xfer_len >= (uint32_t)rep->bs)
                good_outblks += (a_v4p->dout_xfer_len - a_v4p->dout_resid) /
                                rep->bs;
            if (a_v4p->din_xfer_len >= (uint32_t)rep->bs)
                good_inblks += (a_v4p->din_xfer_len - a_v4p->din_resid) /
                               rep->bs;
        }
    }   /* end of request array scan loop */
    if ((n_subm == num_mrq) || (vb < 3))
        goto fini;
    pr2serr_lk("[%d] checking response array _beyond_ number of "
               "submissions [%d] to num_mrq:\n", id, k);
    for (all_good = true; k < num_mrq; ++k, ++a_v4p) {
        if (SG_INFO_MRQ_FINI & a_v4p->info) {
            pr2serr_lk("[%d] a_v4[%d]: unexpected SG_INFO_MRQ_FINI set [%s]\n",
                       id, k, sg_info_str(a_v4p->info, sizeof(b), b));
            all_good = false;
        }
        if (a_v4p->device_status || a_v4p->transport_status ||
            a_v4p->driver_status) {
            pr2serr_lk("[%d] a_v4[%d]:\n", id, k);
            lk_chk_n_print4("    ", a_v4p, vb > 4);
            all_good = false;
        }
    }
    if (all_good)
        pr2serr_lk("    ... all good\n");
fini:
    return n_good;
}

#if 0
static int
chk_mrq_response(Rq_elem * rep, const struct sg_io_v4 * ctl_v4p,
                 const struct sg_io_v4 * a_v4p, int nrq,
                 uint32_t * good_inblksp, uint32_t * good_outblksp)
{
    struct global_collection * clp = rep->clp;
    bool ok;
    int id = rep->id;
    int resid = ctl_v4p->din_resid;
    int sres = ctl_v4p->spare_out;
    int n_subm = nrq - ctl_v4p->dout_resid;
    int n_cmpl = ctl_v4p->info;
    int n_good = 0;
    int vb = clp->verbose;
    int k, slen, blen;
    uint32_t good_inblks = 0;
    uint32_t good_outblks = 0;
    const struct sg_io_v4 * a_np = a_v4p;
    char b[80];

    blen = sizeof(b);
    if (n_subm < 0) {
        pr2serr_lk("[%d] %s: co.dout_resid(%d) > nrq(%d)\n", id, __func__,
                   ctl_v4p->dout_resid, nrq);
        return -1;
    }
    if (n_cmpl != (nrq - resid))
        pr2serr_lk("[%d] %s: co.info(%d) != (nrq(%d) - co.din_resid(%d))\n"
                   "will use co.info\n", id, __func__, n_cmpl, nrq, resid);
    if (n_cmpl > n_subm) {
        pr2serr_lk("[%d] %s: n_cmpl(%d) > n_subm(%d), use n_subm for both\n",
                   id, __func__, n_cmpl, n_subm);
        n_cmpl = n_subm;
    }
    if (sres) {
        pr2serr_lk("[%d] %s: secondary error: %s [%d], info=0x%x\n", id,
                   __func__, strerror(sres), sres, ctl_v4p->info);
        if (E2BIG == sres) {
            sg_take_snap(rep->infd, id, true);
            sg_take_snap(rep->outfd, id, true);
        }
    }
    /* Check if those submitted have finished or not */
    for (k = 0; k < n_subm; ++k, ++a_np) {
        slen = a_np->response_len;
        if (! (SG_INFO_MRQ_FINI & a_np->info)) {
            pr2serr_lk("[%d] %s, a_n[%d]: missing SG_INFO_MRQ_FINI [%s]\n",
                       id, __func__, k, sg_info_str(a_np->info, blen, b));
            v4hdr_out_lk("a_np", a_np, id);
            v4hdr_out_lk("cop", ctl_v4p, id);
        }
        ok = true;
        if (SG_INFO_CHECK & a_np->info) {
            ok = false;
            pr2serr_lk("[%d] a_n[%d]: SG_INFO_CHECK set [%s]\n", id, k,
                       sg_info_str(a_np->info, sizeof(b), b));
        }
        if (sg_scsi_status_is_bad(a_np->device_status) ||
            a_np->transport_status || a_np->driver_status) {
            ok = false;
            if (SAM_STAT_CHECK_CONDITION != a_np->device_status) {
                pr2serr_lk("[%d] %s, a_n[%d]:\n", id, __func__, k);
                if (vb)
                    lk_chk_n_print4("  >>", a_np, false);
            }
        }
        if (slen > 0) {
            struct sg_scsi_sense_hdr ssh;
            const uint8_t *sbp = (const uint8_t *)a_np->response;

            if (sg_scsi_normalize_sense(sbp, slen, &ssh) &&
                (ssh.response_code >= 0x70)) {
                char b[256];

                if (ssh.response_code & 0x1)
                    ok = true;
                if (vb) {
                    sg_get_sense_str("  ", sbp, slen, false, sizeof(b), b);
                    pr2serr_lk("[%d] %s, a_n[%d]:\n%s\n", id, __func__, k, b);
                }
            }
        }
        if (ok) {
            ++n_good;
            if (a_np->dout_xfer_len >= (uint32_t)rep->bs)
                good_outblks += (a_np->dout_xfer_len - a_np->dout_resid) /
                                rep->bs;
            if (a_np->din_xfer_len >= (uint32_t)rep->bs)
                good_inblks += (a_np->din_xfer_len - a_np->din_resid) /
                               rep->bs;
        }
    }
    if ((n_subm == nrq) || (vb < 3))
        goto fini;
    pr2serr_lk("[%d] %s: checking response array beyond number of "
               "submissions:\n", id, __func__);
    for (k = n_subm; k < nrq; ++k, ++a_np) {
        if (SG_INFO_MRQ_FINI & a_np->info)
            pr2serr_lk("[%d] %s, a_n[%d]: unexpected SG_INFO_MRQ_FINI set\n",
                       id, __func__, k);
        if (a_np->device_status || a_np->transport_status ||
            a_np->driver_status) {
            pr2serr_lk("[%d] %s, a_n[%d]:\n", id, __func__, k);
            lk_chk_n_print4("    ", a_np, vb > 4);
        }
    }
fini:
    if (good_inblksp)
        *good_inblksp = good_inblks;
    if (good_outblksp)
        *good_outblksp = good_outblks;
    return n_good;
}
#endif

/* do mrq 'full non-blocking' invocation so both submission and completion
 * is async (i.e. uses SGV4_FLAG_IMMED flag). This type of mrq is
 * restricted to a single file descriptor (i.e. the 'fd' argument). */
static int
sgh_do_async_mrq(Rq_elem * rep, mrq_arr_t & def_arr, int fd,
                 struct sg_io_v4 * ctlop, int nrq)
{
    int half = nrq / 2;
    int k, res, nwait, half_num, rest, err, num_good, b_len;
    const int64_t wait_us = 10;
    uint32_t in_fin_blks, out_fin_blks;
    struct sg_io_v4 * a_v4p;
    struct sg_io_v4 hold_ctlo;
    struct global_collection * clp = rep->clp;
    char b[80];

    hold_ctlo = *ctlop;
    b_len = sizeof(b);
    a_v4p = def_arr.first.data();
    ctlop->flags = SGV4_FLAG_MULTIPLE_REQS;
    if (clp->in_flags.polled || clp->out_flags.polled) {
        /* submit of full non-blocking with POLLED */
        ctlop->flags |= (SGV4_FLAG_IMMED | SGV4_FLAG_POLLED);
        if (!after1 && (clp->verbose > 1)) {
            after1 = true;
            pr2serr_lk("%s: %s\n", __func__, mrq_s_nb_s);
        }
    } else {
        ctlop->flags |= SGV4_FLAG_IMMED;        /* submit non-blocking */
        if (!after1 && (clp->verbose > 1)) {
            after1 = true;
            pr2serr_lk("%s: %s\n", __func__, mrq_s_nb_s);
        }
    }
    if (clp->verbose > 4) {
        pr2serr_lk("%s: Controlling object _before_ ioctl(SG_IOSUBMIT):\n",
                   __func__);
        if (clp->verbose > 5)
            hex2stderr_lk((const uint8_t *)ctlop, sizeof(*ctlop), 1);
        v4hdr_out_lk("Controlling object before", ctlop, rep->id);
    }
    res = ioctl(fd, SG_IOSUBMIT, ctlop);
    if (res < 0) {
        err = errno;
        if (E2BIG == err)
            sg_take_snap(fd, rep->id, true);
        pr2serr_lk("%s: ioctl(SG_IOSUBMIT, %s)-->%d, errno=%d: %s\n", __func__,
                   sg_flags_str(ctlop->flags, b_len, b), res, err,
                   strerror(err));
        return -1;
    }
    /* fetch first half */
    for (k = 0; k < 100000; ++k) {
        ++num_waiting_calls;
        res = ioctl(fd, SG_GET_NUM_WAITING, &nwait);
        if (res < 0) {
            err = errno;
            pr2serr_lk("%s: ioctl(SG_GET_NUM_WAITING)-->%d, errno=%d: %s\n",
                       __func__, res, err, strerror(err));
            return -1;
        }
        if (nwait >= half)
            break;
        this_thread::sleep_for(chrono::microseconds{wait_us});
    }
    ctlop->flags = (SGV4_FLAG_MULTIPLE_REQS | SGV4_FLAG_IMMED);
    res = ioctl(fd, SG_IORECEIVE, ctlop);
    if (res < 0) {
        err = errno;
        if (ENODATA != err) {
            pr2serr_lk("%s: ioctl(SG_IORECEIVE, %s),1-->%d, errno=%d: %s\n",
                       __func__, sg_flags_str(ctlop->flags, b_len, b), res,
                       err, strerror(err));
            return -1;
        }
        half_num = 0;
    } else
        half_num = ctlop->info;
    if (clp->verbose > 4) {
        pr2serr_lk("%s: Controlling object output by ioctl(SG_IORECEIVE),1: "
                   "num_received=%d\n", __func__, half_num);
        if (clp->verbose > 5)
            hex2stderr_lk((const uint8_t *)ctlop, sizeof(*ctlop), 1);
        v4hdr_out_lk("Controlling object after", ctlop, rep->id);
        if (clp->verbose > 5) {
            for (k = 0; k < half_num; ++k) {
                pr2serr_lk("AFTER: def_arr[%d]:\n", k);
                v4hdr_out_lk("normal v4 object", (a_v4p + k), rep->id);
                // hex2stderr_lk((const uint8_t *)(a_v4p + k), sizeof(*a_v4p),
                                  // 1);
            }
        }
    }
    in_fin_blks = 0;
    out_fin_blks = 0;
    num_good = process_mrq_response(rep, ctlop, a_v4p, half_num, in_fin_blks,
                                    out_fin_blks);
    if (clp->verbose > 2)
        pr2serr_lk("%s: >>>1 num_good=%d, in_q/fin blks=%u/%u;  out_q/fin "
                   "blks=%u/%u\n", __func__, num_good, rep->in_mrq_q_blks,
                   in_fin_blks, rep->out_mrq_q_blks, out_fin_blks);

    if (num_good < 0)
        res = -1;
    else if (num_good < half_num) {
        int resid_blks = rep->in_mrq_q_blks - in_fin_blks;

        if (resid_blks > 0)
            gcoll.in_rem_count += resid_blks;
        resid_blks = rep->out_mrq_q_blks - out_fin_blks;
        if (resid_blks > 0)
            gcoll.out_rem_count += resid_blks;

        return -1;
    }

    rest = nrq - half_num;
    if (rest < 1)
        goto fini;
    /* fetch remaining */
    for (k = 0; k < 100000; ++k) {
        ++num_waiting_calls;
        res = ioctl(fd, SG_GET_NUM_WAITING, &nwait);
        if (res < 0) {
            pr2serr_lk("%s: ioctl(SG_GET_NUM_WAITING)-->%d, errno=%d: %s\n",
                       __func__, res, errno, strerror(errno));
            return -1;
        }
        if (nwait >= rest)
            break;
        this_thread::sleep_for(chrono::microseconds{wait_us});
    }
    ctlop = &hold_ctlo;
    ctlop->din_xferp += (half_num * sizeof(struct sg_io_v4));
    ctlop->din_xfer_len -= (half_num * sizeof(struct sg_io_v4));
    ctlop->dout_xferp = ctlop->din_xferp;
    ctlop->dout_xfer_len = ctlop->din_xfer_len;
    ctlop->flags = (SGV4_FLAG_MULTIPLE_REQS | SGV4_FLAG_IMMED);
    res = ioctl(fd, SG_IORECEIVE, ctlop);
    if (res < 0) {
        err = errno;
        if (ENODATA != err) {
            pr2serr_lk("%s: ioctl(SG_IORECEIVE, %s),2-->%d, errno=%d: %s\n",
                       __func__, sg_flags_str(ctlop->flags, b_len, b), res,
                       err, strerror(err));
            return -1;
        }
        half_num = 0;
    } else
        half_num = ctlop->info;
    if (clp->verbose > 4) {
        pr2serr_lk("%s: Controlling object output by ioctl(SG_IORECEIVE),2: "
                   "num_received=%d\n", __func__, half_num);
        if (clp->verbose > 5)
            hex2stderr_lk((const uint8_t *)ctlop, sizeof(*ctlop), 1);
        v4hdr_out_lk("Controlling object after", ctlop, rep->id);
        if (clp->verbose > 5) {
            for (k = 0; k < half_num; ++k) {
                pr2serr_lk("AFTER: def_arr[%d]:\n", k);
                v4hdr_out_lk("normal v4 object", (a_v4p + k), rep->id);
                // hex2stderr_lk((const uint8_t *)(a_v4p + k), sizeof(*a_v4p),
                                  // 1);
            }
        }
    }
    in_fin_blks = 0;
    out_fin_blks = 0;
    num_good = process_mrq_response(rep, ctlop, a_v4p, half_num, in_fin_blks,
                                    out_fin_blks);
    if (clp->verbose > 2)
        pr2serr_lk("%s: >>>2 num_good=%d, in_q/fin blks=%u/%u;  out_q/fin "
                   "blks=%u/%u\n", __func__, num_good, rep->in_mrq_q_blks,
                   in_fin_blks, rep->out_mrq_q_blks, out_fin_blks);

    if (num_good < 0)
        res = -1;
    else if (num_good < half_num) {
        int resid_blks = rep->in_mrq_q_blks - in_fin_blks;

        if (resid_blks > 0)
            gcoll.in_rem_count += resid_blks;
        resid_blks = rep->out_mrq_q_blks - out_fin_blks;
        if (resid_blks > 0)
            gcoll.out_rem_count += resid_blks;

        res = -1;
    }

fini:
    return res;
}

/* Split def_arr into fd_def_arr and o_fd_arr based on whether each element's
 * flags field has SGV4_FLAG_DO_ON_OTHER set. If it is set place in
 * o_fd_def_arr and mask out SGV4_DO_ON_OTHER. Returns number of elements
 * in o_fd_def_arr. */
static int
split_def_arr(const mrq_arr_t & def_arr, mrq_arr_t & fd_def_arr,
              mrq_arr_t & o_fd_def_arr)
{
    int nrq, k;
    int res = 0;
    const struct sg_io_v4 * a_v4p;

    a_v4p = def_arr.first.data();
    nrq = def_arr.first.size();

    for (k = 0; k < nrq; ++k) {
        int flags;
        const struct sg_io_v4 * h4p = a_v4p + k;

        flags = h4p->flags;
        if (flags & SGV4_FLAG_DO_ON_OTHER) {
            o_fd_def_arr.first.push_back(def_arr.first[k]);
            o_fd_def_arr.second.push_back(def_arr.second[k]);
            flags &= ~SGV4_FLAG_DO_ON_OTHER;    /* mask out DO_ON_OTHER */
            o_fd_def_arr.first[res].flags = flags;
            ++res;
        } else {
            fd_def_arr.first.push_back(def_arr.first[k]);
            fd_def_arr.second.push_back(def_arr.second[k]);
        }
    }
    return res;
}

/* This function sets up a multiple request (mrq) transaction and sends it
 * to the pass-through. Returns 0 on success, 1 if ENOMEM error else -1 for
 * other errors. */
static int
sgh_do_deferred_mrq(Rq_elem * rep, mrq_arr_t & def_arr)
{
    bool launch_mrq_abort = false;
    int nrq, k, res, fd, mrq_pack_id, status, id, num_good, b_len;
    uint32_t in_fin_blks, out_fin_blks;
    const int max_cdb_sz = 16;
    struct sg_io_v4 * a_v4p;
    struct sg_io_v4 ctl_v4 {};
    uint8_t * cmd_ap = NULL;
    struct global_collection * clp = rep->clp;
    const char * iosub_str = "iosub_str";
    char b[80];

    id = rep->id;
    b_len = sizeof(b);
    ctl_v4.guard = 'Q';
    a_v4p = def_arr.first.data();
    nrq = def_arr.first.size();
    if (nrq < 1) {
        pr2serr_lk("[%d] %s: strange nrq=0, nothing to do\n", id, __func__);
        return 0;
    }
    if (clp->mrq_cmds) {
        cmd_ap = (uint8_t *)calloc(nrq, max_cdb_sz);
        if (NULL == cmd_ap) {
            pr2serr_lk("[%d] %s: no memory for calloc(%d * 16)\n", id,
                       __func__, nrq);
            return 1;
        }
    }
    for (k = 0; k < nrq; ++k) {
        struct sg_io_v4 * h4p = a_v4p + k;
        uint8_t *cmdp = &def_arr.second[k].front();

        if (clp->mrq_cmds) {
            memcpy(cmd_ap + (k * max_cdb_sz), cmdp, h4p->request_len);
            h4p->request = 0;
        } else
            h4p->request = (uint64_t)cmdp;
        if (clp->verbose > 5) {
            pr2serr_lk("%s%s[%d] def_arr[%d]", ((0 == k) ? __func__ : ""),
                       ((0 == k) ? ": " : ""), id, k);
            if (h4p->din_xferp)
                pr2serr_lk(" [din=0x%p]:\n", (void *)h4p->din_xferp);
            else if (h4p->dout_xferp)
                pr2serr_lk(" [dout=0x%p]:\n", (void *)h4p->dout_xferp);
            else
                pr2serr_lk(":\n");
            hex2stderr_lk((const uint8_t *)h4p, sizeof(*h4p), 1);
        }
    }
    if (rep->both_sg || rep->same_sg)
        fd = rep->infd;         /* assume share to rep->outfd */
    else if (rep->only_in_sg)
        fd = rep->infd;
    else if (rep->only_out_sg)
        fd = rep->outfd;
    else {
        pr2serr_lk("[%d] %s: why am I here? No sg devices\n", id, __func__);
        res = -1;
        goto fini;
    }
    res = 0;
    if (clp->mrq_cmds) {
        ctl_v4.request_len = nrq * max_cdb_sz;
        ctl_v4.request = (uint64_t)cmd_ap;
    }
    ctl_v4.flags = SGV4_FLAG_MULTIPLE_REQS;
    if (! clp->mrq_async) {
        ctl_v4.flags |= SGV4_FLAG_STOP_IF;
        if (clp->in_flags.mrq_svb || clp->out_flags.mrq_svb)
            ctl_v4.flags |= SGV4_FLAG_SHARE;
    }
    ctl_v4.dout_xferp = (uint64_t)a_v4p;        /* request array */
    ctl_v4.dout_xfer_len = nrq * sizeof(*a_v4p);
    ctl_v4.din_xferp = (uint64_t)a_v4p;         /* response array */
    ctl_v4.din_xfer_len = nrq * sizeof(*a_v4p);
    mrq_pack_id = atomic_fetch_add(&mono_mrq_id, 1);
    if ((clp->m_aen > 0) && (MONO_MRQ_ID_INIT != mrq_pack_id) &&
        (0 == ((mrq_pack_id - MONO_MRQ_ID_INIT) % clp->m_aen))) {
        launch_mrq_abort = true;
        if (clp->verbose > 2)
            pr2serr_lk("[%d] %s: Decide to launch MRQ abort thread, "
                       "mrq_id=%d\n", id, __func__, mrq_pack_id);
        memset(&rep->mai, 0, sizeof(rep->mai));
        rep->mai.from_tid = id;
        rep->mai.mrq_id = mrq_pack_id;
        rep->mai.fd = fd;
        rep->mai.debug = clp->verbose;

        status = pthread_create(&rep->mrq_abort_thread_id, NULL,
                                mrq_abort_thread, (void *)&rep->mai);
        if (0 != status) err_exit(status, "pthread_create, sig...");
    }
    ctl_v4.request_extra = launch_mrq_abort ? mrq_pack_id : 0;
    rep->mrq_id = mrq_pack_id;
    if (clp->verbose && rep->both_sg && clp->mrq_async)
        iosub_str = "SG_IOSUBMIT(variable)";
    if (clp->verbose > 4) {
        pr2serr_lk("%s: Controlling object _before_ ioctl(%s):\n",
                   __func__, iosub_str);
        if (clp->verbose > 5)
            hex2stderr_lk((const uint8_t *)&ctl_v4, sizeof(ctl_v4), 1);
        v4hdr_out_lk("Controlling object before", &ctl_v4, id);
    }
    if (clp->mrq_async && (! rep->both_sg)) {
        /* do 'submit non-blocking' or 'full non-blocking' mrq */
        mrq_arr_t fd_def_arr;
        mrq_arr_t o_fd_def_arr;

        /* need to deconstruct def_arr[] into two separate lists, one for
         * the source, the other for the destination. */
        int o_num_fd = split_def_arr(def_arr, fd_def_arr, o_fd_def_arr);
        int num_fd = fd_def_arr.first.size();
        if (num_fd > 0) {
            struct sg_io_v4 fd_ctl = ctl_v4;
            struct sg_io_v4 * aa_v4p = fd_def_arr.first.data();

            for (k = 0; k < num_fd; ++k) {
                struct sg_io_v4 * h4p = aa_v4p + k;
                uint8_t *cmdp = &fd_def_arr.second[k].front();

                if (clp->mrq_cmds) {
                    memcpy(cmd_ap + (k * max_cdb_sz), cmdp, h4p->request_len);
                    h4p->request = 0;
                } else
                    h4p->request = (uint64_t)cmdp;
                if (clp->verbose > 5) {
                    pr2serr_lk("[%d] df_def_arr[%d]:\n", id, k);
                    hex2stderr_lk((const uint8_t *)(aa_v4p + k),
                                  sizeof(*aa_v4p), 1);
                }
            }
            fd_ctl.dout_xferp = (uint64_t)aa_v4p;        /* request array */
            fd_ctl.dout_xfer_len = num_fd * sizeof(*aa_v4p);
            fd_ctl.din_xferp = (uint64_t)aa_v4p;         /* response array */
            fd_ctl.din_xfer_len = num_fd * sizeof(*aa_v4p);
            fd_ctl.request_extra = launch_mrq_abort ? mrq_pack_id : 0;
            /* this is the source side mrq command */
            res = sgh_do_async_mrq(rep, fd_def_arr, fd, &fd_ctl, num_fd);
            rep->in_mrq_q_blks = 0;
            if (res)
                goto fini;
        }
        if (o_num_fd > 0) {
            struct sg_io_v4 o_fd_ctl = ctl_v4;
            struct sg_io_v4 * aa_v4p = o_fd_def_arr.first.data();

            for (k = 0; k < o_num_fd; ++k) {
                struct sg_io_v4 * h4p = aa_v4p + k;
                uint8_t *cmdp = &o_fd_def_arr.second[k].front();

                if (clp->mrq_cmds) {
                    memcpy(cmd_ap + (k * max_cdb_sz), cmdp, h4p->request_len);
                    h4p->request = 0;
                } else
                    h4p->request = (uint64_t)cmdp;
                if (clp->verbose > 5) {
                    pr2serr_lk("[%d] o_fd_def_arr[%d]:\n", id, k);
                    hex2stderr_lk((const uint8_t *)(aa_v4p + k),
                                  sizeof(*aa_v4p), 1);
                }
            }
            o_fd_ctl.dout_xferp = (uint64_t)aa_v4p;     /* request array */
            o_fd_ctl.dout_xfer_len = o_num_fd * sizeof(*aa_v4p);
            o_fd_ctl.din_xferp = (uint64_t)aa_v4p;      /* response array */
            o_fd_ctl.din_xfer_len = o_num_fd * sizeof(*aa_v4p);
            o_fd_ctl.request_extra = launch_mrq_abort ? mrq_pack_id : 0;
            /* this is the destination side mrq command */
            res = sgh_do_async_mrq(rep, o_fd_def_arr, rep->outfd, &o_fd_ctl,
                                   o_num_fd);
            rep->out_mrq_q_blks = 0;
        }
        goto fini;
    }

try_again:
    if (clp->unbalanced_mrq) {
        iosub_str = "SG_IOSUBMIT(variable_blocking)";
        if (!after1 && (clp->verbose > 1)) {
            after1 = true;
            pr2serr_lk("%s: unbalanced %s\n", __func__, mrq_vb_s);
        }
        res = ioctl(fd, SG_IOSUBMIT, &ctl_v4);
    } else {
        if (clp->mrq_async) {
            iosub_str = "SG_IOSUBMIT(variable_blocking)";
            if (!after1 && (clp->verbose > 1)) {
                after1 = true;
                pr2serr_lk("%s: %s\n", __func__, mrq_vb_s);
            }
            res = ioctl(fd, SG_IOSUBMIT, &ctl_v4);
        } else if (clp->in_flags.mrq_svb || clp->out_flags.mrq_svb) {
            iosub_str = "SG_IOSUBMIT(shared_variable_blocking)";
            if (!after1 && (clp->verbose > 1)) {
                after1 = true;
                pr2serr_lk("%s: %s\n", __func__, mrq_svb_s);
            }
            res = ioctl(fd, SG_IOSUBMIT, &ctl_v4);
        } else {
            iosub_str = "SG_IO(ordered_blocking)";
            if (!after1 && (clp->verbose > 1)) {
                after1 = true;
                pr2serr_lk("%s: %s\n", __func__, mrq_blk_s);
            }
            res = ioctl(fd, SG_IO, &ctl_v4);
        }
    }
    if (res < 0) {
        int err = errno;

        if (E2BIG == err)
                sg_take_snap(fd, id, true);
        else if (EBUSY == err) {
            ++num_ebusy;
            std::this_thread::yield();/* allow another thread to progress */
            goto try_again;
        }
        pr2serr_lk("%s: ioctl(%s, %s)-->%d, errno=%d: %s\n",
                   __func__, iosub_str, sg_flags_str(ctl_v4.flags, b_len, b),
                   res, err, strerror(err));
        res = -1;
        goto fini;
    }
    if (clp->verbose && vb_first_time.load()) {
        pr2serr_lk("First controlling object output by ioctl(%s), flags: "
                   "%s\n", iosub_str, sg_flags_str(ctl_v4.flags, b_len, b));
        vb_first_time.store(false);
    } else if (clp->verbose > 4)
        pr2serr_lk("%s: Controlling object output by ioctl(%s):\n",
                   __func__, iosub_str);
    if (clp->verbose > 4) {
        if (clp->verbose > 5)
            hex2stderr_lk((const uint8_t *)&ctl_v4, sizeof(ctl_v4), 1);
        v4hdr_out_lk("Controlling object after", &ctl_v4, id);
        if (clp->verbose > 5) {
            for (k = 0; k < nrq; ++k) {
                pr2serr_lk("AFTER: def_arr[%d]:\n", k);
                v4hdr_out_lk("normal v4 object", (a_v4p + k), id);
                // hex2stderr_lk((const uint8_t *)(a_v4p + k), sizeof(*a_v4p),
                                  // 1);
            }
        }
    }
    in_fin_blks = 0;
    out_fin_blks = 0;
    num_good = process_mrq_response(rep, &ctl_v4, a_v4p, nrq, in_fin_blks,
                                    out_fin_blks);
    if (clp->verbose > 2)
        pr2serr_lk("%s: >>> num_good=%d, in_q/fin blks=%u/%u;  out_q/fin "
                   "blks=%u/%u\n", __func__, num_good, rep->in_mrq_q_blks,
                   in_fin_blks, rep->out_mrq_q_blks, out_fin_blks);

    if (num_good < 0)
        res = -1;
    else if (num_good < nrq) {
        int resid_blks = rep->in_mrq_q_blks - in_fin_blks;

        if (resid_blks > 0)
            gcoll.in_rem_count += resid_blks;
        resid_blks = rep->out_mrq_q_blks - out_fin_blks;
        if (resid_blks > 0)
            gcoll.out_rem_count += resid_blks;

        res = -1;
    }
    rep->in_mrq_q_blks = 0;
    rep->out_mrq_q_blks = 0;
fini:
    def_arr.first.clear();
    def_arr.second.clear();
    if (cmd_ap)
        free(cmd_ap);
    if (launch_mrq_abort) {
        if (clp->verbose > 1)
            pr2serr_lk("[%d] %s: About to join MRQ abort thread, "
                       "mrq_id=%d\n", id, __func__, mrq_pack_id);

        void * vp;      /* not used */
        status = pthread_join(rep->mrq_abort_thread_id, &vp);
        if (0 != status) err_exit(status, "pthread_join");
    }
    return res;
}

/* Returns 0 on success, 1 if ENOMEM error else -1 for other errors. */
static int
sg_start_io(Rq_elem * rep, mrq_arr_t & def_arr, int & pack_id,
            struct sg_io_extra *xtrp)
{
    struct global_collection * clp = rep->clp;
    bool wr = rep->wr;
    bool fua = wr ? clp->out_flags.fua : clp->in_flags.fua;
    bool dpo = wr ? clp->out_flags.dpo : clp->in_flags.dpo;
    bool dio = wr ? clp->out_flags.dio : clp->in_flags.dio;
    bool mmap = wr ? clp->out_flags.mmap : clp->in_flags.mmap;
    bool noxfer = wr ? clp->out_flags.noxfer : clp->in_flags.noxfer;
    bool v4 = wr ? clp->out_flags.v4 : clp->in_flags.v4;
    bool qhead = wr ? clp->out_flags.qhead : clp->in_flags.qhead;
    bool qtail = wr ? clp->out_flags.qtail : clp->in_flags.qtail;
    bool polled = wr ? clp->out_flags.polled : clp->in_flags.polled;
    bool mout_if = wr ? clp->out_flags.mout_if : clp->in_flags.mout_if;
    bool prefetch = xtrp ? xtrp->prefetch : false;
    bool is_wr2 = xtrp ? xtrp->is_wr2 : false;
    int cdbsz = wr ? clp->cdbsz_out : clp->cdbsz_in;
    int flags = 0;
    int res, err, fd, b_len, nblks, blk_off;
    int64_t blk = wr ? rep->oblk : rep->iblk;
    struct sg_io_hdr * hp = &rep->io_hdr;
    struct sg_io_v4 * h4p = &rep->io_hdr4[xtrp ? xtrp->hpv4_ind : 0];
    const char * cp = "";
    const char * crwp;
    char b[80];

    b_len = sizeof(b);
    if (wr) {
        fd = is_wr2 ? rep->out2fd : rep->outfd;
        if (clp->verify) {
            crwp = is_wr2 ? "verifying2" : "verifying";
            if (prefetch)
                crwp = is_wr2 ? "prefetch2" : "prefetch";
        } else
            crwp = is_wr2 ? "writing2" : "writing";
    } else {
        fd = rep->infd;
        crwp = "reading";
    }
    if (qhead)
        qtail = false;          /* qhead takes precedence */

    if (v4 && xtrp && xtrp->dout_is_split) {
        res = sg_build_scsi_cdb(rep->cmd, cdbsz, xtrp->blks,
                                blk + (unsigned int)xtrp->blk_offset,
                                clp->verify, true, fua, dpo);
    } else
        res = sg_build_scsi_cdb(rep->cmd, cdbsz, rep->num_blks, blk,
                                wr ? clp->verify : false, wr, fua, dpo);
    if (res) {
        pr2serr_lk("%sbad cdb build, start_blk=%" PRId64 ", blocks=%d\n",
                   my_name, blk, rep->num_blks);
        return -1;
    }
    if (prefetch) {
        if (cdbsz == 10)
            rep->cmd[0] = SGP_PRE_FETCH10;
        else if (cdbsz == 16)
            rep->cmd[0] = SGP_PRE_FETCH16;
        else {
            pr2serr_lk("%sbad PRE-FETCH build, start_blk=%" PRId64 ", "
                       "blocks=%d\n", my_name, blk, rep->num_blks);
            return -1;
        }
        rep->cmd[1] = 0x2;      /* set IMMED (no fua or dpo) */
    }
    if (mmap && (clp->noshare || (rep->outregfd >= 0)))
        flags |= SG_FLAG_MMAP_IO;
    if (noxfer)
        flags |= SG_FLAG_NO_DXFER;
    if (dio)
        flags |= SG_FLAG_DIRECT_IO;
    if (polled)
        flags |= SGV4_FLAG_POLLED;
    if (qhead)
        flags |= SG_FLAG_Q_AT_HEAD;
    if (qtail)
        flags |= SG_FLAG_Q_AT_TAIL;
    if (mout_if)
        flags |= SGV4_FLAG_META_OUT_IF;
    if (rep->has_share) {
        flags |= SGV4_FLAG_SHARE;
        if (wr)
            flags |= SGV4_FLAG_NO_DXFER;
        else if (rep->outregfd < 0)
            flags |= SGV4_FLAG_NO_DXFER;

        cp = (wr ? " write_side active" : " read_side active");
    } else
        cp = (wr ? " write-side not sharing" : " read_side not sharing");
    if (rep->both_sg) {
        if (wr)
            pack_id = rep->rd_p_id + 1;
        else {
            pack_id = 2 * atomic_fetch_add(&mono_pack_id, 1);
            rep->rd_p_id = pack_id;
        }
    } else
        pack_id = atomic_fetch_add(&mono_pack_id, 1);    /* fetch before */
    rep->rq_id = pack_id;
    nblks = rep->num_blks;
    blk_off = 0;
    if (clp->verbose && 0 == clp->nmrqs && vb_first_time.load()) {
        vb_first_time.store(false);
        pr2serr("First normal IO: %s, flags: %s\n", cp,
                sg_flags_str(flags, b_len, b));
    }
    if (v4) {
        memset(h4p, 0, sizeof(struct sg_io_v4));
        if (clp->nmrqs > 0) {
            if (rep->both_sg && (rep->outfd == fd))
                flags |= SGV4_FLAG_DO_ON_OTHER;
        }
        if (xtrp && xtrp->dout_is_split && (nblks > 0)) {
            if (1 == xtrp->hpv4_ind) {
                flags |= SGV4_FLAG_DOUT_OFFSET;
                blk_off = xtrp->blk_offset;
                h4p->spare_in = clp->bs * blk_off;
            }
            nblks = xtrp->blks;
            if ((0 == xtrp->hpv4_ind) && (nblks < rep->num_blks))
                flags |= SGV4_FLAG_KEEP_SHARE;
        }
        if (clp->ofile2_given && wr && rep->has_share && ! is_wr2)
            flags |= SGV4_FLAG_KEEP_SHARE; /* set on first write only */
        else if (clp->fail_mask & 1)
            flags |= SGV4_FLAG_KEEP_SHARE; /* troublemaking .... */
    } else
        memset(hp, 0, sizeof(struct sg_io_hdr));
    if (clp->verbose > 3) {
        bool lock = true;
        char prefix[128];

        if (4 == clp->verbose) {
            snprintf(prefix, sizeof(prefix), "tid,rq_id=%d,%d: ", rep->id,
                     pack_id);
            lock = false;
        } else {
            prefix[0] = '\0';
            pr2serr_lk("%s tid,rq_id=%d,%d: SCSI %s%s %s, blk=%" PRId64
                       " num_blks=%d\n", __func__, rep->id, pack_id, crwp, cp,
                       sg_flags_str(flags, b_len, b), blk + blk_off, nblks);
        }
        lk_print_command_len(prefix, rep->cmd, cdbsz, lock);
    }
    if (v4)
        goto do_v4;     // <<<<<<<<<<<<<<< look further down

    hp->interface_id = 'S';
    hp->cmd_len = cdbsz;
    hp->cmdp = rep->cmd;
    hp->dxferp = get_buffp(rep);
    hp->dxfer_len = clp->bs * rep->num_blks;
    if (!wr)
        hp->dxfer_direction = SG_DXFER_FROM_DEV;
    else if (prefetch) {
        hp->dxfer_direction = SG_DXFER_NONE;
        hp->dxfer_len = 0;
        hp->dxferp = NULL;
    } else
        hp->dxfer_direction = SG_DXFER_TO_DEV;
    hp->mx_sb_len = sizeof(rep->sb);
    hp->sbp = rep->sb;
    hp->timeout = clp->cmd_timeout;
    hp->usr_ptr = rep;
    hp->pack_id = pack_id;
    hp->flags = flags;

    while (((res = write(fd, hp, sizeof(struct sg_io_hdr))) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno) || (EBUSY == errno))) {
        if (EAGAIN == errno) {
            ++num_start_eagain;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        } else if (EBUSY == errno) {
            ++num_ebusy;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        }
        std::this_thread::yield();/* another thread may be able to progress */
    }
    err = errno;
    if (res < 0) {
        if (ENOMEM == err)
            return 1;
        pr2serr_lk("%s tid=%d: %s %s write(2) failed: %s\n", __func__,
                   rep->id, cp, sg_flags_str(hp->flags, b_len, b),
                   strerror(err));
        return -1;
    }
    return 0;

do_v4:
    h4p->guard = 'Q';
    h4p->request_len = cdbsz;
    h4p->request = (uint64_t)rep->cmd;
    if (wr) {
        if (prefetch) {
            h4p->dout_xfer_len = 0;     // din_xfer_len is also 0
            h4p->dout_xferp = 0;
        } else {
            h4p->dout_xfer_len = clp->bs * nblks;
            h4p->dout_xferp = (uint64_t)get_buffp(rep);
        }
    } else if (nblks > 0) {
        h4p->din_xfer_len = clp->bs * nblks;
        h4p->din_xferp = (uint64_t)get_buffp(rep);
    }
    h4p->max_response_len = sizeof(rep->sb);
    h4p->response = (uint64_t)rep->sb;
    h4p->timeout = clp->cmd_timeout;
    h4p->usr_ptr = (uint64_t)rep;
    h4p->request_extra = pack_id;    /* this is the pack_id */
    h4p->flags = flags;
    if (clp->nmrqs > 0) {
        big_cdb cdb_arr;
        uint8_t * cmdp = &(cdb_arr[0]);

        if (wr)
            rep->out_mrq_q_blks += nblks;
        else
            rep->in_mrq_q_blks += nblks;
        memcpy(cmdp, rep->cmd, cdbsz);
        def_arr.first.push_back(*h4p);
        def_arr.second.push_back(cdb_arr);
        res = 0;
        if ((int)def_arr.first.size() >= clp->nmrqs) {
            res = sgh_do_deferred_mrq(rep, def_arr);
            if (res)
                pr2serr_lk("%s tid=%d: sgh_do_deferred_mrq failed\n",
                           __func__, rep->id);
        }
        return res;
    }
    while (((res = ioctl(fd, SG_IOSUBMIT, h4p)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno) || (EBUSY == errno))) {
        if (EAGAIN == errno) {
            ++num_start_eagain;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        } else if (EBUSY == errno) {
            ++num_ebusy;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        }
        std::this_thread::yield();/* another thread may be able to progress */
    }
    err = errno;
    if (res < 0) {
        if (ENOMEM == err)
            return 1;
        if (E2BIG == err)
            sg_take_snap(fd, rep->id, true);
        pr2serr_lk("%s tid=%d: %s %s ioctl(2) failed: %s\n", __func__,
                   rep->id, cp, sg_flags_str(h4p->flags, b_len, b),
                   strerror(err));
        // v4hdr_out_lk("leadin", h4p, rep->id);
        return -1;
    }
    if ((clp->aen > 0) && (rep->rep_count > 0)) {
        if (0 == (rep->rq_id % clp->aen)) {
            struct timespec tspec = {0, 4000 /* 4 usecs */};

            nanosleep(&tspec, NULL);
#if 0
            struct pollfd a_poll;

            a_poll.fd = fd;
            a_poll.events = POLL_IN;
            a_poll.revents = 0;
            res = poll(&a_poll, 1 /* element */, 1 /* millisecond */);
            if (res < 0)
                pr2serr_lk("%s: poll() failed: %s [%d]\n",
                           __func__, safe_strerror(errno), errno);
            else if (0 == res) { /* timeout, cmd still inflight, so abort */
            }
#endif
            ++num_abort_req;
            res = ioctl(fd, SG_IOABORT, h4p);
            if (res < 0) {
                err = errno;
                if (ENODATA == err) {
                    if (clp->verbose > 2)
                        pr2serr_lk("%s: ioctl(SG_IOABORT) no match on "
                                   "pack_id=%d\n", __func__, pack_id);
                } else
                    pr2serr_lk("%s: ioctl(SG_IOABORT) failed: %s [%d]\n",
                               __func__, safe_strerror(err), err);
            } else {
                ++num_abort_req_success;
                if (clp->verbose > 2)
                    pr2serr_lk("%s: sent ioctl(SG_IOABORT) on rq_id=%d, "
                               "success\n", __func__, pack_id);
            }
        }   /* else got response, too late for timeout, so skip */
    }
    return 0;
}

/* 0 -> successful, SG_LIB_CAT_UNIT_ATTENTION or SG_LIB_CAT_ABORTED_COMMAND
   -> try again, SG_LIB_CAT_NOT_READY, SG_LIB_CAT_MEDIUM_HARD,
   -1 other errors */
static int
sg_finish_io(bool wr, Rq_elem * rep, int pack_id, struct sg_io_extra *xtrp)
{
    struct global_collection * clp = rep->clp;
    bool v4 = wr ? clp->out_flags.v4 : clp->in_flags.v4;
    bool mout_if = wr ? clp->out_flags.mout_if : clp->in_flags.mout_if;
    bool is_wr2 = xtrp ? xtrp->is_wr2 : false;
    bool prefetch = xtrp ? xtrp->prefetch : false;
    int res, fd;
    int64_t blk = wr ? rep->oblk : rep->iblk;
    struct sg_io_hdr io_hdr;
    struct sg_io_hdr * hp;
    struct sg_io_v4 * h4p;
    const char *cp;

    if (wr) {
        fd = is_wr2 ? rep->out2fd : rep->outfd;
        cp = is_wr2 ? "writing2" : "writing";
        if (clp->verify) {
            cp = is_wr2 ? "verifying2" : "verifying";
            if (prefetch)
                cp = is_wr2 ? "prefetch2" : "prefetch";
        }
    } else {
        fd = rep->infd;
        cp = "reading";
    }
    if (v4)
        goto do_v4;
    memset(&io_hdr, 0 , sizeof(struct sg_io_hdr));
    /* FORCE_PACK_ID active set only read packet with matching pack_id */
    io_hdr.interface_id = 'S';
    io_hdr.dxfer_direction = wr ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV;
    io_hdr.pack_id = pack_id;

    while (((res = read(fd, &io_hdr, sizeof(struct sg_io_hdr))) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno) || (EBUSY == errno))) {
        if (EAGAIN == errno) {
            ++num_fin_eagain;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        } else if (EBUSY == errno) {
            ++num_ebusy;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        }
        std::this_thread::yield();/* another thread may be able to progress */
    }
    if (res < 0) {
        perror("finishing io [read(2)] on sg device, error");
        return -1;
    }
    if (rep != (Rq_elem *)io_hdr.usr_ptr)
        err_exit(0, "sg_finish_io: bad usr_ptr, request-response mismatch\n");
    memcpy(&rep->io_hdr, &io_hdr, sizeof(struct sg_io_hdr));
    hp = &rep->io_hdr;

    res = sg_err_category3(hp);
    switch (res) {
    case SG_LIB_CAT_CLEAN:
    case SG_LIB_CAT_CONDITION_MET:
        break;
    case SG_LIB_CAT_RECOVERED:
        lk_chk_n_print3(cp, hp, false);
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        if (clp->verbose > 3)
            lk_chk_n_print3(cp, hp, false);
        return res;
    case SG_LIB_CAT_MISCOMPARE:
        ++num_miscompare;
        // fall through
    case SG_LIB_CAT_NOT_READY:
    default:
        {
            char ebuff[EBUFF_SZ];

            snprintf(ebuff, EBUFF_SZ, "%s blk=%" PRId64, cp, blk);
            lk_chk_n_print3(ebuff, hp, clp->verbose > 1);
            return res;
        }
    }
    if ((wr ? clp->out_flags.dio : clp->in_flags.dio) &&
        (! (hp->info & SG_INFO_DIRECT_IO_MASK)))
        rep->dio_incomplete_count = 1; /* count dios done as indirect IO */
    else
        rep->dio_incomplete_count = 0;
    rep->resid = hp->resid;
    if (clp->verbose > 3)
        pr2serr_lk("%s: tid=%d: completed %s\n", __func__, rep->id, cp);
    return 0;

do_v4:
    if (clp->nmrqs > 0) {
        rep->resid = 0;
        return 0;
    }
    h4p = &rep->io_hdr4[xtrp ? xtrp->hpv4_ind : 0];
    h4p->request_extra = pack_id;
    if (mout_if) {
        h4p->info = 0;
        h4p->din_resid = 0;
    }
    while (((res = ioctl(fd, SG_IORECEIVE, h4p)) < 0) &&
           ((EINTR == errno) || (EAGAIN == errno) || (EBUSY == errno))) {
        if (EAGAIN == errno) {
            ++num_fin_eagain;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        } else if (EBUSY == errno) {
            ++num_ebusy;
#ifdef SGH_DD_SNAP_DEV
            if (0 == (num_ebusy % 1000))
                sg_take_snap(fd, rep->id, (clp->verbose > 2));
#endif
        }
        std::this_thread::yield();/* another thread may be able to progress */
    }
    if (res < 0) {
        perror("finishing io [SG_IORECEIVE] on sg device, error");
        return -1;
    }
    if (mout_if && (0 == h4p->info) && (0 == h4p->din_resid))
        goto all_good;
    if (rep != (Rq_elem *)h4p->usr_ptr)
        err_exit(0, "sg_finish_io: bad usr_ptr, request-response mismatch\n");
    res = sg_err_category_new(h4p->device_status, h4p->transport_status,
                              h4p->driver_status,
                              (const uint8_t *)h4p->response,
                              h4p->response_len);
    switch (res) {
    case SG_LIB_CAT_CLEAN:
    case SG_LIB_CAT_CONDITION_MET:
        break;
    case SG_LIB_CAT_RECOVERED:
        lk_chk_n_print4(cp, h4p, false);
        break;
    case SG_LIB_CAT_ABORTED_COMMAND:
    case SG_LIB_CAT_UNIT_ATTENTION:
        if (clp->verbose > 3)
            lk_chk_n_print4(cp, h4p, false);
        return res;
    case SG_LIB_CAT_MISCOMPARE:
        ++num_miscompare;
        // fall through
    case SG_LIB_CAT_NOT_READY:
    default:
        {
            char ebuff[EBUFF_SZ];

            snprintf(ebuff, EBUFF_SZ, "%s rq_id=%d, blk=%" PRId64, cp,
                     pack_id, blk);
            lk_chk_n_print4(ebuff, h4p, clp->verbose > 1);
            if ((clp->verbose > 4) && h4p->info)
                pr2serr_lk(" info=0x%x sg_info_check=%d direct=%d "
                           "detaching=%d aborted=%d\n", h4p->info,
                           !!(h4p->info & SG_INFO_CHECK),
                           !!(h4p->info & SG_INFO_DIRECT_IO),
                           !!(h4p->info & SG_INFO_DEVICE_DETACHING),
                           !!(h4p->info & SG_INFO_ABORTED));
            return res;
        }
    }
    if ((wr ? clp->out_flags.dio : clp->in_flags.dio) &&
        ! (h4p->info & SG_INFO_DIRECT_IO))
        rep->dio_incomplete_count = 1; /* count dios done as indirect IO */
    else
        rep->dio_incomplete_count = 0;
    rep->resid = h4p->din_resid;
    if (clp->verbose > 4) {
        pr2serr_lk("%s: tid,rq_id=%d,%d: completed %s\n", __func__, rep->id,
                   pack_id, cp);
        if ((clp->verbose > 4) && h4p->info)
            pr2serr_lk(" info=0x%x sg_info_check=%d direct=%d "
                       "detaching=%d aborted=%d\n", h4p->info,
                       !!(h4p->info & SG_INFO_CHECK),
                       !!(h4p->info & SG_INFO_DIRECT_IO),
                       !!(h4p->info & SG_INFO_DEVICE_DETACHING),
                       !!(h4p->info & SG_INFO_ABORTED));
    }
all_good:
    return 0;
}

/* Returns reserved_buffer_size/mmap_size if success, else 0 for failure */
static int
sg_prepare_resbuf(int fd, bool is_in, struct global_collection *clp,
                  uint8_t **mmpp)
{
    static bool done = false;
    bool def_res = is_in ? clp->in_flags.defres : clp->out_flags.defres;
    bool no_dur = is_in ? clp->in_flags.no_dur : clp->out_flags.no_dur;
    bool masync = is_in ? clp->in_flags.masync : clp->out_flags.masync;
    bool wq_excl = is_in ? clp->in_flags.wq_excl : clp->out_flags.wq_excl;
    bool skip_thresh = is_in ? clp->in_flags.no_thresh :
                               clp->out_flags.no_thresh;
    int res, t;
    int num = 0;
    uint8_t *mmp;
    struct sg_extended_info sei {};
    struct sg_extended_info * seip = &sei;

    res = ioctl(fd, SG_GET_VERSION_NUM, &t);
    if ((res < 0) || (t < 40000)) {
        if (ioctl(fd, SG_GET_RESERVED_SIZE, &num) < 0) {
            perror("SG_GET_RESERVED_SIZE ioctl failed");
            return 0;
        }
        if (! done) {
            done = true;
            sg_version_lt_4 = true;
            pr2serr_lk("%ssg driver prior to 4.0.00, reduced functionality\n",
                       my_name);
        }
        goto bypass;
    }
    if (! sg_version_ge_40045)
        goto bypass;
    if (clp->elem_sz >= 4096) {
        seip->sei_rd_mask |= SG_SEIM_SGAT_ELEM_SZ;
        res = ioctl(fd, SG_SET_GET_EXTENDED, seip);
        if (res < 0)
            pr2serr_lk("%s%s: SG_SET_GET_EXTENDED(SGAT_ELEM_SZ) rd "
                       "error: %s\n", my_name, __func__, strerror(errno));
        if (clp->elem_sz != (int)seip->sgat_elem_sz) {
            memset(seip, 0, sizeof(*seip));
            seip->sei_wr_mask |= SG_SEIM_SGAT_ELEM_SZ;
            seip->sgat_elem_sz = clp->elem_sz;
            res = ioctl(fd, SG_SET_GET_EXTENDED, seip);
            if (res < 0)
                pr2serr_lk("%s%s: SG_SET_GET_EXTENDED(SGAT_ELEM_SZ) wr "
                           "error: %s\n", my_name, __func__, strerror(errno));
        }
    }
    if (no_dur || masync || skip_thresh) {
        memset(seip, 0, sizeof(*seip));
        seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
        if (no_dur) {
            seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_NO_DURATION;
            seip->ctl_flags |= SG_CTL_FLAGM_NO_DURATION;
        }
        if (masync) {
            seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_MORE_ASYNC;
            seip->ctl_flags |= SG_CTL_FLAGM_MORE_ASYNC;
        }
        if (wq_excl) {
            seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_EXCL_WAITQ;
            seip->ctl_flags |= SG_CTL_FLAGM_EXCL_WAITQ;
        }
        if (skip_thresh) {
            seip->tot_fd_thresh = 0;
            sei.sei_wr_mask |= SG_SEIM_TOT_FD_THRESH;
        }
        res = ioctl(fd, SG_SET_GET_EXTENDED, seip);
        if (res < 0)
            pr2serr_lk("%s%s: SG_SET_GET_EXTENDED(NO_DURATION) error: %s\n",
                       my_name, __func__, strerror(errno));
    }
bypass:
    if (! def_res) {
        num = clp->bs * clp->bpt;
        res = ioctl(fd, SG_SET_RESERVED_SIZE, &num);
        if (res < 0) {
            perror("sgh_dd: SG_SET_RESERVED_SIZE error");
            return 0;
        } else {
            int nn;

            res = ioctl(fd, SG_GET_RESERVED_SIZE, &nn);
            if (res < 0) {
                perror("sgh_dd: SG_GET_RESERVED_SIZE error");
                return 0;
            }
            if (nn < num) {
                pr2serr_lk("%s: SG_GET_RESERVED_SIZE shows size truncated, "
                           "wanted %d got %d\n", __func__, num, nn);
                return 0;
            }
        }
        if (mmpp) {
            mmp = (uint8_t *)mmap(NULL, num, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
            if (MAP_FAILED == mmp) {
                int err = errno;

                pr2serr_lk("%s%s: sz=%d, fd=%d, mmap() failed: %s\n",
                           my_name, __func__, num, fd, strerror(err));
                return 0;
            }
            *mmpp = mmp;
        }
    }
    t = 1;
    res = ioctl(fd, SG_SET_FORCE_PACK_ID, &t);
    if (res < 0)
        perror("sgh_dd: SG_SET_FORCE_PACK_ID error");
    if (clp->unit_nanosec && sg_version_ge_40045) {
        memset(seip, 0, sizeof(*seip));
        seip->sei_wr_mask |= SG_SEIM_CTL_FLAGS;
        seip->ctl_flags_wr_mask |= SG_CTL_FLAGM_TIME_IN_NS;
        seip->ctl_flags |= SG_CTL_FLAGM_TIME_IN_NS;
        if (ioctl(fd, SG_SET_GET_EXTENDED, seip) < 0) {
            pr2serr_lk("ioctl(EXTENDED(TIME_IN_NS)) failed, errno=%d %s\n",
                       errno, strerror(errno));
        }
    }
    t = 1;
    res = ioctl(fd, SG_SET_DEBUG, &t);  /* more info in the kernel log */
    if (res < 0)
        perror("sgs_dd: SG_SET_DEBUG error");
    return (res < 0) ? 0 : num;
}

static bool
process_flags(const char * arg, struct flags_t * fp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        pr2serr("no flag found\n");
        return false;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "00"))
            fp->zero = true;
        else if (0 == strcmp(cp, "append"))
            fp->append = true;
        else if (0 == strcmp(cp, "coe"))
            fp->coe = true;
        else if (0 == strcmp(cp, "defres"))
            fp->defres = true;
        else if (0 == strcmp(cp, "dio"))
            fp->dio = true;
        else if (0 == strcmp(cp, "direct"))
            fp->direct = true;
        else if (0 == strcmp(cp, "dpo"))
            fp->dpo = true;
        else if (0 == strcmp(cp, "dsync"))
            fp->dsync = true;
        else if (0 == strcmp(cp, "excl"))
            fp->excl = true;
        else if (0 == strcmp(cp, "ff"))
            fp->ff = true;
        else if (0 == strcmp(cp, "fua"))
            fp->fua = true;
        else if (0 == strcmp(cp, "hipri"))
            fp->polled = true;
        else if (0 == strcmp(cp, "masync"))
            fp->masync = true;
        else if (0 == strcmp(cp, "mmap"))
            ++fp->mmap;         /* mmap > 1 stops munmap() being called */
        else if (0 == strcmp(cp, "mrq_imm"))
            fp->mrq_immed = true;
        else if (0 == strcmp(cp, "mrq_immed"))
            fp->mrq_immed = true;
        else if (0 == strcmp(cp, "mrq_svb"))
            fp->mrq_svb = true;
        else if (0 == strcmp(cp, "nodur"))
            fp->no_dur = true;
        else if (0 == strcmp(cp, "no_dur"))
            fp->no_dur = true;
        else if (0 == strcmp(cp, "nocreat"))
            fp->nocreat = true;
        else if (0 == strcmp(cp, "noshare"))
            fp->noshare = true;
        else if (0 == strcmp(cp, "no_share"))
            fp->noshare = true;
        else if (0 == strcmp(cp, "no_thresh"))
            fp->no_thresh = true;
        else if (0 == strcmp(cp, "no-thresh"))
            fp->no_thresh = true;
        else if (0 == strcmp(cp, "nothresh"))
            fp->no_thresh = true;
        else if (0 == strcmp(cp, "no_unshare"))
            fp->no_unshare = true;
        else if (0 == strcmp(cp, "no-unshare"))
            fp->no_unshare = true;
        else if (0 == strcmp(cp, "no_waitq"))
            fp->no_waitq = true;
        else if (0 == strcmp(cp, "no-waitq"))
            fp->no_waitq = true;
        else if (0 == strcmp(cp, "nowaitq"))
            fp->no_waitq = true;
        else if (0 == strcmp(cp, "noxfer"))
            fp->noxfer = true;
        else if (0 == strcmp(cp, "no_xfer"))
            fp->noxfer = true;
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "polled"))
            fp->polled = true;
        else if (0 == strcmp(cp, "qhead"))
            fp->qhead = true;
        else if (0 == strcmp(cp, "qtail"))
            fp->qtail = true;
        else if (0 == strcmp(cp, "random"))
            fp->random = true;
        else if ((0 == strcmp(cp, "mout_if")) || (0 == strcmp(cp, "mout-if")))
            fp->mout_if = true;
        else if (0 == strcmp(cp, "same_fds"))
            fp->same_fds = true;
        else if (0 == strcmp(cp, "swait"))
            fp->swait = true;
        else if (0 == strcmp(cp, "v3"))
            fp->v3 = true;
        else if (0 == strcmp(cp, "v4")) {
            fp->v4 = true;
            fp->v4_given = true;
        } else if (0 == strcmp(cp, "wq_excl"))
            fp->wq_excl = true;
        else {
            pr2serr("unrecognised flag: %s\n", cp);
            return false;
        }
        cp = np;
    } while (cp);
    return true;
}

/* Returns the number of times 'ch' is found in string 's' given the
 * string's length. */
static int
num_chs_in_str(const char * s, int slen, int ch)
{
    int res = 0;

    while (--slen >= 0) {
        if (ch == s[slen])
            ++res;
    }
    return res;
}

static int
sg_in_open(struct global_collection *clp, const char *inf, uint8_t **mmpp,
           int * mmap_lenp)
{
    int fd, n;
    int flags = O_RDWR;

    if (clp->in_flags.direct)
        flags |= O_DIRECT;
    if (clp->in_flags.excl)
        flags |= O_EXCL;
    if (clp->in_flags.dsync)
        flags |= O_SYNC;

    if ((fd = open(inf, flags)) < 0) {
        int err = errno;
        char ebuff[EBUFF_SZ];

        snprintf(ebuff, EBUFF_SZ, "%s: could not open %s for sg reading",
                 __func__, inf);
        perror(ebuff);
        return -sg_convert_errno(err);
    }
    n = sg_prepare_resbuf(fd, true, clp, mmpp);
    if (n <= 0) {
        close(fd);
        return -SG_LIB_FILE_ERROR;
    }
    if (clp->noshare)
        sg_noshare_enlarge(fd, clp->verbose > 3);
    if (mmap_lenp)
        *mmap_lenp = n;
    return fd;
}

static int
sg_out_open(struct global_collection *clp, const char *outf, uint8_t **mmpp,
            int * mmap_lenp)
{
    int fd, n;
    int flags = O_RDWR;

    if (clp->out_flags.direct)
        flags |= O_DIRECT;
    if (clp->out_flags.excl)
        flags |= O_EXCL;
    if (clp->out_flags.dsync)
        flags |= O_SYNC;

    if ((fd = open(outf, flags)) < 0) {
        int err = errno;
        char ebuff[EBUFF_SZ];

        snprintf(ebuff,  EBUFF_SZ, "%s: could not open %s for sg %s",
                 __func__, outf, (clp->verify ? "verifying" : "writing"));
        perror(ebuff);
        return -sg_convert_errno(err);
    }
    n = sg_prepare_resbuf(fd, false, clp, mmpp);
    if (n <= 0) {
        close(fd);
        return -SG_LIB_FILE_ERROR;
    }
    if (clp->noshare)
        sg_noshare_enlarge(fd, clp->verbose > 3);
    if (mmap_lenp)
        *mmap_lenp = n;
    return fd;
}

/* Process arguments given to 'conv=" option. Returns 0 on success,
 * 1 on error. */
static int
process_conv(const char * arg, struct flags_t * ifp, struct flags_t * ofp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        pr2serr("no conversions found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "nocreat"))
            ofp->nocreat = true;
        else if (0 == strcmp(cp, "noerror"))
            ifp->coe = true;         /* will still fail on write error */
        else if (0 == strcmp(cp, "notrunc"))
            ;         /* this is the default action of sg_dd so ignore */
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "sync"))
            ;   /* dd(susv4): pad errored block(s) with zeros but sg_dd does
                 * that by default. Typical dd use: 'conv=noerror,sync' */
        else {
            pr2serr("unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

#define STR_SZ 1024
#define INOUTF_SZ 512

static int
parse_cmdline_sanity(int argc, char * argv[], struct global_collection * clp,
                     char * inf, char * outf, char * out2f, char * outregf)
{
    bool verbose_given = false;
    bool version_given = false;
    bool verify_given = false;
    bool bpt_given = false;
    int ibs = 0;
    int obs = 0;
    int k, keylen, n, res;
    char str[STR_SZ];
    char * key;
    char * buf;
    const char * cp;

    for (k = 1; k < argc; k++) {
        if (argv[k]) {
            strncpy(str, argv[k], STR_SZ);
            str[STR_SZ - 1] = '\0';
        }
        else
            continue;
        for (key = str, buf = key; *buf && *buf != '=';)
            buf++;
        if (*buf)
            *buf++ = '\0';
        keylen = strlen(key);
        if (0 == strcmp(key, "ae")) {
            clp->aen = sg_get_num(buf);
            if (clp->aen < 0) {
                pr2serr("%sbad AEN argument to 'ae=', want 0 or higher\n",
                        my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            cp = strchr(buf, ',');
            if (cp) {
                clp->m_aen = sg_get_num(cp + 1);
                if (clp->m_aen < 0) {
                    pr2serr("%sbad MAEN argument to 'ae=', want 0 or "
                            "higher\n", my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
                clp->m_aen_given = true;
            }
            clp->aen_given = true;
        } else if (0 == strcmp(key, "bpt")) {
            clp->bpt = sg_get_num(buf);
            if ((clp->bpt < 0) || (clp->bpt > MAX_BPT_VALUE)) {
                pr2serr("%sbad argument to 'bpt='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            bpt_given = true;
        } else if (0 == strcmp(key, "bs")) {
            clp->bs = sg_get_num(buf);
            if ((clp->bs < 0) || (clp->bs > MAX_BPT_VALUE)) {
                pr2serr("%sbad argument to 'bs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "cdbsz")) {
            clp->cdbsz_in = sg_get_num(buf);
            if ((clp->cdbsz_in < 6) || (clp->cdbsz_in > 32)) {
                pr2serr("%s'cdbsz' expects 6, 10, 12, 16 or 32\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            clp->cdbsz_out = clp->cdbsz_in;
            clp->cdbsz_given = true;
        } else if (0 == strcmp(key, "coe")) {
            clp->in_flags.coe = !! sg_get_num(buf);
            clp->out_flags.coe = clp->in_flags.coe;
        } else if (0 == strcmp(key, "conv")) {
            if (process_conv(buf, &clp->in_flags, &clp->out_flags)) {
                pr2serr("%s: bad argument to 'conv='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "count")) {
            if (0 != strcmp("-1", buf)) {
                dd_count = sg_get_llnum(buf);
                if ((dd_count < 0) || (dd_count > MAX_COUNT_SKIP_SEEK)) {
                    pr2serr("%sbad argument to 'count='\n", my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
            }   /* treat 'count=-1' as calculate count (same as not given) */
        } else if (0 == strcmp(key, "dio")) {
            clp->in_flags.dio = !! sg_get_num(buf);
            clp->out_flags.dio = clp->in_flags.dio;
        } else if (0 == strcmp(key, "elemsz_kb")) {
            n = sg_get_num(buf);
            if (n < 1) {
                pr2serr("elemsz_kb=EKB wants an integer > 0\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (n & (n - 1)) {
                pr2serr("elemsz_kb=EKB wants EKB to be power of 2\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            clp->elem_sz = n * 1024;
        } else if ((0 == strcmp(key, "fail_mask")) ||
                   (0 == strcmp(key, "fail-mask"))) {
            clp->fail_mask = sg_get_num(buf);
            if (clp->fail_mask < 0) {
                pr2serr("fail_mask: couldn't decode argument\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "fua")) {
            n = sg_get_num(buf);
            if (n & 1)
                clp->out_flags.fua = true;
            if (n & 2)
                clp->in_flags.fua = true;
        } else if (0 == strcmp(key, "ibs")) {
            ibs = sg_get_num(buf);
            if ((ibs < 0) || (ibs > MAX_BPT_VALUE)) {
                pr2serr("%sbad argument to 'ibs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "if")) {
            if ('\0' != inf[0]) {
                pr2serr("Second 'if=' argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else {
                memcpy(inf, buf, INOUTF_SZ);
                inf[INOUTF_SZ - 1] = '\0';      /* noisy compiler */
            }
        } else if (0 == strcmp(key, "iflag")) {
            if (! process_flags(buf, &clp->in_flags)) {
                pr2serr("%sbad argument to 'iflag='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "mrq")) {
            if (isdigit(buf[0]))
                cp = buf;
            else {
                if ('I' == isupper(buf[0]))
                    clp->is_mrq_i = true;
                else if ('O' == isupper(buf[0]))
                    clp->is_mrq_o = true;
                else {
                    pr2serr("%sonly mrq=i,NRQS or mrq=o,NRQS allowed here\n",
                            my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
                cp = strchr(buf, ',');
                ++cp;
            }
            clp->nmrqs = sg_get_num(cp);
            if (clp->nmrqs < 0) {
                pr2serr("%sbad argument to 'mrq='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            cp = strchr(cp, ',');
            if (cp && ('C' == toupper(cp[1])))
                clp->mrq_cmds = true;
        } else if (0 == strcmp(key, "noshare")) {
            clp->noshare = !! sg_get_num(buf);
        } else if (0 == strcmp(key, "obs")) {
            obs = sg_get_num(buf);
            if ((obs < 0) || (obs > MAX_BPT_VALUE)) {
                pr2serr("%sbad argument to 'obs='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (strcmp(key, "of2") == 0) {
            if ('\0' != out2f[0]) {
                pr2serr("Second OFILE2 argument??\n");
                return SG_LIB_CONTRADICT;
            } else {
                memcpy(out2f, buf, INOUTF_SZ);
                out2f[INOUTF_SZ - 1] = '\0';    /* noisy compiler */
            }
        } else if (strcmp(key, "ofreg") == 0) {
            if ('\0' != outregf[0]) {
                pr2serr("Second OFREG argument??\n");
                return SG_LIB_CONTRADICT;
            } else {
                memcpy(outregf, buf, INOUTF_SZ);
                outregf[INOUTF_SZ - 1] = '\0';  /* noisy compiler */
            }
        } else if (0 == strcmp(key, "ofsplit")) {
            clp->ofsplit = sg_get_num(buf);
            if (-1 == clp->ofsplit) {
                pr2serr("%sbad argument to 'ofsplit='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (strcmp(key, "of") == 0) {
            if ('\0' != outf[0]) {
                pr2serr("Second 'of=' argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else {
                memcpy(outf, buf, INOUTF_SZ);
                outf[INOUTF_SZ - 1] = '\0';     /* noisy compiler */
            }
        } else if (0 == strcmp(key, "oflag")) {
            if (! process_flags(buf, &clp->out_flags)) {
                pr2serr("%sbad argument to 'oflag='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "sdt")) {
            cp = strchr(buf, ',');
            n = sg_get_num(buf);
            if (n < 0) {
                pr2serr("%sbad argument to 'sdt=CRT[,ICT]'\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            clp->sdt_crt = n;
            if (cp) {
                n = sg_get_num(cp + 1);
                if (n < 0) {
                    pr2serr("%sbad 2nd argument to 'sdt=CRT,ICT'\n",
                            my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
                clp->sdt_ict = n;
            }
        } else if (0 == strcmp(key, "seek")) {
            clp->seek = sg_get_llnum(buf);
            if (clp->seek < 0) {
                pr2serr("%sbad argument to 'seek='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "skip")) {
            clp->skip = sg_get_llnum(buf);
            if (clp->skip < 0) {
                pr2serr("%sbad argument to 'skip='\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "sync"))
            do_sync = !! sg_get_num(buf);
        else if (0 == strcmp(key, "thr"))
            num_threads = sg_get_num(buf);
        else if (0 == strcmp(key, "time")) {
            do_time = sg_get_num(buf);
            if (do_time < 0) {
                pr2serr("%sbad argument to 'time=0|1|2'\n", my_name);
                return SG_LIB_SYNTAX_ERROR;
            }
            cp = strchr(buf, ',');
            if (cp) {
                n = sg_get_num(cp + 1);
                if (n < 0) {
                    pr2serr("%sbad argument to 'time=0|1|2,TO'\n", my_name);
                    return SG_LIB_SYNTAX_ERROR;
                }
                clp->cmd_timeout = n ? (n * 1000) : DEF_TIMEOUT;
            }
        } else if (0 == strcmp(key, "unshare"))
            clp->unshare = !! sg_get_num(buf);  /* default: true */
        else if (0 == strncmp(key, "verb", 4))
            clp->verbose = sg_get_num(buf);
        else if ((keylen > 1) && ('-' == key[0]) && ('-' != key[1])) {
            res = 0;
            n = num_chs_in_str(key + 1, keylen - 1, 'c');
            clp->chkaddr += n;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'd');
            clp->dry_run += n;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'h');
            clp->help += n;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'p');
            if (n > 0)
                clp->prefetch = true;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'v');
            if (n > 0)
                verbose_given = true;
            clp->verbose += n;   /* -v  ---> --verbose */
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'V');
            if (n > 0)
                version_given = true;
            res += n;
            n = num_chs_in_str(key + 1, keylen - 1, 'x');
            if (n > 0)
                verify_given = true;
            res += n;

            if (res < (keylen - 1)) {
                pr2serr("Unrecognised short option in '%s', try '--help'\n",
                        key);
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strncmp(key, "--chkaddr", 9))
            ++clp->chkaddr;
        else if ((0 == strncmp(key, "--dry-run", 9)) ||
                 (0 == strncmp(key, "--dry_run", 9)))
            ++clp->dry_run;
        else if ((0 == strncmp(key, "--help", 6)) ||
                   (0 == strcmp(key, "-?")))
            ++clp->help;
        else if ((0 == strncmp(key, "--prefetch", 10)) ||
                 (0 == strncmp(key, "--pre-fetch", 11)))
            clp->prefetch = true;
        else if (0 == strncmp(key, "--verb", 6)) {
            verbose_given = true;
            ++clp->verbose;      /* --verbose */
        } else if (0 == strncmp(key, "--veri", 6))
            verify_given = true;
        else if (0 == strncmp(key, "--vers", 6))
            version_given = true;
        else {
            pr2serr("Unrecognized option '%s'\n", key);
            pr2serr("For more information use '--help' or '-h'\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }

#ifdef DEBUG
    pr2serr("In DEBUG mode, ");
    if (verbose_given && version_given) {
        pr2serr("but override: '-vV' given, zero verbose and continue\n");
        verbose_given = false;
        version_given = false;
        clp->verbose = 0;
    } else if (! verbose_given) {
        pr2serr("set '-vv'\n");
        clp->verbose = 2;
    } else
        pr2serr("keep verbose=%d\n", clp->verbose);
#else
    if (verbose_given && version_given)
        pr2serr("Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (version_given) {
        pr2serr("%s%s\n", my_name, version_str);
        return SG_LIB_OK_FALSE;
    }
    if (clp->help > 0) {
        usage(clp->help);
        return SG_LIB_OK_FALSE;
    }
    if (clp->bs <= 0) {
        clp->bs = DEF_BLOCK_SIZE;
        pr2serr("Assume default 'bs' ((logical) block size) of %d bytes\n",
                clp->bs);
    }
    if (verify_given) {
        pr2serr("Doing verify/cmp rather than copy\n");
        clp->verify = true;
    }
    if ((ibs && (ibs != clp->bs)) || (obs && (obs != clp->bs))) {
        pr2serr("If 'ibs' or 'obs' given must be same as 'bs'\n");
        usage(0);
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((clp->skip < 0) || (clp->seek < 0)) {
        pr2serr("skip and seek cannot be negative\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->out_flags.append) {
        if (clp->seek > 0) {
            pr2serr("Can't use both append and seek switches\n");
            return SG_LIB_SYNTAX_ERROR;
        }
        if (verify_given) {
            pr2serr("Can't use both append and verify switches\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (clp->bpt < 1) {
        pr2serr("bpt must be greater than 0\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->in_flags.mmap && clp->out_flags.mmap) {
        pr2serr("mmap flag on both IFILE and OFILE doesn't work\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (! clp->noshare) {
        if (clp->in_flags.noshare || clp->out_flags.noshare)
            clp->noshare = true;
    }
    if (clp->unshare) {
        if (clp->in_flags.no_unshare || clp->out_flags.no_unshare)
            clp->unshare = false;
    }
    if (clp->out_flags.mmap && ! clp->noshare) {
        pr2serr("oflag=mmap needs either noshare=1\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((clp->in_flags.mmap || clp->out_flags.mmap) &&
        (clp->in_flags.same_fds || clp->out_flags.same_fds)) {
        pr2serr("can't have both 'mmap' and 'same_fds' flags\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((! clp->noshare) && (clp->in_flags.dio || clp->out_flags.dio)) {
        pr2serr("dio flag can only be used with noshare=1\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->nmrqs > 0) {
        if (clp->in_flags.mrq_immed || clp->out_flags.mrq_immed)
            clp->mrq_async = true;
    }
    /* defaulting transfer size to 128*2048 for CD/DVDs is too large
       for the block layer in lk 2.6 and results in an EIO on the
       SG_IO ioctl. So reduce it in that case. */
    if ((clp->bs >= 2048) && (! bpt_given))
        clp->bpt = DEF_BLOCKS_PER_2048TRANSFER;
    if (clp->ofsplit >= clp->bpt) {
        pr2serr("ofsplit when given must be less than BPT\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((num_threads < 1) || (num_threads > MAX_NUM_THREADS)) {
        pr2serr("too few or too many threads requested\n");
        usage(1);
        return SG_LIB_SYNTAX_ERROR;
    }
    if (clp->in_flags.swait || clp->out_flags.swait) {
        if (clp->verbose)
            pr2serr("the 'swait' flag is now ignored\n");
        /* remnants ... */
        if (clp->in_flags.swait && (! clp->out_flags.swait))
            clp->out_flags.swait = true;
    }
    clp->unit_nanosec = (do_time > 1) || !!getenv("SG3_UTILS_LINUX_NANO");
#if 0
    if (clp->verbose) {
        pr2serr("%sif=%s skip=%" PRId64 " of=%s seek=%" PRId64 " count=%"
                PRId64, my_name, inf, clp->skip, outf, clp->seek, dd_count);
        if (clp->nmrqs > 0)
            pr2serr(" mrq=%d%s\n", clp->nmrqs, (clp->mrq_cmds ? ",C" : ""));
        else
            pr2serr("\n");
    }
#endif
    return 0;
}


int
main(int argc, char * argv[])
{
    char inf[INOUTF_SZ];
    char outf[INOUTF_SZ];
    char out2f[INOUTF_SZ];
    char outregf[INOUTF_SZ];
    int res, k, err;
    int64_t in_num_sect = 0;
    int64_t out_num_sect = 0;
    int in_sect_sz, out_sect_sz, status, flags;
    void * vp;
    const char * ccp = NULL;
    const char * cc2p;
    struct global_collection * clp = &gcoll;
    Thread_info thread_arr[MAX_NUM_THREADS] {};
    char ebuff[EBUFF_SZ];
#if SG_LIB_ANDROID
    struct sigaction actions;

    memset(&actions, 0, sizeof(actions));
    sigemptyset(&actions.sa_mask);
    actions.sa_flags = 0;
    actions.sa_handler = thread_exit_handler;
    sigaction(SIGUSR1, &actions, NULL);
    sigaction(SIGUSR2, &actions, NULL);
#endif
    /* memset(clp, 0, sizeof(*clp)); */
    clp->bpt = DEF_BLOCKS_PER_TRANSFER;
    clp->cmd_timeout = DEF_TIMEOUT;
    clp->in_type = FT_OTHER;
    /* change dd's default: if of=OFILE not given, assume /dev/null */
    clp->out_type = FT_DEV_NULL;
    clp->out2_type = FT_DEV_NULL;
    clp->cdbsz_in = DEF_SCSI_CDBSZ;
    clp->cdbsz_out = DEF_SCSI_CDBSZ;
    clp->sdt_ict = DEF_SDT_ICT_MS;
    clp->sdt_crt = DEF_SDT_CRT_SEC;
    clp->nmrqs = DEF_NUM_MRQS;
    clp->unshare = true;
    inf[0] = '\0';
    outf[0] = '\0';
    out2f[0] = '\0';
    outregf[0] = '\0';
    fetch_sg_version();
    if (sg_version >= 40045)
        sg_version_ge_40045 = true;

    res = parse_cmdline_sanity(argc, argv, clp, inf, outf, out2f, outregf);
    if (SG_LIB_OK_FALSE == res)
        return 0;
    if (res)
        return res;
    if (sg_version > 40000) {
        if (! clp->in_flags.v3)
            clp->in_flags.v4 = true;
        if (! clp->out_flags.v3)
            clp->out_flags.v4 = true;
    }

    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGQUIT, interrupt_handler);
    install_handler(SIGPIPE, interrupt_handler);
    install_handler(SIGUSR1, siginfo_handler);
    install_handler(SIGUSR2, siginfo2_handler);

    clp->infd = STDIN_FILENO;
    clp->outfd = STDOUT_FILENO;
    if (clp->in_flags.ff && clp->in_flags.zero) {
        ccp = "<addr_as_data>";
        cc2p = "addr_as_data";
    } else if (clp->in_flags.ff) {
        ccp = "<0xff bytes>";
        cc2p = "ff";
    } else if (clp->in_flags.random) {
        ccp = "<random>";
        cc2p = "random";
    } else if (clp->in_flags.zero) {
        ccp = "<zero bytes>";
        cc2p = "00";
    }
    if (ccp) {
        if (inf[0]) {
            pr2serr("%siflag=%s and if=%s contradict\n", my_name, cc2p, inf);
            return SG_LIB_CONTRADICT;
        }
        clp->in_type = FT_RANDOM_0_FF;
        clp->infp = ccp;
        clp->infd = -1;
    } else if (inf[0] && ('-' != inf[0])) {
        clp->in_type = dd_filetype(inf, clp->in_st_size);

        if (FT_ERROR == clp->in_type) {
            pr2serr("%sunable to access %s\n", my_name, inf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_ST == clp->in_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, inf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_CHAR == clp->in_type) {
            pr2serr("%sunable to use unknown char device %s\n", my_name, inf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_SG == clp->in_type) {
            clp->infd = sg_in_open(clp, inf, NULL, NULL);
            if (clp->verbose > 2)
                pr2serr("using sg v%c interface on %s\n",
                        (clp->in_flags.v4 ? '4' : '3'), inf);
            if (clp->infd < 0)
                return -clp->infd;
        } else {
            flags = O_RDONLY;
            if (clp->in_flags.direct)
                flags |= O_DIRECT;
            if (clp->in_flags.excl)
                flags |= O_EXCL;
            if (clp->in_flags.dsync)
                flags |= O_SYNC;

            if ((clp->infd = open(inf, flags)) < 0) {
                err = errno;
                snprintf(ebuff, EBUFF_SZ, "%scould not open %s for reading",
                         my_name, inf);
                perror(ebuff);
                return sg_convert_errno(err);
            } else if (clp->skip > 0) {
                off64_t offset = clp->skip;

                offset *= clp->bs;       /* could exceed 32 here! */
                if (lseek64(clp->infd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't skip to required "
                             "position on %s", my_name, inf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->infp = inf;
        if ((clp->in_flags.v3 || clp->in_flags.v4_given) &&
            (FT_SG != clp->in_type)) {
            clp->in_flags.v3 = false;
            clp->in_flags.v4 = false;
            pr2serr("%siflag= v3 and v4 both ignored when IFILE is not sg "
                    "device\n", my_name);
        }
    }
    if (clp->verbose && (clp->in_flags.no_waitq || clp->out_flags.no_waitq))
        pr2serr("no_waitq: flag no longer does anything\n");
    if (outf[0])
        clp->ofile_given = true;
    if (outf[0] && ('-' != outf[0])) {
        clp->out_type = dd_filetype(outf, clp->out_st_size);

        if ((FT_SG != clp->out_type) && clp->verify) {
            pr2serr("%s --verify only supported by sg OFILEs\n", my_name);
            return SG_LIB_FILE_ERROR;
        } else if (FT_ST == clp->out_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, outf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_CHAR == clp->out_type) {
            pr2serr("%sunable to use unknown char device %s\n", my_name, outf);
            return SG_LIB_FILE_ERROR;
        } else if (FT_SG == clp->out_type) {
            clp->outfd = sg_out_open(clp, outf, NULL, NULL);
            if (clp->verbose > 2)
                pr2serr("using sg v%c interface on %s\n",
                        (clp->out_flags.v4 ? '4' : '3'), outf);
            if (clp->outfd < 0)
                return -clp->outfd;
        } else if (FT_DEV_NULL == clp->out_type)
            clp->outfd = -1; /* don't bother opening */
        else {
            flags = O_WRONLY;
            if (! clp->out_flags.nocreat)
                flags |= O_CREAT;
            if (clp->out_flags.direct)
                flags |= O_DIRECT;
            if (clp->out_flags.excl)
                flags |= O_EXCL;
            if (clp->out_flags.dsync)
                flags |= O_SYNC;
            if (clp->out_flags.append)
                flags |= O_APPEND;

            if ((clp->outfd = open(outf, flags, 0666)) < 0) {
                err = errno;
                snprintf(ebuff, EBUFF_SZ, "%scould not open %s for "
                         "writing", my_name, outf);
                perror(ebuff);
                return sg_convert_errno(err);
            }
            if (clp->seek > 0) {
                off64_t offset = clp->seek;

                offset *= clp->bs;       /* could exceed 32 bits here! */
                if (lseek64(clp->outfd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't seek to required "
                             "position on %s", my_name, outf);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->outfp = outf;
        if ((clp->out_flags.v3 || clp->out_flags.v4_given) &&
            (FT_SG != clp->out_type)) {
            clp->out_flags.v3 = false;
            clp->out_flags.v4 = false;
            pr2serr("%soflag= v3 and v4 both ignored when OFILE is not sg "
                    "device\n", my_name);
        }
    }

    if (out2f[0])
        clp->ofile2_given = true;
    if (out2f[0] && ('-' != out2f[0])) {
        off_t out2_st_size;

        clp->out2_type = dd_filetype(out2f, out2_st_size);
        if (FT_ST == clp->out2_type) {
            pr2serr("%sunable to use scsi tape device %s\n", my_name, out2f);
            return SG_LIB_FILE_ERROR;
        }
        else if (FT_SG == clp->out2_type) {
            clp->out2fd = sg_out_open(clp, out2f, NULL, NULL);
            if (clp->out2fd < 0)
                return -clp->out2fd;
        }
        else if (FT_DEV_NULL == clp->out2_type)
            clp->out2fd = -1; /* don't bother opening */
        else {
            flags = O_WRONLY;
            if (! clp->out_flags.nocreat)
                flags |= O_CREAT;
            if (clp->out_flags.direct)
                flags |= O_DIRECT;
            if (clp->out_flags.excl)
                flags |= O_EXCL;
            if (clp->out_flags.dsync)
                flags |= O_SYNC;
            if (clp->out_flags.append)
                flags |= O_APPEND;

            if ((clp->out2fd = open(out2f, flags, 0666)) < 0) {
                err = errno;
                snprintf(ebuff, EBUFF_SZ, "%scould not open %s for "
                         "writing", my_name, out2f);
                perror(ebuff);
                return sg_convert_errno(err);
            }
            if (clp->seek > 0) {
                off64_t offset = clp->seek;

                offset *= clp->bs;       /* could exceed 32 bits here! */
                if (lseek64(clp->out2fd, offset, SEEK_SET) < 0) {
                    err = errno;
                    snprintf(ebuff, EBUFF_SZ, "%scouldn't seek to required "
                             "position on %s", my_name, out2f);
                    perror(ebuff);
                    return sg_convert_errno(err);
                }
            }
        }
        clp->out2fp = out2f;
    }
    if ((FT_SG == clp->in_type ) && (FT_SG == clp->out_type)) {
        if (clp->nmrqs > 0) {
            if (clp->is_mrq_i == clp->is_mrq_o) {
                if (clp->ofsplit > 0) {
                    if (0 != (clp->nmrqs % 3)) {
                        pr2serr("When both IFILE+OFILE sg devices and OSP>0, "
                                "mrq=NRQS must be divisible by 3\n");
                        pr2serr("    triple NRQS to avoid error\n");
                        clp->nmrqs *= 3;
                    }
                } else if (0 != (clp->nmrqs % 2)) {
                    pr2serr("When both IFILE+OFILE sg devices (and OSP=0), "
                            "mrq=NRQS must be even\n");
                    pr2serr("    double NRQS to avoid error\n");
                    clp->nmrqs *= 2;
                }
            }
            if (clp->is_mrq_i && clp->is_mrq_o)
                ;
            else if (clp->is_mrq_i || clp->is_mrq_o)
                clp->unbalanced_mrq = true;
        }
        if (clp->in_flags.v4_given && (! clp->out_flags.v3)) {
            if (! clp->out_flags.v4_given) {
                clp->out_flags.v4 = true;
                if (clp->verbose)
                    pr2serr("Changing OFILE from v3 to v4, use oflag=v3 to "
                            "force v3\n");
            }
        }
        if (clp->out_flags.v4_given && (! clp->in_flags.v3)) {
            if (! clp->in_flags.v4_given) {
                clp->in_flags.v4 = true;
                if (clp->verbose)
                    pr2serr("Changing IFILE from v3 to v4, use iflag=v3 to "
                            "force v3\n");
            }
        }
#if 0
        if (clp->mrq_async && !(clp->noshare)) {
            pr2serr("With mrq_immed also need noshare on sg-->sg copy\n");
            return SG_LIB_SYNTAX_ERROR;
        }
#endif
    } else if ((FT_SG == clp->in_type ) || (FT_SG == clp->out_type)) {
        if (clp->nmrqs > 0)
            clp->unbalanced_mrq = true;
    }
    if (outregf[0]) {
        off_t outrf_st_size;
        int ftyp = dd_filetype(outregf, outrf_st_size);

        clp->outreg_type = ftyp;
        if (! ((FT_OTHER == ftyp) || (FT_ERROR == ftyp) ||
               (FT_DEV_NULL == ftyp))) {
            pr2serr("File: %s can only be regular file or pipe (or "
                    "/dev/null)\n", outregf);
            return SG_LIB_SYNTAX_ERROR;
        }
        if ((clp->outregfd = open(outregf, O_WRONLY | O_CREAT, 0666)) < 0) {
            err = errno;
            snprintf(ebuff, EBUFF_SZ, "could not open %s for writing",
                     outregf);
            perror(ebuff);
            return sg_convert_errno(err);
        }
        if (clp->verbose > 1)
            pr2serr("ofreg=%s opened okay, fd=%d\n", outregf, clp->outregfd);
        if (FT_ERROR == ftyp)
            clp->outreg_type = FT_OTHER;        /* regular file created */
    } else
        clp->outregfd = -1;

    if ((STDIN_FILENO == clp->infd) && (STDOUT_FILENO == clp->outfd)) {
        pr2serr("Won't default both IFILE to stdin _and_ OFILE to "
                "/dev/null\n");
        pr2serr("For more information use '--help' or '-h'\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (dd_count < 0) {
        in_num_sect = -1;
        if (FT_SG == clp->in_type) {
            res = scsi_read_capacity(clp->infd, &in_num_sect, &in_sect_sz);
            if (2 == res) {
                pr2serr("Unit attention, media changed(in), continuing\n");
                res = scsi_read_capacity(clp->infd, &in_num_sect,
                                         &in_sect_sz);
            }
            if (0 != res) {
                if (res == SG_LIB_CAT_INVALID_OP)
                    pr2serr("read capacity not supported on %s\n", inf);
                else if (res == SG_LIB_CAT_NOT_READY)
                    pr2serr("read capacity failed, %s not ready\n", inf);
                else
                    pr2serr("Unable to read capacity on %s\n", inf);
                return SG_LIB_FILE_ERROR;
            } else if (clp->bs != in_sect_sz) {
                pr2serr(">> warning: logical block size on %s confusion: "
                        "bs=%d, device claims=%d\n", clp->infp, clp->bs,
                        in_sect_sz);
                return SG_LIB_FILE_ERROR;
            }
        } else if (FT_BLOCK == clp->in_type) {
            if (0 != read_blkdev_capacity(clp->infd, &in_num_sect,
                                          &in_sect_sz)) {
                pr2serr("Unable to read block capacity on %s\n", inf);
                in_num_sect = -1;
            }
            if (clp->bs != in_sect_sz) {
                pr2serr("logical block size on %s confusion; bs=%d, from "
                        "device=%d\n", inf, clp->bs, in_sect_sz);
                in_num_sect = -1;
            }
        } else if (FT_OTHER == clp->in_type) {
            in_num_sect = clp->in_st_size / clp->bs;
            if (clp->in_st_size % clp->bs) {
                ++in_num_sect;
                pr2serr("Warning: the file size of %s is not a multiple of BS "
                        "[%d]\n", inf, clp->bs);
            }
        }
        if (in_num_sect > clp->skip)
            in_num_sect -= clp->skip;

        out_num_sect = -1;
        if (FT_SG == clp->out_type) {
            res = scsi_read_capacity(clp->outfd, &out_num_sect, &out_sect_sz);
            if (2 == res) {
                pr2serr("Unit attention, media changed(out), continuing\n");
                res = scsi_read_capacity(clp->outfd, &out_num_sect,
                                         &out_sect_sz);
            }
            if (0 != res) {
                if (res == SG_LIB_CAT_INVALID_OP)
                    pr2serr("read capacity not supported on %s\n", outf);
                else if (res == SG_LIB_CAT_NOT_READY)
                    pr2serr("read capacity failed, %s not ready\n", outf);
                else
                    pr2serr("Unable to read capacity on %s\n", outf);
                out_num_sect = -1;
                return SG_LIB_FILE_ERROR;
            } else if (clp->bs != out_sect_sz) {
                pr2serr(">> warning: logical block size on %s confusion: "
                        "bs=%d, device claims=%d\n", clp->outfp, clp->bs,
                        out_sect_sz);
                return SG_LIB_FILE_ERROR;
            }
        } else if (FT_BLOCK == clp->out_type) {
            if (0 != read_blkdev_capacity(clp->outfd, &out_num_sect,
                                          &out_sect_sz)) {
                pr2serr("Unable to read block capacity on %s\n", outf);
                out_num_sect = -1;
            }
            if (clp->bs != out_sect_sz) {
                pr2serr("logical block size on %s confusion: bs=%d, from "
                        "device=%d\n", outf, clp->bs, out_sect_sz);
                out_num_sect = -1;
            }
        } else if (FT_OTHER == clp->out_type) {
            out_num_sect = clp->out_st_size / clp->bs;
            if (clp->out_st_size % clp->bs) {
                ++out_num_sect;
                pr2serr("Warning: the file size of %s is not a multiple of BS "
                        "[%d]\n", outf, clp->bs);
            }
        }
        if (out_num_sect > clp->seek)
            out_num_sect -= clp->seek;

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
    if (clp->verbose > 2)
        pr2serr("Start of loop, count=%" PRId64 ", in_num_sect=%" PRId64
                ", out_num_sect=%" PRId64 "\n", dd_count, in_num_sect,
                out_num_sect);
    if (dd_count < 0) {
        pr2serr("Couldn't calculate count, please give one\n");
        return SG_LIB_CAT_OTHER;
    }
    if (! clp->cdbsz_given) {
        if ((FT_SG == clp->in_type) && (MAX_SCSI_CDBSZ != clp->cdbsz_in) &&
            (((dd_count + clp->skip) > UINT_MAX) || (clp->bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'if')\n");
            clp->cdbsz_in = MAX_SCSI_CDBSZ;
        }
        if ((FT_SG == clp->out_type) && (MAX_SCSI_CDBSZ != clp->cdbsz_out) &&
            (((dd_count + clp->seek) > UINT_MAX) || (clp->bpt > USHRT_MAX))) {
            pr2serr("Note: SCSI command size increased to 16 bytes (for "
                    "'of')\n");
            clp->cdbsz_out = MAX_SCSI_CDBSZ;
        }
    }

    // clp->in_count = dd_count;
    clp->in_rem_count = dd_count;
    clp->out_count = dd_count;
    clp->out_rem_count = dd_count;
    clp->out_blk = clp->seek;
    status = pthread_mutex_init(&clp->in_mutex, NULL);
    if (0 != status) err_exit(status, "init in_mutex");
    status = pthread_mutex_init(&clp->out_mutex, NULL);
    if (0 != status) err_exit(status, "init out_mutex");
    status = pthread_mutex_init(&clp->out2_mutex, NULL);
    if (0 != status) err_exit(status, "init out2_mutex");
    status = pthread_cond_init(&clp->out_sync_cv, NULL);
    if (0 != status) err_exit(status, "init out_sync_cv");

    if (clp->dry_run > 0) {
        pr2serr("Due to --dry-run option, bypass copy/read\n");
        goto fini;
    }
    if (! clp->ofile_given)
        pr2serr("of=OFILE not given so only read from IFILE, to output to "
                "stdout use 'of=-'\n");

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGUSR2);
    status = pthread_sigmask(SIG_BLOCK, &signal_set, &orig_signal_set);
    if (0 != status) err_exit(status, "pthread_sigmask");
    status = pthread_create(&sig_listen_thread_id, NULL,
                            sig_listen_thread, (void *)clp);
    if (0 != status) err_exit(status, "pthread_create, sig...");

    if (do_time) {
        start_tm.tv_sec = 0;
        start_tm.tv_usec = 0;
        gettimeofday(&start_tm, NULL);
    }

/* vvvvvvvvvvv  Start worker threads  vvvvvvvvvvvvvvvvvvvvvvvv */
    if ((clp->out_rem_count.load() > 0) && (num_threads > 0)) {
        Thread_info *tip = thread_arr + 0;

        tip->gcp = clp;
        tip->id = 0;
        /* Run 1 work thread to shake down infant retryable stuff */
        status = pthread_mutex_lock(&clp->out_mutex);
        if (0 != status) err_exit(status, "lock out_mutex");
        status = pthread_create(&tip->a_pthr, NULL, read_write_thread,
                                (void *)tip);
        if (0 != status) err_exit(status, "pthread_create");

        /* wait for any broadcast */
        pthread_cleanup_push(cleanup_out, (void *)clp);
        status = pthread_cond_wait(&clp->out_sync_cv, &clp->out_mutex);
        if (0 != status) err_exit(status, "cond out_sync_cv");
        pthread_cleanup_pop(0);
        status = pthread_mutex_unlock(&clp->out_mutex);
        if (0 != status) err_exit(status, "unlock out_mutex");

        /* now start the rest of the threads */
        for (k = 1; k < num_threads; ++k) {
            tip = thread_arr + k;
            tip->gcp = clp;
            tip->id = k;
            status = pthread_create(&tip->a_pthr, NULL, read_write_thread,
                                    (void *)tip);
            if (0 != status) err_exit(status, "pthread_create");
        }

        /* now wait for worker threads to finish */
        for (k = 0; k < num_threads; ++k) {
            tip = thread_arr + k;
            status = pthread_join(tip->a_pthr, &vp);
            if (0 != status) err_exit(status, "pthread_join");
            if (clp->verbose > 2)
                pr2serr_lk("%d <-- Worker thread terminated, vp=%s\n", k,
                           ((vp == clp) ? "clp" : "NULL (or !clp)"));
        }
    }   /* started worker threads and here after they have all exited */

    if (do_time && (start_tm.tv_sec || start_tm.tv_usec))
        calc_duration_throughput(0);

    shutting_down = true;
    status = pthread_join(sig_listen_thread_id, &vp);
    if (0 != status) err_exit(status, "pthread_join");
#if 0
    /* pthread_cancel() has issues and is not supported in Android */
    status = pthread_kill(sig_listen_thread_id, SIGUSR2);
    if (0 != status) err_exit(status, "pthread_kill");
    std::this_thread::yield();      // not enough it seems
    {   /* allow time for SIGUSR2 signal to get through */
        struct timespec tspec = {0, 400000}; /* 400 usecs */

        nanosleep(&tspec, NULL);
    }
#endif

    if (do_sync) {
        if (FT_SG == clp->out_type) {
            pr2serr_lk(">> Synchronizing cache on %s\n", outf);
            res = sg_ll_sync_cache_10(clp->outfd, 0, 0, 0, 0, 0, false, 0);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr_lk("Unit attention(out), continuing\n");
                res = sg_ll_sync_cache_10(clp->outfd, 0, 0, 0, 0, 0, false,
                                          0);
            }
            if (0 != res)
                pr2serr_lk("Unable to synchronize cache\n");
        }
        if (FT_SG == clp->out2_type) {
            pr2serr_lk(">> Synchronizing cache on %s\n", out2f);
            res = sg_ll_sync_cache_10(clp->out2fd, 0, 0, 0, 0, 0, false, 0);
            if (SG_LIB_CAT_UNIT_ATTENTION == res) {
                pr2serr_lk("Unit attention(out2), continuing\n");
                res = sg_ll_sync_cache_10(clp->out2fd, 0, 0, 0, 0, 0, false,
                                          0);
            }
            if (0 != res)
                pr2serr_lk("Unable to synchronize cache (of2)\n");
        }
    }

fini:
    if ((STDIN_FILENO != clp->infd) && (clp->infd >= 0))
        close(clp->infd);
    if ((STDOUT_FILENO != clp->outfd) && (FT_DEV_NULL != clp->out_type) &&
        (clp->outfd >= 0))
        close(clp->outfd);
    if ((clp->out2fd >= 0) && (STDOUT_FILENO != clp->out2fd) &&
        (FT_DEV_NULL != clp->out2_type))
        close(clp->out2fd);
    if ((clp->outregfd >= 0) && (STDOUT_FILENO != clp->outregfd) &&
        (FT_DEV_NULL != clp->outreg_type))
        close(clp->outregfd);
    res = exit_status;
    if ((0 != clp->out_count.load()) && (0 == clp->dry_run)) {
        pr2serr(">>>> Some error occurred, remaining blocks=%" PRId64 "\n",
                clp->out_count.load());
        if (0 == res)
            res = SG_LIB_CAT_OTHER;
    }
    print_stats("");
    if (clp->dio_incomplete_count.load()) {
        int fd;
        char c;

        pr2serr(">> Direct IO requested but incomplete %d times\n",
                clp->dio_incomplete_count.load());
        if ((fd = open(sg_allow_dio, O_RDONLY)) >= 0) {
            if (1 == read(fd, &c, 1)) {
                if ('0' == c)
                    pr2serr(">>> %s set to '0' but should be set to '1' for "
                            "direct IO\n", sg_allow_dio);
            }
            close(fd);
        }
    }
    if (clp->sum_of_resids.load())
        pr2serr(">> Non-zero sum of residual counts=%d\n",
               clp->sum_of_resids.load());
    if (clp->verbose && (num_start_eagain > 0))
        pr2serr("Number of start EAGAINs: %d\n", num_start_eagain.load());
    if (clp->verbose && (num_fin_eagain > 0))
        pr2serr("Number of finish EAGAINs: %d\n", num_fin_eagain.load());
    if (clp->verbose && (num_ebusy > 0))
        pr2serr("Number of EBUSYs: %d\n", num_ebusy.load());
    if (clp->verbose && clp->aen_given && (num_abort_req > 0)) {
        pr2serr("Number of Aborts: %d\n", num_abort_req.load());
        pr2serr("Number of successful Aborts: %d\n",
                num_abort_req_success.load());
    }
    if (clp->verbose && clp->m_aen_given && (num_mrq_abort_req > 0)) {
        pr2serr("Number of MRQ Aborts: %d\n", num_mrq_abort_req.load());
        pr2serr("Number of successful MRQ Aborts: %d\n",
                num_mrq_abort_req_success.load());
    }
    if (clp->verbose && (num_miscompare > 0))
        pr2serr("Number of miscompare%s: %d\n",
                (num_miscompare > 1) ? "s" : "", num_miscompare.load());
    if (clp->verbose > 1) {
        if (clp->verbose > 3)
            pr2serr("Final pack_id=%d, mrq_id=%d\n", mono_pack_id.load(),
                    mono_mrq_id.load());
        pr2serr("Number of SG_GET_NUM_WAITING calls=%ld\n",
                num_waiting_calls.load());
    }
    if (clp->verify && (SG_LIB_CAT_MISCOMPARE == res))
        pr2serr("Verify/compare failed due to miscompare\n");
    return (res >= 0) ? res : SG_LIB_CAT_OTHER;
}
