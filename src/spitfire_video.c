/*
 * Xv adaptor for the video overlay of the OTI-64111.
 *
 * The 64111 mixes up to two video windows into the picture as it scans it
 * out, each from its own buffer in video memory, converting YUV to RGB and
 * scaling up with interpolation. It is not in the 64107 datasheet; the
 * registers come from OAK's Windows 3.1 driver for the 64111 (OAKVGA.DRV
 * 2.403) and were checked on the card (init_64111/oakoverlay and the KMS
 * driver oak64111). Window 0 is at MMIO 0xb0-0xbf:
 *
 *  - 0xb0/0xb2: X and Y on screen; 0xb4: width - 1; 0xb6: (height - 1)
 *    XOR 0x7ff (11 bits each).
 *  - 0xb8-0xba: address of the source in video memory, in 8-byte units
 *    (written as a dword, 0xbb is set after it); 0xbb: source pitch in
 *    16-byte units; 0xbf: qwords fetched per line.
 *  - 0xbc-0xbe: bits 8-0 horizontal scale, bits 20-12 vertical, (source
 *    << 9) / destination, 0 for 1:1: the window only scales up; bit 9
 *    horizontal and bit 21 vertical interpolation; bit 22 the destination
 *    color key (0xd0-0xd2, in the pixel format of the screen: at 8 bpp a
 *    palette index): the video only shows where the screen has the key;
 *    bit 23 shows the window.
 *  - 0xd3: source format, common to both windows: 0x98 YUY2, 0x90 UYVY.
 *  - 0xd7: how the two windows share the line buffer; 0x20 with one.
 *  - A window fetches at most 720 bytes (0x5a qwords) per line, through a
 *    line buffer that the two windows share on common lines.
 *
 * Besides, OAK 0x06 bit 5 must be 0 for any window to show (every mode set
 * writes it to 1), and the low nibble of EX30 must be 3, or the colors of
 * the video come out wrong at 8 and 16 bpp (see SpitfireModeInit).
 *
 * The adaptor has one port, on window 0, with the destination color key,
 * so that windows over the video hide it as X expects. It takes YUY2, UYVY,
 * YV12 and I420, and always gives window 0 YUY2: the visible part of the
 * image is converted and, where it must, reduced on the CPU as it goes into
 * video memory: to the size of the window, since the hardware only scales
 * up, and to the bytes per line that the line buffer and the memory
 * bandwidth left by the mode allow (see SPITFIRE_BW_KBS). What is left the
 * window scales up with interpolation. The images go into two buffers in
 * turn (one if only one fits), from the offscreen memory of EXA, or with
 * no EXA from the memory below the cursor images. Interlaced and double
 * scan modes are refused: where the chip puts the window in them is not
 * known.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "spitfire_driver.h"
#include <X11/extensions/Xv.h>
#include "fourcc.h"
#include "regionstr.h"

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)

#define SPITFIRE_OVL_X          0xb0
#define SPITFIRE_OVL_Y          0xb2
#define SPITFIRE_OVL_W          0xb4
#define SPITFIRE_OVL_H          0xb6
#define SPITFIRE_OVL_ADDR       0xb8
#define SPITFIRE_OVL_PITCH      0xbb
#define SPITFIRE_OVL_CTRL       0xbc
#define SPITFIRE_OVL_FETCH      0xbf
#define SPITFIRE_OVL_KEY        0xd0
#define SPITFIRE_OVL_FORMAT     0xd3
#define SPITFIRE_OVL_SHARE      0xd7
#define SPITFIRE_OVL_HFILTER    (1 << 9)
#define SPITFIRE_OVL_VFILTER    (1 << 21)
#define SPITFIRE_OVL_KEYED      (1 << 22)
#define SPITFIRE_OVL_ON         (1 << 23)
#define SPITFIRE_OVL_YUY2       0x98
#define SPITFIRE_OVL_LINE_BYTES 720
#define SPITFIRE_OVL_OFF        0x20    /* OAK 0x06 bit 5 */

#define SPITFIRE_MAX_IMAGE      2048

