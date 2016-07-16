#ifndef SPITFIRE_H
#define SPITFIRE_H

#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef XSERVER_LIBPCIACCESS
#include <pciaccess.h>
#define VENDOR_ID(p)      (p)->vendor_id
#define DEVICE_ID(p)      (p)->device_id
#define SUBSYS_ID(p)      (p)->subdevice_id
#define CHIP_REVISION(p)  (p)->revision
#else
#define VENDOR_ID(p)      (p)->vendor
#define DEVICE_ID(p)      (p)->chipType
#define SUBSYS_ID(p)      (p)->subsysCard
#define CHIP_REVISION(p)  (p)->chipRev
#endif

#include "compiler.h"
#include "vgaHW.h"
#include "xf86.h"
#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#endif
#include "xf86Pci.h"
#include "xf86PciInfo.h"
#include "xf86_OSproc.h"
#include "xf86Cursor.h"
#include "mipointer.h"
#include "micmap.h"
#include "fb.h"
#include "fboverlay.h"
#include "xf86cmap.h"
#include "vbe.h"
#ifdef HAVE_XAA_H
#include "xaa.h"
#endif
#include "exa.h"
#include "xf86xv.h"

/* Description of a VESA mode for this video card */
typedef struct _OAKVMODEENTRY {
   unsigned short Width;
   unsigned short Height;
   unsigned short VesaMode;
   unsigned char RefreshCount;
   unsigned char * RefreshRate;
} SpitfireModeEntry, *SpitfireModeEntryPtr;

/* Table of known VESA modes for video card */
typedef struct _OAKVMODETABLE {
   unsigned short NumModes;
   SpitfireModeEntry Modes[1];
} SpitfireModeTableRec, *SpitfireModeTablePtr;

/* Extra register information to be saved and restored on mode switch */
typedef struct {
    unsigned int mode, refresh;

    /* All of these registers are programmed via port 3deh */
	unsigned char OR03, OR04, OR06, OR0F;
	unsigned char OR10, OR14;
	unsigned char OR20, OR21, OR22, OR25, OR29;
	unsigned char OR30, OR31, OR32, OR33, OR38;

    /* Programmed via MMIO */
	unsigned char MM0A;

    /* The following registers are programmed via the port at pdrv->extIOBase
       plus SPITFIRE_EX_INDEX/SPITFIRE_EX_DATA */
	unsigned char EX0C, EX0D, EX0E, EX0F; /* Clock selection */
	unsigned char EX30, EX31; /* Hicolor/Truecolor and DAC width */
} SpitfireRegRec, *SpitfireRegPtr;

#include "compat-api.h"

#define SPITFIRE_INDEX 0x3de
#define SPITFIRE_DATA  0x3df
#define SPITFIRE_EX_INDEX 0xe4
#define SPITFIRE_EX_DATA  0xe5

#define OTI_OUTB(a, i) outw(SPITFIRE_INDEX, ((i) & 0xFF) | ((unsigned int)(a) << 8))

#define EX_INB(a, i) \
do  { \
	outb(pdrv->extIOBase + SPITFIRE_EX_INDEX, (i)); \
	a = inb(pdrv->extIOBase + SPITFIRE_EX_DATA); \
} while (0)

#define EX_OUTB(a, i) \
do { \
	outb(pdrv->extIOBase + SPITFIRE_EX_INDEX, (i)); \
	outb(pdrv->extIOBase + SPITFIRE_EX_DATA, (a)); \
} while (0)

#define SPITFIRE_STATUS             0x02
#define SPITFIRE_CLOCKSEL           0x06
#define SPITFIRE_DPMS               0x0f
#define SPITFIRE_VIDMEM_MAP         0x14
#define SPITFIRE_MEM_MAP_ENABLE     0x15
#define SPITFIRE_DISPLAY_ADDR_HIGH  0x31 

#define InI2CREG(a,reg)                                 \
do {                                                    \
	outb(SPITFIRE_INDEX, reg);                      \
	a = inb(SPITFIRE_DATA);                           \
} while (0)

#define OutI2CREG(a,reg)                        \
do {                                            \
	outb(SPITFIRE_INDEX, reg);				\
	outb(SPITFIRE_DATA, a);                   \
} while (0) 


/* PCI memory region that has been mapped for access */
struct spitfire_region {
#ifdef XSERVER_LIBPCIACCESS
    pciaddr_t       base;
    pciaddr_t       size;
#else
    unsigned long   base;
    unsigned long   size;
#endif
    void          * memory;
};

typedef struct _StatInfo {
    int     origMode;
    int     pageCnt;    
    pointer statBuf;
    int     realSeg;    
    int     realOff;
} StatInfoRec,*StatInfoPtr;

