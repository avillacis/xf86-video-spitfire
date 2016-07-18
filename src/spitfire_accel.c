#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "compiler.h"
#include "vgaHW.h"

#include "fb.h"

#include "shadowfb.h"

#include "mipointer.h"
#include "micmap.h"
#include "xf86int10.h"

#ifdef XSERVER_LIBPCIACCESS
#include <pciaccess.h>
#endif

#include "exa.h"
#include <X11/Xarch.h>
#include "miline.h"

#include "spitfire_driver.h"
#include  "spitfire_accel.h"

#ifdef HAVE_XAA_H
#include "xaalocal.h"
#endif
#include "xaarop.h"

Bool SpitfireEXAInit(ScreenPtr pScreen);
Bool SpitfireXAAInit(ScreenPtr pScreen);

void SpitfireAccelSync(ScrnInfoPtr pScrn);

#ifdef HAVE_XAA_H
static void 
SpitfireSetupForScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int xdir, 
    int ydir,
    int rop,
    unsigned planemask,
    int transparency_color);
static void 
SpitfireSubsequentScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int w,
    int h);

static void SpitfireSetupForSolidFill(
    ScrnInfoPtr pScrn,
    int color, 
    int rop,
    unsigned int planemask);
static void SpitfireSubsequentSolidFillRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h);
static void SpitfireSetupForMono8x8PatternFill(
	ScrnInfoPtr pScrn,
	int patx, int paty,
	int fg, int bg,
	int rop,
	unsigned int planemask
   );
static void SpitfireSubsequentMono8x8PatternFillRect(
	ScrnInfoPtr pScrn,
	int patx, int paty,
	int x, int y, int w, int h
   );
#endif

Bool SpitfireInitAccel(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);

    pdrv->lDelta = pScrn->virtualX * (pScrn->bitsPerPixel >> 3);
    pdrv->Bpp = pScrn->bitsPerPixel >> 3;
    pdrv->Bpl = pScrn->displayWidth * pdrv->Bpp;
    pdrv->cxMemory = pdrv->lDelta / (pdrv->Bpp);
    pdrv->cyMemory = pdrv->endfb / pdrv->lDelta - 1;

    if (pdrv->useEXA)
        return SpitfireEXAInit(pScreen);
    else
        return SpitfireXAAInit(pScreen);
}

Bool SpitfireXAAInit(ScreenPtr pScreen)
{
#ifdef HAVE_XAA_H
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    XAAInfoRecPtr xaaptr;
    BoxRec AvailFBArea;
    Bool pixmapsSupported;

    /* General acceleration flags */

    if (!(xaaptr = pdrv->AccelInfoRec = XAACreateInfoRec())) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Failed to allocate XAAInfoRec.\n");
        return FALSE;
    }

    /* The 64111 supports ROPs with pixmaps of 1, 8, 16, 32 bits per pixel, BUT
       NOT with 24 bits per pixel. So throughout the code that uses 24-bit 
       pixmaps will only program the GE at 8 bits per pixel. This only works 
       with pitches less than 4096 bytes. Operations that could possibly mix 
       24-bit with 1-bit patterns are out of luck - they cannot be accelerated.
     */
    pixmapsSupported = (pScrn->bitsPerPixel != 24 || pdrv->Bpl < 4096);

    xaaptr->Flags = 0
        | PIXMAP_CACHE
        | OFFSCREEN_PIXMAPS
        | LINEAR_FRAMEBUFFER
	;

    /* Sync with graphics engine */
    xaaptr->Sync = SpitfireAccelSync;

    /* Clipping */

    /* ScreenToScreen copies */
    if (pixmapsSupported) {
        xaaptr->ScreenToScreenCopyFlags = 0
            | NO_PLANEMASK;

        /* No way to define full color keying with 24-bit pixmaps, RGB_EQUAL 
           does not apply to this operation. */
        if (pScrn->bitsPerPixel == 24)
            xaaptr->ScreenToScreenCopyFlags |= NO_TRANSPARENCY;

        xaaptr->SetupForScreenToScreenCopy = SpitfireSetupForScreenToScreenCopy;
        xaaptr->SubsequentScreenToScreenCopy = SpitfireSubsequentScreenToScreenCopy;
    }

    /* Solid filled rectangles */
    if (pixmapsSupported) {
        xaaptr->SolidFillFlags = 0
            | NO_PLANEMASK;

        /* 24-bit mode treated as 8-bit, only supports grayscale filling  */
        if (pScrn->bitsPerPixel == 24) xaaptr->SolidFillFlags |= RGB_EQUAL;

        xaaptr->SetupForSolidFill = SpitfireSetupForSolidFill;
        xaaptr->SubsequentSolidFillRect = SpitfireSubsequentSolidFillRect;
    }

    /* Solid lines */


    /* Mono 8x8 pattern fills, cannot be accelerated in 24 bpp */
    if (pScrn->bitsPerPixel != 24) {
        xaaptr->Mono8x8PatternFillFlags = 0
            | NO_PLANEMASK
            | BIT_ORDER_IN_BYTE_LSBFIRST
            | HARDWARE_PATTERN_PROGRAMMED_ORIGIN
            ;
        xaaptr->SetupForMono8x8PatternFill = SpitfireSetupForMono8x8PatternFill;
        xaaptr->SubsequentMono8x8PatternFillRect = SpitfireSubsequentMono8x8PatternFillRect;
    }
    /* ImageWrite */

    /* Set up screen parameters. */

    xaaptr->maxOffPixWidth = 0xFFF;
    xaaptr->maxOffPixHeight = 0xFFF;

    AvailFBArea.x1 = 0;
    AvailFBArea.y1 = 0;
    AvailFBArea.x2 = pdrv->cxMemory;
    AvailFBArea.y2 = pdrv->cyMemory;

    xf86InitFBManager(pScreen, &AvailFBArea);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "Using %d lines for offscreen memory.\n",
                   pdrv->cyMemory - pScrn->virtualY );


    return XAAInit(pScreen, xaaptr);