typedef struct {
    /* The last image and where it goes, for SpitfireReputImage */
    unsigned char       *image;
    int                 imageSize;
    int                 id;
    short               width, height;
    short               src_x, src_y, src_w, src_h;
    short               drw_x, drw_y, drw_w, drw_h;
    RegionRec           clip;           /* where the key was painted */

    CARD32              colorKey;
    Bool                autopaint;
    Bool                on;

    /* Buffers in video memory */
    ExaOffscreenArea    *area;
    unsigned long       base;           /* from the start of video memory */
    int                 bufSize;        /* bytes of one buffer */
    int                 nbufs, buf;
} SpitfirePortPrivRec, *SpitfirePortPrivPtr;

static Atom xvColorKey, xvAutopaint;

static XF86VideoEncodingRec SpitfireEncodings[] = {
    { 0, "XV_IMAGE", SPITFIRE_MAX_IMAGE, SPITFIRE_MAX_IMAGE, { 1, 1 } }
};

static XF86VideoFormatRec SpitfireFormats[] = {
    { 8, PseudoColor }, { 15, TrueColor }, { 16, TrueColor }, { 24, TrueColor }
};

static XF86AttributeRec SpitfireAttributes[] = {
    { XvSettable | XvGettable, 0, (1 << 24) - 1, "XV_COLORKEY" },
    { XvSettable | XvGettable, 0, 1, "XV_AUTOPAINT_COLORKEY" },
};

static XF86ImageRec SpitfireImages[] = {
    XVIMAGE_YUY2,
    XVIMAGE_UYVY,
    XVIMAGE_YV12,
    XVIMAGE_I420,
};

/* OAK 0x06 bit 5: 0 while the window is shown */
static void SpitfireOverlayEnable(Bool on)
{
    unsigned char v;

    outb(SPITFIRE_INDEX, SPITFIRE_CLOCKSEL);
    v = inb(SPITFIRE_DATA) & ~0x80;     /* bit 7 latches a clock */
    OTI_OUTB(on ? v & ~SPITFIRE_OVL_OFF : v | SPITFIRE_OVL_OFF, SPITFIRE_CLOCKSEL);
}

static void SpitfireOverlayOff(ScrnInfoPtr pScrn, SpitfirePortPrivPtr pPriv)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);

    if (pPriv->on && pScrn->vtSema) {
        MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_CTRL + 2, 0);
        SpitfireOverlayEnable(FALSE);
    }
    pPriv->on = FALSE;
}

static void SpitfireFreeBuffers(ScrnInfoPtr pScrn, SpitfirePortPrivPtr pPriv)
{
    if (pPriv->area)
        exaOffscreenFree(pScrn->pScreen, pPriv->area);
    pPriv->area = NULL;
    pPriv->nbufs = 0;
    pPriv->bufSize = 0;
}

/* EXA takes the area back (at the end of the server generation) */
static void SpitfireAreaLost(ScreenPtr pScreen, ExaOffscreenArea *area)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePortPrivPtr pPriv = area->privData;

    SpitfireOverlayOff(pScrn, pPriv);
    pPriv->area = NULL;
    pPriv->nbufs = 0;
    pPriv->bufSize = 0;
}

/* Two buffers of size bytes if they fit, else one */
static Bool SpitfireAllocBuffers(ScrnInfoPtr pScrn, SpitfirePortPrivPtr pPriv, int size)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned long start, avail;

    size = (size + 1023) & ~1023;
    if (pPriv->nbufs && pPriv->bufSize >= size)
        return TRUE;
    SpitfireOverlayOff(pScrn, pPriv);
    SpitfireFreeBuffers(pScrn, pPriv);

    if (pdrv->EXADriverPtr) {
        int n;

        for (n = 2; n > 0 && !pPriv->area; n--) {
            pPriv->area = exaOffscreenAlloc(pScrn->pScreen, n * size, 64, TRUE,
                                            SpitfireAreaLost, pPriv);
            if (pPriv->area)
                pPriv->nbufs = n;
        }
        if (!pPriv->area)
            return FALSE;
        pPriv->base = pScrn->fbOffset + pPriv->area->offset;
    } else {
        /* Without EXA nothing else uses the memory below the cursor */
        start = (pScrn->fbOffset + (unsigned long)pScrn->virtualY * pdrv->lDelta + 63) & ~63UL;
        avail = pdrv->endfb > start ? pdrv->endfb - start : 0;
        pPriv->nbufs = avail >= 2UL * size ? 2 : avail >= (unsigned long)size ? 1 : 0;
        if (!pPriv->nbufs)
            return FALSE;
        pPriv->base = start;
    }
    pPriv->bufSize = size;
    pPriv->buf = 0;
    return TRUE;
}

