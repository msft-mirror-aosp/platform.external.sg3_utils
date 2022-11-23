/*
 * Copyright (c) 2014-2022 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_lib_data.h"
#include "sg_pt.h"
#include "sg_cmds_basic.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"

/* A utility program originally written for the Linux OS SCSI subsystem.
 *
 * This program issues one of the following SCSI commands:
 *   - CLOSE ZONE
 *   - FINISH ZONE
 *   - OPEN ZONE
 *   - REMOVE ELEMENT AND MODIFY ZONES
 *   - SEQUENTIALIZE ZONE
 */

static const char * version_str = "1.18 20220609";

#define SG_ZONING_OUT_CMDLEN 16
#define CLOSE_ZONE_SA 0x1
#define FINISH_ZONE_SA 0x2
#define OPEN_ZONE_SA 0x3
#define SEQUENTIALIZE_ZONE_SA 0x10
#define REM_ELEM_MOD_ZONES_SA 0x1a      /* uses SERVICE ACTION IN(16) */

#define SENSE_BUFF_LEN 64       /* Arbitrary, could be larger */
#define DEF_PT_TIMEOUT  60      /* 60 seconds */


static struct option long_options[] = {
        {"all", no_argument, 0, 'a'},
        {"close", no_argument, 0, 'c'},
        {"count", required_argument, 0, 'C'},
        {"element", required_argument, 0, 'e'},
        {"finish", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"open", no_argument, 0, 'o'},
        {"quick", no_argument, 0, 'q'},
        {"remove", no_argument, 0, 'r'},
        {"reset-all", no_argument, 0, 'R'},     /* same as --all */
        {"reset_all", no_argument, 0, 'R'},
        {"sequentialize", no_argument, 0, 'S'},
        {"verbose", no_argument, 0, 'v'},
        {"version", no_argument, 0, 'V'},
        {"zone", required_argument, 0, 'z'},
        {0, 0, 0, 0},
};

/* Indexed by service action of opcode 0x94 (Zone out) unless noted */
static const char * sa_name_arr[] = {
    "no SA=0",                  /* 0x0 */
    "Close zone",
    "Finish zone",
    "Open zone",
    "-", "-", "-", "-",
    "-",
    "-", "-", "-", "-",
    "-",
    "-",
    "-",
    "Sequentialize zone",       /* 0x10 */
    "-", "-", "-", "-",
    "-", "-", "-", "-",
    "-",
    "Remove element and modify zones", /* service action in(16), 0x1a */
};


static void
usage()
{
    pr2serr("Usage: "
            "sg_zone  [--all] [--close] [--count=ZC] [--element=EID] "
            "[--finish]\n"
            "                [--help] [--open] [--quick] [--remove] "
            "[--sequentialize]\n"
            "                [--verbose] [--version] [--zone=ID] DEVICE\n");
    pr2serr("  where:\n"
            "    --all|-a           sets the ALL flag in the cdb\n"
            "    --close|-c         issue CLOSE ZONE command\n"
            "    --count=ZC|-C ZC    set zone count field (def: 0)\n"
            "    --element=EID|-e EID    EID is the element identifier to "
            "remove;\n"
            "                            default is 0 which is an invalid "
            "EID\n"
            "    --finish|-f        issue FINISH ZONE command\n"
            "    --help|-h          print out usage message\n"
            "    --open|-o          issue OPEN ZONE command\n"
            "    --quick|-q         bypass 15 second warn and wait "
            "(for --remove)\n"
            "    --remove|-r        issue REMOVE ELEMENT AND MODIFY ZONES "
            "command\n"
            "    --sequentialize|-S    issue SEQUENTIALIZE ZONE command\n"
            "    --verbose|-v       increase verbosity\n"
            "    --version|-V       print version string and exit\n"
            "    --zone=ID|-z ID    ID is the starting LBA of the zone "
            "(def: 0)\n\n"
            "Performs a SCSI OPEN ZONE, CLOSE ZONE, FINISH ZONE, "
            "REMOVE ELEMENT AND\nMODIFY ZONES or SEQUENTIALIZE ZONE "
            "command. Either --close, --finish,\n--open, --remove or "
            "--sequentialize option needs to be given.\n");
}