#else
    return FALSE;
#endif
}

#define MAXLOOP			0xffffff
void SpitfireAccelSync(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int loop = 0;
    
    /* Wait for BUSY bit to change to zero */
    while ((MMIO_IN8(SPITFIRE_MMIO, SPITFIRE_CP_STATUS) & SPITFIRE_CP_BUSY) && (loop++ < MAXLOOP));
}

/* Helper function to program a video address, width and height of a on-screen pixmap */
static void
SpitfireSetupPixMap(
    SpitfirePtr pdrv,
    unsigned int pixIndex,
    CARD32 pixAddr,
    CARD16 pixWidth,
    CARD16 pixHeight,
    CARD8 pixFormat)
{
    /* Select which of the 4 pixmaps is being defined */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_PIXMAP_SELECT, pixIndex);

    /* Location of this pixmap */
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_PIXMAP_BASE, pixAddr);

    /* Program dimensions of the pixmap */
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_PIXMAP_WIDTH,  pixWidth);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_PIXMAP_HEIGHT, pixHeight);
    
    /* Program pixel format. All pixmaps are assumed to be in framebuffer, not in system memory. */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_PIXMAP_FORMAT, pixFormat | SPITFIRE_FORMAT_VIDEOMEM);
}

#ifdef HAVE_XAA_H
static void 
SpitfireSetupForScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int xdir, 
    int ydir,
    int rop,
    unsigned planemask, /* <-- not supported */
    int transparency_color)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int cmd;

    cmd = SPITFIRE_CMD_BITBLT
        | SPITFIRE_SRC_PIXMAP_A
        | SPITFIRE_PAT_FOREGROUND
        | SPITFIRE_DST_PIXMAP_C
        | SPITFIRE_FORE_SRC_PIXMAP
        | SPITFIRE_BACK_SRC_PIXMAP;
    if (xdir == -1) cmd |= SPITFIRE_DEC_X;
    if (ydir == -1) cmd |= SPITFIRE_DEC_Y;

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    if (transparency_color != -1) {
        MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, transparency_color);
        MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 2); /* Update on != transparency_color */
    } else {
        MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, 0);
        MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 6); /* Always update */
    }
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_ROPMIX, XAAGetCopyROP(rop));
    
    /* Set up source and destination pixmaps to the entire framebuffer */
    if (pScrn->bitsPerPixel != 24) {
        CARD8 pixFormat = 0;

        switch (pScrn->bitsPerPixel) {
        case 8:  pixFormat = SPITFIRE_FORMAT_8BPP;  break;
        case 16: pixFormat = SPITFIRE_FORMAT_16BPP; break;
        case 32: pixFormat = SPITFIRE_FORMAT_32BPP; break;
        }
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_A, 
            0, pdrv->cxMemory - 1, pdrv->cyMemory - 1, 
            pixFormat);
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_C, 
            0, pdrv->cxMemory - 1, pdrv->cyMemory - 1, 
            pixFormat);
    } else {
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_A, 
            0, pdrv->cxMemory * 3 - 1, pdrv->cyMemory - 1, 
            SPITFIRE_FORMAT_8BPP);
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_C, 
            0, pdrv->cxMemory * 3 - 1, pdrv->cyMemory - 1, 
            SPITFIRE_FORMAT_8BPP);
    }
    
    pdrv->SavedAccelCmd = cmd;
}