/* Pitches and offsets of the planes of an image; returns its size */
static int SpitfireQueryImageAttributes(ScrnInfoPtr pScrn, int id,
                                        unsigned short *w, unsigned short *h,
                                        int *pitches, int *offsets)
{
    int size, tmp;

    if (*w > SPITFIRE_MAX_IMAGE)
        *w = SPITFIRE_MAX_IMAGE;
    if (*h > SPITFIRE_MAX_IMAGE)
        *h = SPITFIRE_MAX_IMAGE;
    *w = (*w + 1) & ~1;
    if (offsets)
        offsets[0] = 0;

    switch (id) {
    case FOURCC_YV12:
    case FOURCC_I420:
        *h = (*h + 1) & ~1;
        size = (*w + 3) & ~3;
        if (pitches)
            pitches[0] = size;
        size *= *h;
        if (offsets)
            offsets[1] = size;
        tmp = ((*w >> 1) + 3) & ~3;
        if (pitches)
            pitches[1] = pitches[2] = tmp;
        tmp *= (*h >> 1);
        size += tmp;
        if (offsets)
            offsets[2] = size;
        size += tmp;
        break;
    default:                    /* YUY2, UYVY */
        size = *w << 1;
        if (pitches)
            pitches[0] = size;
        size *= *h;
        break;
    }
    return size;
}

/* Memory bandwidth of the mode, and what is left of it for the window */
static int SpitfireOverlayMaxFetch(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    DisplayModePtr mode = pScrn->currentMode;
    long mode_bw, limit, left;

    if (!mode || !mode->Clock || !mode->HTotal)
        return 0;
    /* The memory clock of the mode, see SpitfireModeInit */
    mode_bw = (long)mode->Clock * (pScrn->bitsPerPixel / 8);
    limit = mode_bw > (long)pdrv->BwSlow ? pdrv->BwFast : pdrv->BwSlow;
    left = limit - mode_bw;
    if (left <= 0)
        return 0;
    /* Bytes per line: fetch * Clock / HTotal kB/s */
    left = left * mode->HTotal / mode->Clock;
    return left > SPITFIRE_OVL_LINE_BYTES ? SPITFIRE_OVL_LINE_BYTES : (int)left;
}

/*
 * One line of YUY2 for window 0 from line y of the image: out_w pixels
 * from source pixels x0 + i * step (16.16)
 */
static void SpitfireConvertLine(SpitfirePortPrivPtr pPriv, unsigned char *out,
                                int y, int x0, int out_w, unsigned int step)
{
    unsigned short w = pPriv->width, h = pPriv->height;
    int pitches[3], offsets[3], j;
    const unsigned char *yl, *ul, *vl;

    SpitfireQueryImageAttributes(NULL, pPriv->id, &w, &h, pitches, offsets);

    if (pPriv->id == FOURCC_YV12 || pPriv->id == FOURCC_I420) {
        int uo = pPriv->id == FOURCC_I420 ? offsets[1] : offsets[2];
        int vo = pPriv->id == FOURCC_I420 ? offsets[2] : offsets[1];

        yl = pPriv->image + y * pitches[0];
        ul = pPriv->image + uo + (y >> 1) * pitches[1];
        vl = pPriv->image + vo + (y >> 1) * pitches[1];
        for (j = 0; j < out_w; j += 2) {
            int p0 = x0 + (int)((j * step) >> 16);
            int p1 = x0 + (int)(((j + 1) * step) >> 16);

            out[2 * j] = yl[p0];
            out[2 * j + 1] = ul[p0 >> 1];
            out[2 * j + 2] = yl[p1];
            out[2 * j + 3] = vl[p0 >> 1];
        }
    } else {
        /* Offsets of Y, U and V in a pair of pixels */
        int yo = pPriv->id == FOURCC_UYVY ? 1 : 0;
        int uo = pPriv->id == FOURCC_UYVY ? 0 : 1;

        yl = pPriv->image + y * pitches[0];
        for (j = 0; j < out_w; j += 2) {
            int p0 = x0 + (int)((j * step) >> 16);
            int p1 = x0 + (int)(((j + 1) * step) >> 16);
            const unsigned char *pair = yl + 4 * (p0 >> 1);

            out[2 * j] = yl[2 * p0 + yo];
            out[2 * j + 1] = pair[uo];
            out[2 * j + 2] = yl[2 * p1 + yo];
            out[2 * j + 3] = pair[uo + 2];
        }
    }
}