/* Invokes the zone out command indicated by 'sa' (ZBC).  Return of 0
 * -> success, various SG_LIB_CAT_* positive values or -1 -> other errors */
static int
sg_ll_zone_out(int sg_fd, int sa, uint64_t zid, uint16_t zc, bool all,
               bool noisy, int verbose)
{
    int ret, res, sense_cat;
    struct sg_pt_base * ptvp;
    uint8_t zo_cdb[SG_ZONING_OUT_CMDLEN] =
          {SG_ZONING_OUT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0};
    uint8_t sense_b[SENSE_BUFF_LEN] SG_C_CPP_ZERO_INIT;
    char b[64];

    zo_cdb[1] = 0x1f & sa;
    if (REM_ELEM_MOD_ZONES_SA == sa) {  /* zid carries element identifier */
        zo_cdb[0] = SG_SERVICE_ACTION_IN_16;    /* N.B. changing opcode */
        sg_put_unaligned_be32((uint32_t)zid, zo_cdb + 10); /* element id */
    } else {
        sg_put_unaligned_be64(zid, zo_cdb + 2);
        sg_put_unaligned_be16(zc, zo_cdb + 12);
        if (all)
            zo_cdb[14] = 0x1;
    }
    sg_get_opcode_sa_name(zo_cdb[0], sa, -1, sizeof(b), b);
    if (verbose) {
        char d[128];

        pr2serr("    %s cdb: %s\n", b,
                sg_get_command_str(zo_cdb, SG_ZONING_OUT_CMDLEN,
                                   false, sizeof(d), d));
    }

    ptvp = construct_scsi_pt_obj();
    if (NULL == ptvp) {
        pr2serr("%s: out of memory\n", b);
        return -1;
    }
    set_scsi_pt_cdb(ptvp, zo_cdb, sizeof(zo_cdb));
    set_scsi_pt_sense(ptvp, sense_b, sizeof(sense_b));
    res = do_scsi_pt(ptvp, sg_fd, DEF_PT_TIMEOUT, verbose);
    ret = sg_cmds_process_resp(ptvp, b, res, noisy,
                               verbose, &sense_cat);
    if (-1 == ret) {
        if (get_scsi_pt_transport_err(ptvp))
            ret = SG_LIB_TRANSPORT_ERROR;
        else
            ret = sg_convert_errno(get_scsi_pt_os_err(ptvp));
    } else if (-2 == ret) {
        switch (sense_cat) {
        case SG_LIB_CAT_RECOVERED:
        case SG_LIB_CAT_NO_SENSE:
            ret = 0;
            break;
        default:
            ret = sense_cat;
            break;
        }
    } else
        ret = 0;
    destruct_scsi_pt_obj(ptvp);
    return ret;
}