static void 
SpitfireSubsequentScreenToScreenCopy(
    ScrnInfoPtr pScrn,
    int x1,
    int y1,
    int x2,
    int y2,
    int w,
    int h)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);

    /* On 24bpp, we are pretending to work at 8bpp, so triple all dimensions */
    if (pScrn->bitsPerPixel == 24) {
        x1 *= 3; x2 *= 3; w *= 3;
    }

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_1, w - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_2, h - 1);

    /* When specifying SPITFIRE_DEC_[XY], we need to specify the rightmost or
     * bottommost pixel coordinate, as required. */
    if (pdrv->SavedAccelCmd & SPITFIRE_DEC_X) {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, x1 + w - 1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, x2 + w - 1);
    } else {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, x1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, x2);
    }
    if (pdrv->SavedAccelCmd & SPITFIRE_DEC_Y) {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, y1 + h - 1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, y2 + h - 1);
    } else {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, y1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, y2);
    }

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_COMMAND, pdrv->SavedAccelCmd);
}

static void SpitfireSetupForSolidFill(
    ScrnInfoPtr pScrn,
    int color, 
    int rop,
    unsigned int planemask)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int cmd;

    cmd = SPITFIRE_CMD_FILL
        | SPITFIRE_PAT_FOREGROUND
        | SPITFIRE_DST_PIXMAP_C
        | SPITFIRE_FORE_SRC_FGCOLOR /* <-- Use foreground color, not pixmap, as source */
        | SPITFIRE_BACK_SRC_BGCOLOR;/* <-- Use background color, not pixmap, as source */

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_FGCOLOR, color);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_BGCOLOR, color);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, 0);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 6); /* Always update */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_ROPMIX, XAAGetCopyROP(rop));

    /* Set up source and destination pixmaps to the entire framebuffer */
    if (pScrn->bitsPerPixel != 24) {
        CARD8 pixFormat = 0;

        switch (pScrn->bitsPerPixel) {
        case 8:  pixFormat = SPITFIRE_FORMAT_8BPP;  break;
        case 16: pixFormat = SPITFIRE_FORMAT_16BPP; break;
        case 32: pixFormat = SPITFIRE_FORMAT_32BPP; break;
        }
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_C, 
            0, pdrv->cxMemory - 1, pdrv->cyMemory - 1, 
            pixFormat);
    } else {
        SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_C, 
            0, pdrv->cxMemory * 3 - 1, pdrv->cyMemory - 1, 
            SPITFIRE_FORMAT_8BPP);
    }
    
    pdrv->SavedAccelCmd = cmd;
}

static void SpitfireSubsequentSolidFillRect(
    ScrnInfoPtr pScrn,
    int x,
    int y,
    int w,
    int h)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);

    /* On 24bpp, we are pretending to work at 8bpp, so triple all dimensions */
    if (pScrn->bitsPerPixel == 24) {
        x *= 3; w *= 3;
    }

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_1, w - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_2, h - 1);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, x);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, x);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, y);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, y);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_COMMAND, pdrv->SavedAccelCmd);
}