/*
 * Shows the last image in window 0, with the clip region where the key was
 * painted. Returns Success, or BadAlloc if the window cannot show it.
 */
static int SpitfireDisplayImage(ScrnInfoPtr pScrn, SpitfirePortPrivPtr pPriv)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    ScreenPtr pScreen = pScrn->pScreen;
    DisplayModePtr mode = pScrn->currentMode;
    unsigned char line[SPITFIRE_OVL_LINE_BYTES];
    INT32 x1, x2, y1, y2;
    BoxRec dst, frame;
    RegionRec visible;
    int sx0, sx1, sy0, sy1, sw, sh, dw, dh, out_w, out_h, maxw, fetch;
    int pitch, r, hs, vs;
    unsigned int hstep, vstep;
    unsigned long base;
    CARD32 ctrl;
    Bool shown;

    if (!pScrn->vtSema || !pPriv->image)
        return Success;
    if (!mode || (mode->Flags & (V_INTERLACE | V_DBLSCAN))) {
        SpitfireOverlayOff(pScrn, pPriv);
        return BadAlloc;
    }

    /* The visible part, on the visible frame of the screen */
    frame.x1 = pScrn->frameX0;
    frame.y1 = pScrn->frameY0;
    frame.x2 = pScrn->frameX1 + 1;
    frame.y2 = pScrn->frameY1 + 1;
    REGION_INIT(pScreen, &visible, &frame, 1);
    REGION_INTERSECT(pScreen, &visible, &visible, &pPriv->clip);
    x1 = pPriv->src_x;
    x2 = pPriv->src_x + pPriv->src_w;
    y1 = pPriv->src_y;
    y2 = pPriv->src_y + pPriv->src_h;
    dst.x1 = pPriv->drw_x;
    dst.x2 = pPriv->drw_x + pPriv->drw_w;
    dst.y1 = pPriv->drw_y;
    dst.y2 = pPriv->drw_y + pPriv->drw_h;
    shown = xf86XVClipVideoHelper(&dst, &x1, &x2, &y1, &y2, &visible,
                                  pPriv->width, pPriv->height);
    REGION_UNINIT(pScreen, &visible);
    sx0 = x1 >> 16;
    sx1 = (x2 + 0xffff) >> 16;
    sy0 = y1 >> 16;
    sy1 = (y2 + 0xffff) >> 16;
    if (sx1 > pPriv->width)
        sx1 = pPriv->width;
    if (sy1 > pPriv->height)
        sy1 = pPriv->height;
    sw = sx1 - sx0;
    sh = sy1 - sy0;
    dw = dst.x2 - dst.x1;
    dh = dst.y2 - dst.y1;
    if (!shown || sw < 2 || sh < 1 || dw < 2 || dh < 1) {
        SpitfireOverlayOff(pScrn, pPriv);
        return Success;
    }

    /*
     * Pixels per line for the window: at most those of the window (it
     * only scales up) and those that the line buffer and the bandwidth
     * left by the mode allow (fetched in qwords)
     */
    maxw = (SpitfireOverlayMaxFetch(pScrn) / 8) * 4;
    if (maxw < 2) {
        SpitfireOverlayOff(pScrn, pPriv);
        return BadAlloc;
    }
    out_w = sw < dw ? sw : dw;
    if (out_w > maxw)
        out_w = maxw;
    out_w &= ~1;
    out_h = sh < dh ? sh : dh;
    hstep = ((unsigned int)sw << 16) / out_w;
    vstep = ((unsigned int)sh << 16) / out_h;
    fetch = (out_w * 2 + 7) / 8;
    pitch = (out_w * 2 + 15) & ~15;

    if (!SpitfireAllocBuffers(pScrn, pPriv, pitch * out_h)) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Xv: no video memory for a %dx%d overlay buffer\n", out_w, out_h);
        SpitfireOverlayOff(pScrn, pPriv);
        return BadAlloc;
    }
    if (pPriv->nbufs > 1)
        pPriv->buf ^= 1;
    base = pPriv->base + (unsigned long)pPriv->buf * pPriv->bufSize;

    if (pdrv->EXADriverPtr)
        exaWaitSync(pScreen);
    for (r = 0; r < out_h; r++) {
        SpitfireConvertLine(pPriv, line, sy0 + (int)((r * vstep) >> 16), sx0, out_w, hstep);
        memcpy(pdrv->FBBase + base + (unsigned long)r * pitch, line, out_w * 2);
    }

    /* Scale, (source << 9) / destination, 0 for 1:1 */
    hs = (out_w << 9) / dw;
    vs = (out_h << 9) / dh;
    if (hs > 0x1ff)
        hs = 0;
    if (vs > 0x1ff)
        vs = 0;
    ctrl = SPITFIRE_OVL_ON | SPITFIRE_OVL_HFILTER | SPITFIRE_OVL_VFILTER |
           SPITFIRE_OVL_KEYED | hs | vs << 12;

    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_FORMAT, SPITFIRE_OVL_YUY2);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_KEY, pPriv->colorKey & 0xffff);
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_KEY + 2, (pPriv->colorKey >> 16) & 0xff);
    MMIO_OUT32(SPITFIRE_MMIO, SPITFIRE_OVL_ADDR, base / 8);
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_PITCH, pitch / 16);
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_FETCH, fetch);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_X, dst.x1 - pScrn->frameX0);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_Y, dst.y1 - pScrn->frameY0);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_W, dw - 1);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_H, (dh - 1) ^ 0x7ff);
    MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_CTRL, ctrl & 0xffff);
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_CTRL + 2, ctrl >> 16);
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_SHARE, 0x20);
    if (!pPriv->on)
        SpitfireOverlayEnable(TRUE);
    pPriv->on = TRUE;
    return Success;
}

