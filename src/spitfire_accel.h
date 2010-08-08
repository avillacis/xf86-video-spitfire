#if !defined _SPITFIRE_ACCEL
#define _SPITFIRE_ACCEL

/* Documented registers for the Spitfire 64111 2D engine */
#define SPITFIRE_CP_STATUS      0x10
#define     SPITFIRE_CP_BUSY                    0x80
#define SPITFIRE_CP_CONTROL     0x11
#define     SPITFIRE_INT_PENDING                0x10
#define     SPITFIRE_TERMINATE_OP               0x20
#define     SPITFIRE_ENABLE_TURBO               0x40
#define SPITFIRE_PIXMAP_SELECT  0x12
#define     SPITFIRE_INDEX_MASK                 0
#define     SPITFIRE_INDEX_PIXMAP_A             1
#define     SPITFIRE_INDEX_PIXMAP_B             2
#define     SPITFIRE_INDEX_PIXMAP_C             3

/* BASE, WIDTH must always be DWORD aligned */
#define SPITFIRE_PIXMAP_BASE    0x14
#define SPITFIRE_PIXMAP_WIDTH   0x18
#define SPITFIRE_PIXMAP_HEIGHT  0x1a

#define SPITFIRE_PIXMAP_FORMAT  0x1c
#define     SPITFIRE_FORMAT_1BPP                0x00
#define     SPITFIRE_FORMAT_8BPP                0x03
#define     SPITFIRE_FORMAT_16BPP               0x04
#define     SPITFIRE_FORMAT_32BPP               0x05
#define     SPITFIRE_FORMAT_INTEL               0x00
#define     SPITFIRE_FORMAT_MOTOROLA            0x08
#define     SPITFIRE_FORMAT_VIDEOMEM            0x00
#define     SPITFIRE_FORMAT_PHYSMEM             0x80
#define SPITFIRE_BRESENHAM_ERR  0x20
#define SPITFIRE_BRESENHAM_K1   0x24
#define SPITFIRE_BRESENHAM_K2   0x28
#define SPITFIRE_SHORT_STROKE   0x2c

#define SPITFIRE_ROPMIX          0x48


#define SPITFIRE_DEST_CC_COND   0x4a
#define SPITFIRE_DEST_CC_COLOR  0x4c
#define SPITFIRE_PIXEL_BITMASK  0x50
#define SPITFIRE_FGCOLOR        0x58
#define SPITFIRE_BGCOLOR        0x5c
#define SPITFIRE_OP_DIM_1       0x60
#define SPITFIRE_OP_DIM_2       0x62

#define SPITFIRE_OFFSET_X_MAP   0x6c
#define SPITFIRE_OFFSET_Y_MAP   0x6e
#define SPITFIRE_OFFSET_X_SRC   0x70
#define SPITFIRE_OFFSET_Y_SRC   0x72
#define SPITFIRE_OFFSET_X_PAT   0x74
#define SPITFIRE_OFFSET_Y_PAT   0x76
#define SPITFIRE_OFFSET_X_DST   0x78
#define SPITFIRE_OFFSET_Y_DST   0x7a
#define SPITFIRE_COMMAND        0x7c
#define     SPITFIRE_YMAJOR                     0x00000001UL
#define     SPITFIRE_DEC_Y                      0x00000002UL
#define     SPITFIRE_DEC_X                      0x00000004UL
#define     SPITFIRE_DRAW_ALL                   0
#define     SPITFIRE_DRAW_OMIT_FIRST            0x00000010UL
#define     SPITFIRE_DRAW_OMIT_LAST             0x00000020UL
#define     SPITFIRE_DRAW_BOUNDARY              0x00000030UL
#define     SPITFIRE_MASK_NONE                  0
#define     SPITFIRE_MASK_BOUNDARY              0x00000040UL
#define     SPITFIRE_MASK_MAP                   0x00000080UL
#define     SPITFIRE_PAT_PIXMAP_A               0x00001000UL
#define     SPITFIRE_PAT_PIXMAP_B               0x00002000UL
#define     SPITFIRE_PAT_PIXMAP_C               0x00003000UL
#define     SPITFIRE_PAT_FOREGROUND             0x00008000UL
#define     SPITFIRE_PAT_FROM_SOURCE            0x00009000UL
#define     SPITFIRE_DST_PIXMAP_A               0x00010000UL
#define     SPITFIRE_DST_PIXMAP_B               0x00020000UL
#define     SPITFIRE_DST_PIXMAP_C               0x00030000UL
#define     SPITFIRE_SRC_PIXMAP_A               0x00100000UL
#define     SPITFIRE_SRC_PIXMAP_B               0x00200000UL
#define     SPITFIRE_SRC_PIXMAP_C               0x00300000UL
#define     SPITFIRE_CMD_SHORT_STROKE_READ      0x02000000UL
#define     SPITFIRE_CMD_LINE_DRAW_READ         0x03000000UL
#define     SPITFIRE_CMD_SHORT_STROKE_WRITE     0x04000000UL
#define     SPITFIRE_CMD_LINE_DRAW_WRITE        0x05000000UL
#define     SPITFIRE_CMD_BITBLT                 0x08000000UL
#define     SPITFIRE_CMD_INV_BITBLT             0x09000000UL
#define     SPITFIRE_CMD_FILL                   0x0a000000UL
#define     SPITFIRE_CMD_TEXT_BITBLT            0x0b000000UL
#define     SPITFIRE_CMD_PATTERN_COPY           0x0c000000UL
#define     SPITFIRE_FORE_SRC_FGCOLOR           0
#define     SPITFIRE_FORE_SRC_PIXMAP            0x20000000UL
#define     SPITFIRE_BACK_SRC_BGCOLOR           0
#define     SPITFIRE_BACK_SRC_PIXMAP            0x80000000UL

Bool SpitfireInitAccel(ScreenPtr pScreen);
Bool WaitIdleEmpty(ScrnInfoPtr pScrn);

#endif