static void SpitfireSetupForMono8x8PatternFill(
	ScrnInfoPtr pScrn,
	int patx, int paty,
	int fg, int bg,
	int rop,
	unsigned int planemask)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    CARD8 pixFormat = 0;
    unsigned int cmd;
    CARD32 patoffset;

    cmd = SPITFIRE_CMD_BITBLT
        | SPITFIRE_SRC_PIXMAP_A
        | SPITFIRE_PAT_PIXMAP_B
        | SPITFIRE_DST_PIXMAP_C
        | SPITFIRE_FORE_SRC_FGCOLOR /* <-- Use foreground color, not pixmap, as source */
        | ((bg != -1) 
            ? SPITFIRE_BACK_SRC_BGCOLOR /* <-- Use background color, not pixmap, as source */
            : SPITFIRE_BACK_SRC_PIXMAP) /* <-- Use source pixmap, so should be a noop */
        ;

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_FGCOLOR, fg);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_BGCOLOR, bg);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, 0);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 6); /* Always update */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_ROPMIX, XAAGetCopyROP(rop));

    /* Set up destination pixmap to the entire framebuffer */
    switch (pScrn->bitsPerPixel) {
    case 8:  pixFormat = SPITFIRE_FORMAT_8BPP;  break;
    case 16: pixFormat = SPITFIRE_FORMAT_16BPP; break;
    case 32: pixFormat = SPITFIRE_FORMAT_32BPP; break;
    }
    SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_A, 
        0, pdrv->cxMemory - 1, pdrv->cyMemory - 1, 
        pixFormat);
    SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_C, 
        0, pdrv->cxMemory - 1, pdrv->cyMemory - 1, 
        pixFormat);

    /* Set up pattern pixmap */

    patoffset = (pdrv->cxMemory * paty + patx) * pdrv->Bpp;
    SpitfireSetupPixMap(pdrv, SPITFIRE_INDEX_PIXMAP_B,
        patoffset, 8 - 1, 8 - 1, 
        SPITFIRE_FORMAT_1BPP | SPITFIRE_FORMAT_INTEL);
    pdrv->SavedAccelCmd = cmd;
}

static void SpitfireSubsequentMono8x8PatternFillRect(
	ScrnInfoPtr pScrn,
	int patx, int paty,
	int x, int y, int w, int h)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    CARD32 patoffset = 0;

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_1, w - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_2, h - 1);
 
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, x);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, y);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_PAT, patx);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_PAT, paty);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, x);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, y);
 
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_COMMAND, pdrv->SavedAccelCmd);
}
#endif

static Bool
SpitfirePrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg);

static void
SpitfireSolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2);

static void
SpitfireDoneSolid(PixmapPtr pPixmap);

static Bool
SpitfirePrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir, int ydir,
					int alu, Pixel planemask);

static void
SpitfireCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY, int width, int height);

static void
SpitfireDoneCopy(PixmapPtr pDstPixmap);



static void SpitfireExaSync(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfireAccelSync(pScrn);
}

Bool SpitfireEXAInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);

    if (!(pdrv->EXADriverPtr = exaDriverAlloc())) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
        	"Failed to allocate EXADriverRec.\n");
        return FALSE;
    }
    
    pdrv->EXADriverPtr->exa_major = 2;
    pdrv->EXADriverPtr->exa_minor = 0;
    
    /* use the linear aperture */
    pdrv->EXADriverPtr->memoryBase = pdrv->FBBase + pScrn->fbOffset;
    pdrv->EXADriverPtr->memorySize = pdrv->videoRambytes;
    pdrv->EXADriverPtr->offScreenBase = pScrn->virtualY * pdrv->lDelta;

    if (pdrv->EXADriverPtr->memorySize > pdrv->EXADriverPtr->offScreenBase) {
        pdrv->EXADriverPtr->flags = EXA_OFFSCREEN_PIXMAPS;
    } else {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "Not enough video RAM for EXA offscreen memory manager.\n");
    }
    pdrv->EXADriverPtr->pixmapPitchAlign = 32;
    pdrv->EXADriverPtr->pixmapOffsetAlign = 8;

    /* engine has 12 bit coordinates */
    pdrv->EXADriverPtr->maxX = 4096;
    pdrv->EXADriverPtr->maxY = 4096;

    /* Sync */
    pdrv->EXADriverPtr->WaitMarker = SpitfireExaSync;

    /* Solid fill */
    pdrv->EXADriverPtr->PrepareSolid = SpitfirePrepareSolid;
    pdrv->EXADriverPtr->Solid = SpitfireSolid;
    pdrv->EXADriverPtr->DoneSolid = SpitfireDoneSolid;

    /* Copy */
    pdrv->EXADriverPtr->PrepareCopy = SpitfirePrepareCopy;
    pdrv->EXADriverPtr->Copy = SpitfireCopy;
    pdrv->EXADriverPtr->DoneCopy = SpitfireDoneCopy;

    if(!exaDriverInit(pScreen, pdrv->EXADriverPtr)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "exaDriverinit failed.\n");
        return FALSE;
    } else {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "Spitfire EXA Acceleration enabled.\n");
        return TRUE;
    }
}