/* First level structure with all the driver information */
typedef struct _Spitfire {
    SpitfireRegRec		SavedReg;
    SpitfireRegRec		ModeReg;
    xf86CursorInfoPtr	CursorInfoRec;
    Bool		ModeStructInit;
    int			Bpp, Bpl;
    I2CBusPtr		I2C;
    unsigned char       I2CPort;

    int			videoRambytes;
    int			videoRamKbytes;
    int			MemOffScreen;
    int			endfb;

    /* These are physical addresses. */
    unsigned long	ShadowPhysical;

    /* These are linear addresses. */
    struct spitfire_region   MmioRegion;
    struct spitfire_region   FbRegion;

    unsigned char*	MapBase;
    unsigned int        MapOffset;
    unsigned char*	FBBase;
    unsigned char*	FBStart;
    CARD32 volatile *	ShadowVirtual;

    /* Here are all the Options */

    OptionInfoPtr	Options;
    Bool			IgnoreEDID;
    Bool			NoAccel;
    Bool			shadowFB;
    Bool			UseBIOS;
    int				rotate;
    Bool			ForceInit;

    CloseScreenProcPtr	CloseScreen;

#ifdef XSERVER_LIBPCIACCESS
    struct pci_device * PciInfo;
#else
    pciVideoPtr		PciInfo;
    PCITAG		PciTag;
#endif
    int			Chipset;
    int			ChipId;
    int			ChipRev;
    vbeInfoPtr		pVbe;
    int			EntityIndex;
    int			vgaIOBase;	/* 3b0 or 3d0 */
    int			extIOBase;	/* extended I/O ports from PCI */

    /* This is used to save/restore clock select, to implement clock probing */
    unsigned char	saveClock;
    unsigned char       saveClockReg[2];

    /* Support for shadowFB and rotation */
    unsigned char *	ShadowPtr;
    int				ShadowPitch;
    void			(*PointerMoved)(int index, int x, int y);

    /* support for EXA */
    ExaDriverPtr        EXADriverPtr;
    Bool		useEXA;

    /* Support for XAA acceleration */
#ifdef HAVE_XAA_H
    XAAInfoRecPtr	AccelInfoRec;
#endif
    unsigned int	SavedAccelCmd;

    SpitfireModeTablePtr	ModeTable;

    /* Support for DGA */
    int			numDGAModes;
    DGAModePtr  DGAModes;
    Bool		DGAactive;
    int			DGAViewportStatus;

    int  lDelta;
    /*
     * cxMemory is number of pixels across screen width
     * cyMemory is number of scanlines in available adapter memory.
     *
     * cxMemory * cyMemory is used to determine how much memory to
     * allocate to our heap manager.  So make sure that any space at the
     * end of video memory set aside at bInitializeHardware time is kept
     * out of the cyMemory calculation.
     */
    int cxMemory,cyMemory;
    
    StatInfoRec     StatInfo; /* save the SVGA state */

} SpitfireRec, *SpitfirePtr;

#define DEVPTR(p)	((SpitfirePtr)((p)->driverPrivate))
/* This macro assumes there is a variable "pdrv" in scope of type SpitfirePtr */
#define SPITFIRE_MMIO (pdrv->MapBase + pdrv->MapOffset)
 
/* Prototypes */

void SpitfireAdjustFrame(int scrnIndex, int y, int x, int flags);
Bool SpitfireSwitchMode(int scrnIndex, DisplayModePtr mode, int flags);

/* In spitfire_vbe.c */

void SpitfireSetTextMode( SpitfirePtr psav );
void SpitfireSetVESAMode( SpitfirePtr psav, int n);
void SpitfireFreeBIOSModeTable( SpitfirePtr psav, SpitfireModeTablePtr* ppTable );
SpitfireModeTablePtr SpitfireGetBIOSModeTable( SpitfirePtr psav, int iDepth );
ModeStatus SpitfireMatchBiosMode(ScrnInfoPtr pScrn,int width,int height,int refresh,
                              unsigned int *vesaMode,unsigned int *newRefresh);

unsigned short SpitfireGetBIOSModes( 
    SpitfirePtr psav,
    int iDepth,
    SpitfireModeEntryPtr oakModeTable );

/* In spitfire_shadow.c */

void SpitfirePointerMoved(int index, int x, int y);
void SpitfireRefreshArea(ScrnInfoPtr pScrn, int num, BoxPtr pbox);
void SpitfireRefreshArea8(ScrnInfoPtr pScrn, int num, BoxPtr pbox);
void SpitfireRefreshArea16(ScrnInfoPtr pScrn, int num, BoxPtr pbox);
void SpitfireRefreshArea24(ScrnInfoPtr pScrn, int num, BoxPtr pbox);
void SpitfireRefreshArea32(ScrnInfoPtr pScrn, int num, BoxPtr pbox);


#endif

