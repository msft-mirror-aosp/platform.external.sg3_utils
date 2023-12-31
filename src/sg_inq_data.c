/*
 * A utility program originally written for the Linux OS SCSI subsystem.
 *     Copyright (C) 2000-2022 D. Gilbert
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is an auxiliary file holding data tables for the sg_inq utility.
 * It is mainly based on the SCSI SPC-6 document at https://www.t10.org .
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_lib_data.h"

/* Assume index is less than 16 */
const char * sg_ansi_version_arr[16] =
{
    "no conformance claimed",
    "SCSI-1",           /* obsolete, ANSI X3.131-1986 */
    "SCSI-2",           /* obsolete, ANSI X3.131-1994 */
    "SPC",              /* withdrawn, ANSI INCITS 301-1997 */
    "SPC-2",            /* ANSI INCITS 351-2001, ISO/IEC 14776-452 */
    "SPC-3",            /* ANSI INCITS 408-2005, ISO/IEC 14776-453 */
    "SPC-4",            /* ANSI INCITS 513-2015 */
    "SPC-5",            /* ANSI INCITS 502-2020 */
    "ecma=1, [8h]",
    "ecma=1, [9h]",
    "ecma=1, [Ah]",
    "ecma=1, [Bh]",
    "reserved [Ch]",
    "reserved [Dh]",
    "reserved [Eh]",
    "reserved [Fh]",
};

/* table from SPC-5 revision 16 [sorted numerically (from Annex E.9)] */
/* Can also be obtained from : https://www.t10.org/lists/stds.txt 20170114 */
/* Corrected against spc5r21 on 20190312 */

#ifdef SG_SCSI_STRINGS