static int SpitfireGetSolidROP(int rop) {

    int ALUSolidROP[16] =
    {
    /* GXclear      */      0x00,         /* 0 */
    /* GXand        */      0xA0,         /* src AND dst */
    /* GXandReverse */      0x50,         /* src AND NOT dst */
    /* GXcopy       */      0xF0,         /* src */
    /* GXandInverted*/      0x0A,         /* NOT src AND dst */
    /* GXnoop       */      0xAA,         /* dst */
    /* GXxor        */      0x5A,         /* src XOR dst */
    /* GXor         */      0xFA,         /* src OR dst */
    /* GXnor        */      0x05,         /* NOT src AND NOT dst */
    /* GXequiv      */      0xA5,         /* NOT src XOR dst */
    /* GXinvert     */      0x55,         /* NOT dst */
    /* GXorReverse  */      0xF5,         /* src OR NOT dst */
    /* GXcopyInverted*/     0x0F,         /* NOT src */
    /* GXorInverted */      0xAF,         /* NOT src OR dst */
    /* GXnand       */      0x5F,         /* NOT src OR NOT dst */
    /* GXset        */      0xFF,         /* 1 */

    };
    return (ALUSolidROP[rop]);
}

int SpitfireGetCopyROP(int rop) {

    int ALUCopyROP[16] =
    {
       0x00, /*ROP_0 GXclear */
       0x88, /*ROP_DSa GXand */
       0x44, /*ROP_SDna GXandReverse */
       0xCC, /*ROP_S GXcopy */
       0x22, /*ROP_DSna GXandInverted */
       0xAA, /*ROP_D GXnoop */
       0x66, /*ROP_DSx GXxor */
       0xEE, /*ROP_DSo GXor */
       0x11, /*ROP_DSon GXnor */
       0x99, /*ROP_DSxn GXequiv */
       0x55, /*ROP_Dn GXinvert*/
       0xDD, /*ROP_SDno GXorReverse */
       0x33, /*ROP_Sn GXcopyInverted */
       0xBB, /*ROP_DSno GXorInverted */
       0x77, /*ROP_DSan GXnand */
       0xFF, /*ROP_1 GXset */
    };

    return (ALUCopyROP[rop]);
}

static void SpitfireEXASetupPixmap(SpitfirePtr pdrv, PixmapPtr pPixmap, unsigned int index)
{
    unsigned long xpix, ypix;


    xpix = exaGetPixmapPitch(pPixmap) / (pPixmap->drawable.bitsPerPixel >> 3);
    ypix = pPixmap->drawable.height;
    if (pPixmap->drawable.bitsPerPixel != 24) {
        CARD8 pixFormat = 0;

        switch (pPixmap->drawable.bitsPerPixel) {
        case 8:  pixFormat = SPITFIRE_FORMAT_8BPP;  break;
        case 16: pixFormat = SPITFIRE_FORMAT_16BPP; break;
        case 32: pixFormat = SPITFIRE_FORMAT_32BPP; break;
        }
        SpitfireSetupPixMap(pdrv, index, 
            exaGetPixmapOffset(pPixmap), xpix - 1, ypix - 1, 
            pixFormat);
    } else {
        SpitfireSetupPixMap(pdrv, index, 
            exaGetPixmapOffset(pPixmap), xpix * 3 - 1, ypix - 1, 
            SPITFIRE_FORMAT_8BPP);
    }
}