int
main(int argc, char * argv[])
{
    bool all = false;
    bool close = false;
    bool finish = false;
    bool open = false;
    bool quick = false;
    bool reamz = false;
    bool element_id_given = false;
    bool sequentialize = false;
    bool verbose_given = false;
    bool version_given = false;
    int res, c, n;
    int sg_fd = -1;
    int verbose = 0;
    int ret = 0;
    int sa = 0;
    uint16_t zc = 0;
    uint64_t zid = 0;
    int64_t ll;
    const char * device_name = NULL;
    const char * sa_name;

    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "acC:e:fhoqrRSvVz:", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'a':
        case 'R':
            all = true;
            break;
        case 'c':
            close = true;
            sa = CLOSE_ZONE_SA;
            break;
        case 'C':
            n = sg_get_num(optarg);
            if ((n < 0) || (n > 0xffff)) {
                pr2serr("--count= expects an argument between 0 and 0xffff "
                        "inclusive\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            zc = (uint16_t)n;
            break;
        case 'e':
            ll = sg_get_llnum(optarg);
            if ((ll < 0) || (ll > UINT32_MAX)) {
                pr2serr("bad argument to '--element=EID'\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (0 == ll)
                pr2serr("Warning: 0 is an invalid element identifier\n");
            zid = (uint64_t)ll;    /* putting element_id in zid */
            element_id_given = true;
            break;
        case 'f':
            finish = true;
            sa = FINISH_ZONE_SA;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'o':
            open = true;
            sa = OPEN_ZONE_SA;
            break;
        case 'q':
            quick = true;
            break;
        case 'r':
            reamz = true;
            sa = REM_ELEM_MOD_ZONES_SA;
            break;
        case 'S':
            sequentialize = true;
            sa = SEQUENTIALIZE_ZONE_SA;
            break;
        case 'v':
            verbose_given = true;
            ++verbose;
            break;
        case 'V':
            version_given = true;
            break;
        case 'z':
            ll = sg_get_llnum(optarg);
            if (-1 == ll) {
                pr2serr("bad argument to '--zone=ID'\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            zid = (uint64_t)ll;
            break;
        default:
            pr2serr("unrecognised option code 0x%x ??\n", c);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n",
                        argv[optind]);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }

#ifdef DEBUG
    pr2serr("In DEBUG mode, ");
    if (verbose_given && version_given) {
        pr2serr("but override: '-vV' given, zero verbose and continue\n");
        verbose_given = false;
        version_given = false;
        verbose = 0;
    } else if (! verbose_given) {
        pr2serr("set '-vv'\n");
        verbose = 2;
    } else
        pr2serr("keep verbose=%d\n", verbose);
#else
    if (verbose_given && version_given)
        pr2serr("Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (version_given) {
        pr2serr("version: %s\n", version_str);
        return 0;
    }

    if (1 != ((int)close + (int)finish + (int)open + (int)sequentialize +
              (int)reamz)) {
        pr2serr("One, and only one, of these options needs to be given:\n"
                "   --close, --finish, --open, --remove or --sequentialize "
                "\n\n");
        usage();
        return SG_LIB_CONTRADICT;
    }
    if (element_id_given && (! reamz)) {
        pr2serr("The --element=EID option should only be used with the "
                "--remove option\n\n");
        usage();
        return SG_LIB_CONTRADICT;
    }
    sa_name = sa_name_arr[sa];

    if (NULL == device_name) {
        pr2serr("missing device name!\n");
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }

    sg_fd = sg_cmds_open_device(device_name, false /* rw */, verbose);
    if (sg_fd < 0) {
        int err = -sg_fd;
        if (verbose)
            pr2serr("open error: %s: %s\n", device_name,
                    safe_strerror(err));
        ret = sg_convert_errno(err);
        goto fini;
    }
    if (reamz && (! quick))
        sg_warn_and_wait(sa_name_arr[REM_ELEM_MOD_ZONES_SA], device_name,
                         false);

    res = sg_ll_zone_out(sg_fd, sa, zid, zc, all, true, verbose);
    ret = res;
    if (res) {
        if (SG_LIB_CAT_INVALID_OP == res)
            pr2serr("%s command not supported\n", sa_name);
        else {
            char b[80];

            sg_get_category_sense_str(res, sizeof(b), b, verbose);
            pr2serr("%s command: %s\n", sa_name, b);
        }
    }

fini:
    if (sg_fd >= 0) {
        res = sg_cmds_close_device(sg_fd);
        if (res < 0) {
            pr2serr("close error: %s\n", safe_strerror(-res));
            if (0 == ret)
                ret = sg_convert_errno(-res);
        }
    }
    if (0 == verbose) {
        if (! sg_if_can2stderr("sg_zone failed: ", ret))
            pr2serr("Some error occurred, try again with '-v' or '-vv' for "
                    "more information\n");
    }
    return (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
}