/*
 * Paints the key where it is not yet (the clip changed) and shows the
 * image. If it cannot be shown, the clip is forgotten: the next window may
 * have the same one, without the key.
 */
static int SpitfireShowImage(ScrnInfoPtr pScrn, SpitfirePortPrivPtr pPriv,
                             RegionPtr clipBoxes, DrawablePtr pDraw)
{
    int ret;

    if (!REGION_EQUAL(pScrn->pScreen, &pPriv->clip, clipBoxes)) {
        REGION_COPY(pScrn->pScreen, &pPriv->clip, clipBoxes);
        if (pPriv->autopaint)
            xf86XVFillKeyHelperDrawable(pDraw, pPriv->colorKey, clipBoxes);
    }
    ret = SpitfireDisplayImage(pScrn, pPriv);
    if (ret != Success)
        REGION_EMPTY(pScrn->pScreen, &pPriv->clip);
    return ret;
}

static int SpitfirePutImage(ScrnInfoPtr pScrn, short src_x, short src_y,
                            short drw_x, short drw_y, short src_w, short src_h,
                            short drw_w, short drw_h, int id, unsigned char *buf,
                            short width, short height, Bool sync,
                            RegionPtr clipBoxes, void *data, DrawablePtr pDraw)
{
    SpitfirePortPrivPtr pPriv = data;
    unsigned short w = width, h = height;
    int size;

    if (width > SPITFIRE_MAX_IMAGE || height > SPITFIRE_MAX_IMAGE)
        return BadAlloc;
    size = SpitfireQueryImageAttributes(pScrn, id, &w, &h, NULL, NULL);
    if (size > pPriv->imageSize) {
        free(pPriv->image);
        if (!(pPriv->image = malloc(size))) {
            pPriv->imageSize = 0;
            return BadAlloc;
        }
        pPriv->imageSize = size;
    }
    memcpy(pPriv->image, buf, size);
    pPriv->id = id;
    pPriv->width = width;
    pPriv->height = height;
    pPriv->src_x = src_x;
    pPriv->src_y = src_y;
    pPriv->src_w = src_w;
    pPriv->src_h = src_h;
    pPriv->drw_x = drw_x;
    pPriv->drw_y = drw_y;
    pPriv->drw_w = drw_w;
    pPriv->drw_h = drw_h;

    return SpitfireShowImage(pScrn, pPriv, clipBoxes, pDraw);
}