static Bool
SpitfirePrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int cmd;

    cmd = SPITFIRE_CMD_FILL
        | SPITFIRE_PAT_FOREGROUND
        | SPITFIRE_DST_PIXMAP_C
        | SPITFIRE_FORE_SRC_FGCOLOR /* <-- Use foreground color, not pixmap, as source */
        | SPITFIRE_BACK_SRC_BGCOLOR;/* <-- Use background color, not pixmap, as source */

    /* Cannot accelerate solid fill for 24-bit if not grayscale */
    if (pPixmap->drawable.bitsPerPixel == 24 && 
        (((fg & 0x0000FF) != ((fg >> 8) & 0x0000FF)) || ((fg & 0x0000FF) != ((fg >> 16) & 0x0000FF))))
        return FALSE;

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_FGCOLOR, fg);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_BGCOLOR, fg);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_PIXEL_BITMASK, planemask);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, 0);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 6); /* Always update */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_ROPMIX, SpitfireGetCopyROP(alu));

    /* Set up destination pixmap */
    SpitfireEXASetupPixmap(pdrv, pPixmap, SPITFIRE_INDEX_PIXMAP_C);
    
    pdrv->SavedAccelCmd = cmd;
    return TRUE;
}

static void
SpitfireSolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int w = x2 - x1;
    int h = y2 - y1;

    /* On 24bpp, we are pretending to work at 8bpp, so triple all dimensions */
    if (pPixmap->drawable.bitsPerPixel == 24) {
        x1 *= 3; w *= 3;
    }

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_1, w - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_2, h - 1);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, x1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, x1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, y1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, y1);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_COMMAND, pdrv->SavedAccelCmd);
}

static void
SpitfireDoneSolid(PixmapPtr pPixmap)
{
    // Nothing needs to be done here.
}

static Bool
SpitfirePrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir, int ydir,
					int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrcPixmap->drawable.pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int cmd;

    cmd = SPITFIRE_CMD_BITBLT
        | SPITFIRE_SRC_PIXMAP_A
        | SPITFIRE_PAT_FOREGROUND
        | SPITFIRE_DST_PIXMAP_C
        | SPITFIRE_FORE_SRC_PIXMAP
        | SPITFIRE_BACK_SRC_PIXMAP;
    if (xdir < 0) cmd |= SPITFIRE_DEC_X;
    if (ydir < 0) cmd |= SPITFIRE_DEC_Y;

    // Cannot accelerate copy when just one of the pixmaps is 24bpp
    if ((pSrcPixmap->drawable.bitsPerPixel == 24 || pDstPixmap->drawable.bitsPerPixel == 24)
        && pSrcPixmap->drawable.bitsPerPixel != pDstPixmap->drawable.bitsPerPixel)
        return FALSE;

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_PIXEL_BITMASK, planemask);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COLOR, 0);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_DEST_CC_COND, 6); /* Always update */
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_ROPMIX, SpitfireGetCopyROP(alu));
    
    /* Set up source and destination pixmaps */
    SpitfireEXASetupPixmap(pdrv, pSrcPixmap, SPITFIRE_INDEX_PIXMAP_A);
    SpitfireEXASetupPixmap(pdrv, pDstPixmap, SPITFIRE_INDEX_PIXMAP_C);

    pdrv->SavedAccelCmd = cmd;
    return TRUE;
}

static void
SpitfireCopy(PixmapPtr pDstPixmap, int srcX, int srcY, int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);

    /* On 24bpp, we are pretending to work at 8bpp, so triple all dimensions */
    if (pDstPixmap->drawable.bitsPerPixel == 24) {
        srcX *= 3; dstX *= 3; width *= 3;
    }

    /* Need to wait for previous accel operation to finish, otherwise
     * output gets scrambled. */
    SpitfireAccelSync(pScrn);

    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_1, width - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OP_DIM_2, height - 1);

    /* When specifying SPITFIRE_DEC_[XY], we need to specify the rightmost or
     * bottommost pixel coordinate, as required. */
    if (pdrv->SavedAccelCmd & SPITFIRE_DEC_X) {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, srcX + width - 1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, dstX + width - 1);
    } else {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_SRC, srcX);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_X_DST, dstX);
    }
    if (pdrv->SavedAccelCmd & SPITFIRE_DEC_Y) {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, srcY + height - 1);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, dstY + height - 1);
    } else {
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_SRC, srcY);
        MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OFFSET_Y_DST, dstY);
    }

    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_COMMAND, pdrv->SavedAccelCmd);
}

static void
SpitfireDoneCopy(PixmapPtr pDstPixmap)
{
    // Nothing needs to be done here.
}