struct sg_lib_simple_value_name_t sg_version_descriptor_arr[] = {
    {0x0, "Version Descriptor not supported or No standard identified"},
    {0x20, "SAM (no version claimed)"},
    {0x3b, "SAM T10/0994-D revision 18"},
    {0x3c, "SAM ANSI INCITS 270-1996"},
    {0x40, "SAM-2 (no version claimed)"},
    {0x54, "SAM-2 T10/1157-D revision 23"},
    {0x55, "SAM-2 T10/1157-D revision 24"},
    {0x5c, "SAM-2 ANSI INCITS 366-2003"},
    {0x5e, "SAM-2 ISO/IEC 14776-412"},
    {0x60, "SAM-3 (no version claimed)"},
    {0x62, "SAM-3 T10/1561-D revision 7"},
    {0x75, "SAM-3 T10/1561-D revision 13"},
    {0x76, "SAM-3 T10/1561-D revision 14"},
    {0x77, "SAM-3 ANSI INCITS 402-2005"},
    {0x80, "SAM-4 (no version claimed)"},
    {0x87, "SAM-4 T10/1683-D revision 13"},
    {0x8b, "SAM-4 T10/1683-D revision 14"},
    {0x90, "SAM-4 ANSI INCITS 447-2008"},
    {0x92, "SAM-4 ISO/IEC 14776-414"},
    {0xa0, "SAM-5 (no version claimed)"},
    {0xa2, "SAM-5 T10/2104-D revision 4"},
    {0xa4, "SAM-5 T10/2104-D revision 20"},
    {0xa6, "SAM-5 T10/2104-D revision 21"},
    {0xa8, "SAM-5 ANSI INCITS 515-2016"},
    {0xc0, "SAM-6 (no version claimed)"},
    {0x120, "SPC (no version claimed)"},
    {0x13b, "SPC T10/0995-D revision 11a"},
    {0x13c, "SPC ANSI INCITS 301-1997"},
    {0x140, "MMC (no version claimed)"},
    {0x15b, "MMC T10/1048-D revision 10a"},
    {0x15c, "MMC ANSI INCITS 304-1997"},
    {0x160, "SCC (no version claimed)"},
    {0x17b, "SCC T10/1047-D revision 06c"},
    {0x17c, "SCC ANSI INCITS 276-1997"},
    {0x180, "SBC (no version claimed)"},
    {0x19b, "SBC T10/0996-D revision 08c"},
    {0x19c, "SBC ANSI INCITS 306-1998"},
    {0x1a0, "SMC (no version claimed)"},
    {0x1bb, "SMC T10/0999-D revision 10a"},
    {0x1bc, "SMC ANSI INCITS 314-1998"},
    {0x1be, "SMC ISO/IEC 14776-351"},
    {0x1c0, "SES (no version claimed)"},
    {0x1db, "SES T10/1212-D revision 08b"},
    {0x1dc, "SES ANSI INCITS 305-1998"},
    {0x1dd, "SES T10/1212-D revision 08b w/ Amendment ANSI "
            "INCITS.305/AM1:2000"},
    {0x1de, "SES ANSI INCITS 305-1998 w/ Amendment ANSI "
            "INCITS.305/AM1:2000"},
    {0x1e0, "SCC-2 (no version claimed}"},
    {0x1fb, "SCC-2 T10/1125-D revision 04"},
    {0x1fc, "SCC-2 ANSI INCITS 318-1998"},
    {0x200, "SSC (no version claimed)"},
    {0x201, "SSC T10/0997-D revision 17"},
    {0x207, "SSC T10/0997-D revision 22"},
    {0x21c, "SSC ANSI INCITS 335-2000"},
    {0x220, "RBC (no version claimed)"},
    {0x238, "RBC T10/1240-D revision 10a"},
    {0x23c, "RBC ANSI INCITS 330-2000"},
    {0x240, "MMC-2 (no version claimed)"},
    {0x255, "MMC-2 T10/1228-D revision 11"},
    {0x25b, "MMC-2 T10/1228-D revision 11a"},
    {0x25c, "MMC-2 ANSI INCITS 333-2000"},
    {0x260, "SPC-2 (no version claimed)"},
    {0x267, "SPC-2 T10/1236-D revision 12"},
    {0x269, "SPC-2 T10/1236-D revision 18"},
    {0x275, "SPC-2 T10/1236-D revision 19"},
    {0x276, "SPC-2 T10/1236-D revision 20"},
    {0x277, "SPC-2 ANSI INCITS 351-2001"},
    {0x278, "SPC-2 ISO/IEC 14776-452"},
    {0x280, "OCRW (no version claimed)"},
    {0x29e, "OCRW ISO/IEC 14776-381"},
    {0x2a0, "MMC-3 (no version claimed)"},
    {0x2b5, "MMC-3 T10/1363-D revision 9"},
    {0x2b6, "MMC-3 T10/1363-D revision 10g"},
    {0x2b8, "MMC-3 ANSI INCITS 360-2002"},
    {0x2e0, "SMC-2 (no version claimed)"},
    {0x2f5, "SMC-2 T10/1383-D revision 5"},
    {0x2fc, "SMC-2 T10/1383-D revision 6"},
    {0x2fd, "SMC-2 T10/1383-D revision 7"},
    {0x2fe, "SMC-2 ANSI INCITS 382-2004"},
    {0x300, "SPC-3 (no version claimed)"},
    {0x301, "SPC-3 T10/1416-D revision 7"},
    {0x307, "SPC-3 T10/1416-D revision 21"},
    {0x30f, "SPC-3 T10/1416-D revision 22"},
    {0x312, "SPC-3 T10/1416-D revision 23"},
    {0x314, "SPC-3 ANSI INCITS 408-2005"},
    {0x316, "SPC-3 ISO/IEC 14776-453"},
    {0x320, "SBC-2 (no version claimed)"},
    {0x322, "SBC-2 T10/1417-D revision 5a"},
    {0x324, "SBC-2 T10/1417-D revision 15"},
    {0x33b, "SBC-2 T10/1417-D revision 16"},
    {0x33d, "SBC-2 ANSI INCITS 405-2005"},
    {0x33e, "SBC-2 ISO/IEC 14776-322"},
    {0x340, "OSD (no version claimed)"},
    {0x341, "OSD T10/1355-D revision 0"},
    {0x342, "OSD T10/1355-D revision 7a"},
    {0x343, "OSD T10/1355-D revision 8"},
    {0x344, "OSD T10/1355-D revision 9"},
    {0x355, "OSD T10/1355-D revision 10"},
    {0x356, "OSD ANSI INCITS 400-2004"},
    {0x360, "SSC-2 (no version claimed)"},
    {0x374, "SSC-2 T10/1434-D revision 7"},
    {0x375, "SSC-2 T10/1434-D revision 9"},
    {0x37d, "SSC-2 ANSI INCITS 380-2003"},
    {0x380, "BCC (no version claimed)"},
    {0x3a0, "MMC-4 (no version claimed)"},
    {0x3b0, "MMC-4 T10/1545-D revision 5"},     /* dropped in spc4r09 */
    {0x3b1, "MMC-4 T10/1545-D revision 5a"},
    {0x3bd, "MMC-4 T10/1545-D revision 3"},
    {0x3be, "MMC-4 T10/1545-D revision 3d"},
    {0x3bf, "MMC-4 ANSI INCITS 401-2005"},
    {0x3c0, "ADC (no version claimed)"},
    {0x3d5, "ADC T10/1558-D revision 6"},
    {0x3d6, "ADC T10/1558-D revision 7"},
    {0x3d7, "ADC ANSI INCITS 403-2005"},
    {0x3e0, "SES-2 (no version claimed)"},
    {0x3e1, "SES-2 T10/1559-D revision 16"},
    {0x3e7, "SES-2 T10/1559-D revision 19"},
    {0x3eb, "SES-2 T10/1559-D revision 20"},
    {0x3f0, "SES-2 ANSI INCITS 448-2008"},
    {0x3f2, "SES-2 ISO/IEC 14776-372"},
    {0x400, "SSC-3 (no version claimed)"},
    {0x403, "SSC-3 T10/1611-D revision 04a"},
    {0x407, "SSC-3 T10/1611-D revision 05"},
    {0x409, "SSC-3 ANSI INCITS 467-2011"},
    {0x40b, "SSC-3 ISO/IEC 14776-333:2013"},
    {0x420, "MMC-5 (no version claimed)"},
    {0x42f, "MMC-5 T10/1675-D revision 03"},
    {0x431, "MMC-5 T10/1675-D revision 03b"},
    {0x432, "MMC-5 T10/1675-D revision 04"},
    {0x434, "MMC-5 ANSI INCITS 430-2007"},
    {0x440, "OSD-2 (no version claimed)"},
    {0x444, "OSD-2 T10/1729-D revision 4"},
    {0x446, "OSD-2 T10/1729-D revision 5"},
    {0x448, "OSD-2 ANSI INCITS 458-2011"},
    {0x460, "SPC-4 (no version claimed)"},
    {0x461, "SPC-4 T10/BSR INCITS 513 revision 16"},
    {0x462, "SPC-4 T10/BSR INCITS 513 revision 18"},
    {0x463, "SPC-4 T10/BSR INCITS 513 revision 23"},
    {0x466, "SPC-4 T10/BSR INCITS 513 revision 36"},
    {0x468, "SPC-4 T10/BSR INCITS 513 revision 37"},
    {0x469, "SPC-4 T10/BSR INCITS 513 revision 37a"},
    {0x46c, "SPC-4 ANSI INCITS 513-2015"},
    {0x480, "SMC-3 (no version claimed)"},
    {0x482, "SMC-3 T10/1730-D revision 15"},
    {0x484, "SMC-3 T10/1730-D revision 16"},
    {0x486, "SMC-3 ANSI INCITS 484-2012"},
    {0x4a0, "ADC-2 (no version claimed)"},
    {0x4a7, "ADC-2 T10/1741-D revision 7"},
    {0x4aa, "ADC-2 T10/1741-D revision 8"},
    {0x4ac, "ADC-2 ANSI INCITS 441-2008"},
    {0x4c0, "SBC-3 (no version claimed)"},
    {0x4c3, "SBC-3 T10/BSR INCITS 514 revision 35"},
    {0x4c5, "SBC-3 T10/BSR INCITS 514 revision 36"},
    {0x4c8, "SBC-3 ANSI INCITS 514-2014"},
    {0x4e0, "MMC-6 (no version claimed)"},
    {0x4e3, "MMC-6 T10/1836-D revision 2b"},
    {0x4e5, "MMC-6 T10/1836-D revision 02g"},
    {0x4e6, "MMC-6 ANSI INCITS 468-2010"},
    {0x4e7, "MMC-6 ANSI INCITS 468-2010 + MMC-6/AM1 ANSI INCITS "
            "468-2010/AM 1"},
    {0x500, "ADC-3 (no version claimed)"},
    {0x502, "ADC-3 T10/1895-D revision 04"},
    {0x504, "ADC-3 T10/1895-D revision 05"},
    {0x506, "ADC-3 T10/1895-D revision 05a"},
    {0x50a, "ADC-3 ANSI INCITS 497-2012"},
    {0x520, "SSC-4 (no version claimed)"},
    {0x523, "SSC-4 T10/BSR INCITS 516 revision 2"},
    {0x525, "SSC-4 T10/BSR INCITS 516 revision 3"},
    {0x527, "SSC-4 SSC-4 ANSI INCITS 516-2013"},
    {0x560, "OSD-3 (no version claimed)"},
    {0x580, "SES-3 (no version claimed)"},
    {0x582, "SES-3 T10/BSR INCITS 518 revision 13"},
    {0x584, "SES-3 T10/BSR INCITS 518 revision 14"},
    {0x5a0, "SSC-5 (no version claimed)"},
    {0x5c0, "SPC-5 (no version claimed)"},
/* SPC-5 is now a standard [ANSI INCITS 502-2020] but no version code */
/* SPC-6 is now up to draft 06 but still no version code */
    {0x5e0, "SFSC (no version claimed)"},
    {0x5e3, "SFSC BSR INCITS 501 revision 01"},
    {0x5e5, "SFSC BSR INCITS 501 revision 02"},
    {0x5e8, "SFSC ANSI INCITS 501-2016"},
    {0x600, "SBC-4 (no version claimed)"},
    {0x620, "ZBC (no version claimed)"},
    {0x622, "ZBC BSR INCITS 536 revision 02"},
    {0x624, "ZBC BSR INCITS 536 revision 05"},
    {0x640, "ADC-4 (no version claimed)"},
    {0x660, "ZBC-2 (no version claimed)"},
    {0x680, "SES-4 (no version claimed)"},
    {0x820, "SSA-TL2 (no version claimed)"},
    {0x83b, "SSA-TL2 T10/1147-D revision 05b"},
    {0x83c, "SSA-TL2 ANSI INCITS 308-1998"},
    {0x840, "SSA-TL1 (no version claimed)"},
    {0x85b, "SSA-TL1 T10/0989-D revision 10b"},
    {0x85c, "SSA-TL1 ANSI INCITS 295-1996"},
    {0x860, "SSA-S3P (no version claimed)"},
    {0x87b, "SSA-S3P T10/1051-D revision 05b"},
    {0x87c, "SSA-S3P ANSI INCITS 309-1998"},
    {0x880, "SSA-S2P (no version claimed)"},
    {0x89b, "SSA-S2P T10/1121-D revision 07b"},
    {0x89c, "SSA-S2P ANSI INCITS 294-1996"},
    {0x8a0, "SIP (no version claimed)"},
    {0x8bb, "SIP T10/0856-D revision 10"},
    {0x8bc, "SIP ANSI INCITS 292-1997"},
    {0x8c0, "FCP (no version claimed)"},
    {0x8db, "FCP T10/0856-D revision 12"},
    {0x8dc, "FCP ANSI INCITS 269-1996"},
    {0x8e0, "SBP-2 (no version claimed)"},
    {0x8fb, "SBP-2 T10/1155-D revision 04"},
    {0x8fc, "SBP-2 ANSI INCITS 325-1999"},
    {0x900, "FCP-2 (no version claimed)"},
    {0x901, "FCP-2 T10/1144-D revision 4"},
    {0x915, "FCP-2 T10/1144-D revision 7"},
    {0x916, "FCP-2 T10/1144-D revision 7a"},
    {0x917, "FCP-2 ANSI INCITS 350-2003"},
    {0x918, "FCP-2 T10/1144-D revision 8"},
    {0x920, "SST (no version claimed)"},
    {0x935, "SST T10/1380-D revision 8b"},
    {0x940, "SRP (no version claimed)"},
    {0x954, "SRP T10/1415-D revision 10"},
    {0x955, "SRP T10/1415-D revision 16a"},
    {0x95c, "SRP ANSI INCITS 365-2002"},
    {0x960, "iSCSI (no version claimed)"},
    {0x961, "iSCSI RFC 7143"},
    {0x962, "iSCSI RFC 7144"},
    /* 0x960 up to 0x97f for iSCSI use */
    {0x980, "SBP-3 (no version claimed)"},
    {0x982, "SBP-3 T10/1467-D revision 1f"},
    {0x994, "SBP-3 T10/1467-D revision 3"},
    {0x99a, "SBP-3 T10/1467-D revision 4"},
    {0x99b, "SBP-3 T10/1467-D revision 5"},
    {0x99c, "SBP-3 ANSI INCITS 375-2004"},
    {0x9a0, "SRP-2 (no version claimed)"},
    {0x9c0, "ADP (no version claimed)"},
    {0x9e0, "ADT (no version claimed)"},
    {0x9f9, "ADT T10/1557-D revision 11"},
    {0x9fa, "ADT T10/1557-D revision 14"},
    {0x9fd, "ADT ANSI INCITS 406-2005"},
    {0xa00, "FCP-3 (no version claimed)"},
    {0xa07, "FCP-3 T10/1560-D revision 3f"},
    {0xa0f, "FCP-3 T10/1560-D revision 4"},
    {0xa11, "FCP-3 ANSI INCITS 416-2006"},
    {0xa1c, "FCP-3 ISO/IEC 14776-223"},
    {0xa20, "ADT-2 (no version claimed)"},
    {0xa22, "ADT-2 T10/1742-D revision 06"},
    {0xa27, "ADT-2 T10/1742-D revision 08"},
    {0xa28, "ADT-2 T10/1742-D revision 09"},
    {0xa2b, "ADT-2 ANSI INCITS 472-2011"},
    {0xa40, "FCP-4 (no version claimed)"},
    {0xa42, "FCP-4 T10/1828-D revision 01"},
    {0xa44, "FCP-4 T10/1828-D revision 02"},
    {0xa45, "FCP-4 T10/1828-D revision 02b"},
    {0xa46, "FCP-4 ANSI INCITS 481-2012"},
    {0xa60, "ADT-3 (no version claimed)"},
    {0xaa0, "SPI (no version claimed)"},
    {0xab9, "SPI T10/0855-D revision 15a"},
    {0xaba, "SPI ANSI INCITS 253-1995"},
    {0xabb, "SPI T10/0855-D revision 15a with SPI Amnd revision 3a"},
    {0xabc, "SPI ANSI INCITS 253-1995 with SPI Amnd ANSI INCITS "
            "253/AM1:1998"},
    {0xac0, "Fast-20 (no version claimed)"},
    {0xadb, "Fast-20 T10/1071-D revision 06"},
    {0xadc, "Fast-20 ANSI INCITS 277-1996"},
    {0xae0, "SPI-2 (no version claimed)"},
    {0xafb, "SPI-2 T10/1142-D revision 20b"},
    {0xafc, "SPI-2 ANSI INCITS 302-1999"},
    {0xb00, "SPI-3 (no version claimed)"},
    {0xb18, "SPI-3 T10/1302-D revision 10"},
    {0xb19, "SPI-3 T10/1302-D revision 13a"},
    {0xb1a, "SPI-3 T10/1302-D revision 14"},
    {0xb1c, "SPI-3 ANSI INCITS 336-2000"},
    {0xb20, "EPI (no version claimed)"},
    {0xb3b, "EPI T10/1134-D revision 16"},
    {0xb3c, "EPI ANSI INCITS TR-23 1999"},
    {0xb40, "SPI-4 (no version claimed)"},
    {0xb54, "SPI-4 T10/1365-D revision 7"},
    {0xb55, "SPI-4 T10/1365-D revision 9"},
    {0xb56, "SPI-4 ANSI INCITS 362-2002"},
    {0xb59, "SPI-4 T10/1365-D revision 10"},
    {0xb60, "SPI-5 (no version claimed)"},
    {0xb79, "SPI-5 T10/1525-D revision 3"},
    {0xb7a, "SPI-5 T10/1525-D revision 5"},
    {0xb7b, "SPI-5 T10/1525-D revision 6"},
    {0xb7c, "SPI-5 ANSI INCITS 367-2004"},
    {0xbe0, "SAS (no version claimed)"},
    {0xbe1, "SAS T10/1562-D revision 01"},
    {0xbf5, "SAS T10/1562-D revision 03"},
    {0xbfa, "SAS T10/1562-D revision 04"},
    {0xbfb, "SAS T10/1562-D revision 04"},
    {0xbfc, "SAS T10/1562-D revision 05"},
    {0xbfd, "SAS ANSI INCITS 376-2003"},
    {0xc00, "SAS-1.1 (no version claimed)"},
    {0xc07, "SAS-1.1 T10/1602-D revision 9"},
    {0xc0f, "SAS-1.1 T10/1602-D revision 10"},
    {0xc11, "SAS-1.1 ANSI INCITS 417-2006"},
    {0xc12, "SAS-1.1 ISO/IEC 14776-151"},
    {0xc20, "SAS-2 (no version claimed)"},
    {0xc23, "SAS-2 T10/1760-D revision 14"},
    {0xc27, "SAS-2 T10/1760-D revision 15"},
    {0xc28, "SAS-2 T10/1760-D revision 16"},
    {0xc2a, "SAS-2 ANSI INCITS 457-2010"},
    {0xc40, "SAS-2.1 (no version claimed)"},
    {0xc48, "SAS-2.1 T10/2125-D revision 04"},
    {0xc4a, "SAS-2.1 T10/2125-D revision 06"},
    {0xc4b, "SAS-2.1 T10/2125-D revision 07"},
    {0xc4e, "SAS-2.1 ANSI INCITS 478-2011"},
    {0xc4f, "SAS-2.1 ANSI INCITS 478-2011 w/ Amnd 1 ANSI INCITS "
            "478/AM1-2014"},
    {0xc52, "SAS-2.1 ISO/IEC 14776-153"},
    {0xc60, "SAS-3 (no version claimed)"},
    {0xc63, "SAS-3 T10/BSR INCITS 519 revision 05a"},
    {0xc65, "SAS-3 T10/BSR INCITS 519 revision 06"},
    {0xc68, "SAS-3 ANSI INCITS 519-2014"},
    {0xc80, "SAS-4 (no version claimed)"},
    {0xc82, "SAS-4 T10/BSR INCITS 534 revision 08a"},
    {0xd20, "FC-PH (no version claimed)"},
    {0xd3b, "FC-PH ANSI INCITS 230-1994"},
    {0xd3c, "FC-PH ANSI INCITS 230-1994 with Amnd 1 ANSI INCITS "
            "230/AM1:1996"},
    {0xd40, "FC-AL (no version claimed)"},
    {0xd5c, "FC-AL ANSI INCITS 272-1996"},
    {0xd60, "FC-AL-2 (no version claimed)"},
    {0xd61, "FC-AL-2 T11/1133-D revision 7.0"},
    {0xd63, "FC-AL-2 ANSI INCITS 332-1999 with AM1-2003 & AM2-2006"},
    {0xd64, "FC-AL-2 ANSI INCITS 332-1999 with Amnd 2 AM2-2006"},
    {0xd65, "FC-AL-2 ISO/IEC 14165-122 with AM1 & AM2"},
    {0xd7c, "FC-AL-2 ANSI INCITS 332-1999"},
    {0xd7d, "FC-AL-2 ANSI INCITS 332-1999 with Amnd 1 AM1:2002"},
    {0xd80, "FC-PH-3 (no version claimed)"},
    {0xd9c, "FC-PH-3 ANSI INCITS 303-1998"},
    {0xda0, "FC-FS (no version claimed)"},
    {0xdb7, "FC-FS T11/1331-D revision 1.2"},
    {0xdb8, "FC-FS T11/1331-D revision 1.7"},
    {0xdbc, "FC-FS ANSI INCITS 373-2003"},
    {0xdbd, "FC-FS ISO/IEC 14165-251"},
    {0xdc0, "FC-PI (no version claimed)"},
    {0xddc, "FC-PI ANSI INCITS 352-2002"},
    {0xde0, "FC-PI-2 (no version claimed)"},
    {0xde2, "FC-PI-2 T11/1506-D revision 5.0"},
    {0xde4, "FC-PI-2 ANSI INCITS 404-2006"},
    {0xe00, "FC-FS-2 (no version claimed)"},
    {0xe02, "FC-FS-2 ANSI INCITS 242-2007"},
    {0xe03, "FC-FS-2 ANSI INCITS 242-2007 with AM1 ANSI INCITS 242/AM1-2007"},
    {0xe20, "FC-LS (no version claimed)"},
    {0xe21, "FC-LS T11/1620-D revision 1.62"},
    {0xe29, "FC-LS ANSI INCITS 433-2007"},
    {0xe40, "FC-SP (no version claimed)"},
    {0xe42, "FC-SP T11/1570-D revision 1.6"},
    {0xe45, "FC-SP ANSI INCITS 426-2007"},
    {0xe60, "FC-PI-3 (no version claimed)"},
    {0xe62, "FC-PI-3 T11/1625-D revision 2.0"},
    {0xe68, "FC-PI-3 T11/1625-D revision 2.1"},
    {0xe6a, "FC-PI-3 T11/1625-D revision 4.0"},
    {0xe6e, "FC-PI-3 ANSI INCITS 460-2011"},
    {0xe80, "FC-PI-4 (no version claimed)"},
    {0xe82, "FC-PI-4 T11/1647-D revision 8.0"},
    {0xe88, "FC-PI-4 ANSI INCITS 450 -2009"},
    {0xea0, "FC 10GFC (no version claimed)"},
    {0xea2, "FC 10GFC ANSI INCITS 364-2003"},
    {0xea3, "FC 10GFC ISO/IEC 14165-116"},
    {0xea5, "FC 10GFC ISO/IEC 14165-116 with AM1"},
    {0xea6, "FC 10GFC ANSI INCITS 364-2003 with AM1 ANSI INCITS 364/AM1-2007"},
    {0xec0, "FC-SP-2 (no version claimed)"},
    {0xee0, "FC-FS-3 (no version claimed)"},
    {0xee2, "FC-FS-3 T11/1861-D revision 0.9"},
    {0xee7, "FC-FS-3 T11/1861-D revision 1.0"},
    {0xee9, "FC-FS-3 T11/1861-D revision 1.10"},
    {0xeeb, "FC-FS-3 ANSI INCITS 470-2011"},
    {0xf00, "FC-LS-2 (no version claimed)"},
    {0xf03, "FC-LS-2 T11/2103-D revision 2.11"},
    {0xf05, "FC-LS-2 T11/2103-D revision 2.21"},
    {0xf07, "FC-LS-2 ANSI INCITS 477-2011"},
    {0xf20, "FC-PI-5 (no version claimed)"},
    {0xf27, "FC-PI-5 T11/2118-D revision 2.00"},
    {0xf28, "FC-PI-5 T11/2118-D revision 3.00"},
    {0xf2a, "FC-PI-5 T11/2118-D revision 6.00"},
    {0xf2b, "FC-PI-5 T11/2118-D revision 6.10"},
    {0xf2e, "FC-PI-5 ANSI INCITS 479-2011"},
    {0xf40, "FC-PI-6 (no version claimed)"},
    {0xf60, "FC-FS-4 (no version claimed)"},
    {0xf80, "FC-LS-3 (no version claimed)"},
    {0x12a0, "FC-SCM (no version claimed)"},
    {0x12a3, "FC-SCM T11/1824DT revision 1.0"},
    {0x12a5, "FC-SCM T11/1824DT revision 1.1"},
    {0x12a7, "FC-SCM T11/1824DT revision 1.4"},
    {0x12aa, "FC-SCM INCITS TR-47 2012"},
    {0x12c0, "FC-DA-2 (no version claimed)"},
    {0x12c3, "FC-DA-2 T11/1870DT revision 1.04"},
    {0x12c5, "FC-DA-2 T11/1870DT revision 1.06"},
    {0x12c9, "FC-DA-2 INCITS TR-49 2012"},
    {0x12e0, "FC-DA (no version claimed)"},
    {0x12e2, "FC-DA T11/1513-DT revision 3.1"},
    {0x12e8, "FC-DA ANSI INCITS TR-36 2004"},
    {0x12e9, "FC-DA ISO/IEC 14165-341"},
    {0x1300, "FC-Tape (no version claimed)"},
    {0x1301, "FC-Tape T11/1315-D revision 1.16"},
    {0x131b, "FC-Tape T11/1315-D revision 1.17"},
    {0x131c, "FC-Tape ANSI INCITS TR-24 1999"},
    {0x1320, "FC-FLA (no version claimed)"},
    {0x133b, "FC-FLA T11/1235-D revision 7"},
    {0x133c, "FC-FLA ANSI INCITS TR-20 1998"},
    {0x1340, "FC-PLDA (no version claimed)"},
    {0x135b, "FC-PLDA T11/1162-D revision 2.1"},
    {0x135c, "FC-PLDA ANSI INCITS TR-19 1998"},
    {0x1360, "SSA-PH2 (no version claimed)"},
    {0x137b, "SSA-PH2 T10/1145-D revision 09c"},
    {0x137c, "SSA-PH2 ANSI INCITS 293-1996"},
    {0x1380, "SSA-PH3 (no version claimed)"},
    {0x139b, "SSA-PH3 T10/1146-D revision 05b"},
    {0x139c, "SSA-PH3 ANSI INCITS 307-1998"},
    {0x14a0, "IEEE 1394 (no version claimed)"},
    {0x14bd, "ANSI IEEE 1394:1995"},
    {0x14c0, "IEEE 1394a (no version claimed)"},
    {0x14e0, "IEEE 1394b (no version claimed)"},
    {0x15e0, "ATA/ATAPI-6 (no version claimed)"},
    {0x15fd, "ATA/ATAPI-6 ANSI INCITS 361-2002"},
    {0x1600, "ATA/ATAPI-7 (no version claimed)"},
    {0x1602, "ATA/ATAPI-7 T13/1532-D revision 3"},
    {0x161c, "ATA/ATAPI-7 ANSI INCITS 397-2005"},
    {0x161e, "ATA/ATAPI-7 ISO/IEC 24739"},
    {0x1620, "ATA/ATAPI-8 ATA-AAM Architecture model (no version claimed)"},
    {0x1621, "ATA/ATAPI-8 ATA-PT Parallel transport (no version claimed)"},
    {0x1622, "ATA/ATAPI-8 ATA-AST Serial transport (no version claimed)"},
    {0x1623, "ATA/ATAPI-8 ATA-ACS ATA/ATAPI command set (no version "
             "claimed)"},
    {0x1628, "ATA/ATAPI-8 ATA-AAM ANSI INCITS 451-2008"},
    {0x162a, "ATA/ATAPI-8 ATA8-ACS ANSI INCITS 452-2009 w/ Amendment 1"},
    {0x1728, "Universal Serial Bus Specification, Revision 1.1"},
    {0x1729, "Universal Serial Bus Specification, Revision 2.0"},
    {0x1730, "USB Mass Storage Class Bulk-Only Transport, Revision 1.0"},
    {0x1740, "UAS (no version claimed)"},    /* USB attached SCSI */
    {0x1743, "UAS T10/2095-D revision 02"},
    {0x1747, "UAS T10/2095-D revision 04"},
    {0x1748, "UAS ANSI INCITS 471-2010"},
    {0x1749, "UAS ISO/IEC 14776-251:2014"},
    {0x1761, "ACS-2 (no version claimed)"},
    {0x1762, "ACS-2 ANSI INCITS 482-2013"},
    {0x1765, "ACS-3 INCITS 522-2014"},
    {0x1767, "ACS-4 INCITS 529-2018"},
    {0x1780, "UAS-2 (no version claimed)"},
    {0x1ea0, "SAT (no version claimed)"},
    {0x1ea7, "SAT T10/1711-D rev 8"},
    {0x1eab, "SAT T10/1711-D rev 9"},
    {0x1ead, "SAT ANSI INCITS 431-2007"},
    {0x1ec0, "SAT-2 (no version claimed)"},
    {0x1ec4, "SAT-2 T10/1826-D revision 06"},
    {0x1ec8, "SAT-2 T10/1826-D revision 09"},
    {0x1eca, "SAT-2 ANSI INCITS 465-2010"},
    {0x1ee0, "SAT-3 (no version claimed)"},
    {0x1ee2, "SAT-3 T10/BSR INCITS 517 revision 4"},
    {0x1ee4, "SAT-3 T10/BSR INCITS 517 revision 7"},
    {0x1ee8, "SAT-3 ANSI INCITS 517-2015"},
    {0x1f00, "SAT-4 (no version claimed)"},
    {0x1f02, "SAT-4 T10/BSR INCITS 491 revision 5"},
    {0x1f04, "SAT-4 T10/BSR INCITS 491 revision 6"},
    {0x20a0, "SPL (no version claimed)"},
    {0x20a3, "SPL T10/2124-D revision 6a"},
    {0x20a5, "SPL T10/2124-D revision 7"},
    {0x20a7, "SPL ANSI INCITS 476-2011"},
    {0x20a8, "SPL ANSI INCITS 476-2011 + SPL AM1 INCITS 476/AM1 2012"},
    {0x20aa, "SPL ISO/IEC 14776-261:2012"},
    {0x20c0, "SPL-2 (no version claimed)"},
    {0x20c2, "SPL-2 T10/BSR INCITS 505 revision 4"},
    {0x20c4, "SPL-2 T10/BSR INCITS 505 revision 5"},
    {0x20c8, "SPL-2 ANSI INCITS 505-2013"},
    {0x20e0, "SPL-3 (no version claimed)"},
    {0x20e4, "SPL-3 T10/BSR INCITS 492 revision 6"},
    {0x20e6, "SPL-3 T10/BSR INCITS 492 revision 7"},
    {0x20e8, "SPL-3 ANSI INCITS 492-2015"},
    {0x2100, "SPL-4 (no version claimed)"},
    {0x2102, "SPL-4 T10/BSR INCITS 538 revision 08a"},
    {0x2104, "SPL-4 T10/BSR INCITS 538 revision 10"},
    {0x2105, "SPL-4 T10/BSR INCITS 538 revision 11"},
    {0x2120, "SPL-5 (no version claimed)"},
    {0x21e0, "SOP (no version claimed)"},
    {0x21e4, "SOP T10/BSR INCITS 489 revision 4"},
    {0x21e6, "SOP T10/BSR INCITS 489 revision 5"},
    {0x21e8, "SOP ANSI INCITS 489-2014"},
    {0x2200, "PQI (no version claimed)"},
    {0x2204, "PQI T10/BSR INCITS 490 revision 6"},
    {0x2206, "PQI T10/BSR INCITS 490 revision 7"},
    {0x2208, "PQI ANSI INCITS 490-2014"},
    {0x2220, "SOP-2 (no draft published)"},
    {0x2240, "PQI-2 (no version claimed)"},
    {0x2242, "PQI-2 T10/BSR INCITS 507 revision 01"},
    {0x2244, "PQI-2 PQI-2 ANSI INCITS 507-2016"},
    {0xffc0, "IEEE 1667 (no version claimed)"},
    {0xffc1, "IEEE 1667-2006"},
    {0xffc2, "IEEE 1667-2009"},
    {0xffc3, "IEEE 1667-2015"},
    {0xffc4, "IEEE 1667-2018"},
    {0xffff, NULL},     /* sentinel, leave at end */
};

#else

struct sg_lib_simple_value_name_t sg_version_descriptor_arr[] = {
    {0xffff, NULL},     /* sentinel, leave at end */
};

#endif