/* The window moved or its clip changed: the same image */
static int SpitfireReputImage(ScrnInfoPtr pScrn, short src_x, short src_y,
                              short drw_x, short drw_y, short src_w, short src_h,
                              short drw_w, short drw_h, RegionPtr clipBoxes,
                              void *data, DrawablePtr pDraw)
{
    SpitfirePortPrivPtr pPriv = data;

    if (!pPriv->image)
        return Success;
    pPriv->src_x = src_x;
    pPriv->src_y = src_y;
    pPriv->src_w = src_w;
    pPriv->src_h = src_h;
    pPriv->drw_x = drw_x;
    pPriv->drw_y = drw_y;
    pPriv->drw_w = drw_w;
    pPriv->drw_h = drw_h;
    return SpitfireShowImage(pScrn, pPriv, clipBoxes, pDraw);
}

static void SpitfireStopVideo(ScrnInfoPtr pScrn, void *data, Bool exit)
{
    SpitfirePortPrivPtr pPriv = data;

    REGION_EMPTY(pScrn->pScreen, &pPriv->clip);
    SpitfireOverlayOff(pScrn, pPriv);
    if (exit) {
        if (pScrn->vtSema)
            SpitfireFreeBuffers(pScrn, pPriv);
        free(pPriv->image);
        pPriv->image = NULL;
        pPriv->imageSize = 0;
    }
}

static int SpitfireSetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
                                    INT32 value, void *data)
{
    SpitfirePortPrivPtr pPriv = data;

    if (attribute == xvColorKey) {
        pPriv->colorKey = value & ((1U << pScrn->depth) - 1);
        REGION_EMPTY(pScrn->pScreen, &pPriv->clip);     /* paint it again */
        if (pPriv->on && pScrn->vtSema) {
            SpitfirePtr pdrv = DEVPTR(pScrn);

            MMIO_OUT16(SPITFIRE_MMIO, SPITFIRE_OVL_KEY, pPriv->colorKey & 0xffff);
            MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_KEY + 2, (pPriv->colorKey >> 16) & 0xff);
        }
    } else if (attribute == xvAutopaint) {
        pPriv->autopaint = value != 0;
        REGION_EMPTY(pScrn->pScreen, &pPriv->clip);
    } else {
        return BadMatch;
    }
    return Success;
}

static int SpitfireGetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
                                    INT32 *value, void *data)
{
    SpitfirePortPrivPtr pPriv = data;

    if (attribute == xvColorKey)
        *value = pPriv->colorKey;
    else if (attribute == xvAutopaint)
        *value = pPriv->autopaint;
    else
        return BadMatch;
    return Success;
}

static void SpitfireQueryBestSize(ScrnInfoPtr pScrn, Bool motion,
                                  short vid_w, short vid_h, short drw_w, short drw_h,
                                  unsigned int *p_w, unsigned int *p_h, void *data)
{
    *p_w = drw_w;
    *p_h = drw_h;
}

static XF86VideoAdaptorPtr SpitfireSetupImageVideo(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    XF86VideoAdaptorPtr adapt;
    SpitfirePortPrivPtr pPriv;

    if (!(adapt = calloc(1, sizeof(XF86VideoAdaptorRec) + sizeof(DevUnion) +
                            sizeof(SpitfirePortPrivRec))))
        return NULL;

    adapt->type = XvWindowMask | XvInputMask | XvImageMask;
    adapt->flags = VIDEO_OVERLAID_IMAGES;
    adapt->name = "OAK OTI-64111 Video Overlay";
    adapt->nEncodings = 1;
    adapt->pEncodings = SpitfireEncodings;
    adapt->nFormats = sizeof(SpitfireFormats) / sizeof(SpitfireFormats[0]);
    adapt->pFormats = SpitfireFormats;
    adapt->nPorts = 1;
    adapt->pPortPrivates = (DevUnion *)(&adapt[1]);
    pPriv = (SpitfirePortPrivPtr)(&adapt->pPortPrivates[1]);
    adapt->pPortPrivates[0].ptr = pPriv;
    adapt->nAttributes = sizeof(SpitfireAttributes) / sizeof(SpitfireAttributes[0]);
    adapt->pAttributes = SpitfireAttributes;
    adapt->nImages = sizeof(SpitfireImages) / sizeof(SpitfireImages[0]);
    adapt->pImages = SpitfireImages;
    adapt->StopVideo = SpitfireStopVideo;
    adapt->SetPortAttribute = SpitfireSetPortAttribute;
    adapt->GetPortAttribute = SpitfireGetPortAttribute;
    adapt->QueryBestSize = SpitfireQueryBestSize;
    adapt->PutImage = SpitfirePutImage;
    adapt->ReputImage = SpitfireReputImage;
    adapt->QueryImageAttributes = SpitfireQueryImageAttributes;

    /* A color seldom drawn: palette entry 0xfd, or a blue just off pure */
    if (pScrn->depth == 8)
        pPriv->colorKey = 0xfd;
    else
        pPriv->colorKey = (1 << pScrn->offset.red) | (1 << pScrn->offset.green) |
                          (((pScrn->mask.blue >> pScrn->offset.blue) - 1) << pScrn->offset.blue);
    pPriv->autopaint = TRUE;
    REGION_NULL(pScreen, &pPriv->clip);

    xvColorKey = MAKE_ATOM("XV_COLORKEY");
    xvAutopaint = MAKE_ATOM("XV_AUTOPAINT_COLORKEY");

    pdrv->VideoPriv = pPriv;
    return adapt;
}

void SpitfireInitVideo(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    XF86VideoAdaptorPtr *adaptors, *newAdaptors = NULL;
    XF86VideoAdaptorPtr overlay;
    int num = xf86XVListGenericAdaptors(pScrn, &adaptors);

    if ((overlay = SpitfireSetupImageVideo(pScreen))) {
        if ((newAdaptors = malloc((num + 1) * sizeof(XF86VideoAdaptorPtr)))) {
            if (num)
                memcpy(newAdaptors, adaptors, num * sizeof(XF86VideoAdaptorPtr));
            newAdaptors[num++] = overlay;
            adaptors = newAdaptors;
        }
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Xv: video overlay adaptor, color key 0x%06x\n",
                   (unsigned)((SpitfirePortPrivPtr)DEVPTR(pScrn)->VideoPriv)->colorKey);
    }
    if (num)
        xf86XVScreenInit(pScreen, adaptors, num);
    free(newAdaptors);
}

/*
 * After a mode set, which writes OAK 0x06 with the windows off: window 0
 * off too. The Xv layer puts the image again after the mode set
 * (ReputImage), with the window fitted to the new mode; a RandR size
 * change stops it first (the root clip is emptied), and then the client
 * puts the next one.
 */
void SpitfireVideoAfterMode(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    SpitfirePortPrivPtr pPriv = pdrv->VideoPriv;

    if (!pPriv || !pPriv->on)
        return;
    MMIO_OUT8(SPITFIRE_MMIO, SPITFIRE_OVL_CTRL + 2, 0);
    pPriv->on = FALSE;
}

/* Before the server leaves the card, or closes the screen (before EXA) */
void SpitfireVideoOff(ScrnInfoPtr pScrn, Bool close)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    SpitfirePortPrivPtr pPriv = pdrv->VideoPriv;

    if (!pPriv)
        return;
    SpitfireOverlayOff(pScrn, pPriv);
    if (close) {
        SpitfireFreeBuffers(pScrn, pPriv);
        free(pPriv->image);
        pPriv->image = NULL;
        pPriv->imageSize = 0;
        REGION_UNINIT(pScrn->pScreen, &pPriv->clip);
        REGION_NULL(pScrn->pScreen, &pPriv->clip);
        pdrv->VideoPriv = NULL;
    }
}
