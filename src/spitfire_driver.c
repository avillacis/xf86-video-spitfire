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

#include "spitfire_driver.h"
#include "spitfire_accel.h"

/*#define TRACEON*/
/*#define DUMP_REGISTERS*/
/*#define ENABLE_DDC */

#ifdef TRACEON
#define TRACE(prms)     ErrorF prms
#else
#define TRACE(prms)
#endif

/* Some systems #define VGA for their own purposes */
#undef VGA

/* A few things all drivers should have */
#define SPITFIRE_NAME            "SPITFIRE"
#define SPITFIRE_DRIVER_NAME     "spitfire"
#define SPITFIRE_VERSION_NAME    PACKAGE_VERSION
#define SPITFIRE_VERSION_MAJOR   PACKAGE_VERSION_MAJOR
#define SPITFIRE_VERSION_MINOR   PACKAGE_VERSION_MINOR
#define SPITFIRE_PATCHLEVEL      PACKAGE_VERSION_PATCHLEVEL
#define SPITFIRE_VERSION_CURRENT ((SPITFIRE_VERSION_MAJOR << 24) | \
                             (SPITFIRE_VERSION_MINOR << 16) | SPITFIRE_PATCHLEVEL)

#define PCI_VENDOR_OAK			0x104E
/* Oak */
#define PCI_CHIP_OTI107			0x0107


/* Prototype section */
static const OptionInfoRec * SpitfireAvailableOptions(int chipid, int busid);
static void SpitfireIdentify(int flags);
#ifdef XSERVER_LIBPCIACCESS
static Bool SpitfirePciProbe(DriverPtr drv, int entity_num,
                           struct pci_device *dev, intptr_t match_data);
#else
static Bool SpitfireProbe(DriverPtr drv, int flags);
static int LookupChipID(PciChipsets* pset, int ChipID);
#endif
static Bool SpitfirePreInit(ScrnInfoPtr pScrn, int flags);

static Bool SpitfireMapMem(ScrnInfoPtr pScrn);
static void SpitfireUnmapMem(ScrnInfoPtr pScrn, int All);
static Bool SpitfireModeInit(ScrnInfoPtr pScrn, DisplayModePtr mode);
static void SpitfireEnableMMIO(ScrnInfoPtr pScrn);
static void SpitfireDisableMMIO(ScrnInfoPtr pScrn);
void SpitfireLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indicies,
		       LOCO *colors, VisualPtr pVisual);
static void SpitfireCalcClock(long freq, int min_m, int min_n1, int max_n1,
			   int min_n2, int max_n2, long freq_min,
			   long freq_max, unsigned int *mdiv,
			   unsigned int *ndiv, unsigned int *r);
static Bool SpitfireEnterVT(VT_FUNC_ARGS_DECL);
static void SpitfireLeaveVT(VT_FUNC_ARGS_DECL);
static void SpitfireSave(ScrnInfoPtr pScrn);
static void SpitfirePrintRegs(ScrnInfoPtr pScrn);
static void SpitfireWriteMode(ScrnInfoPtr pScrn, vgaRegPtr, SpitfireRegPtr, Bool);

static Bool SpitfireScreenInit(SCREEN_INIT_ARGS_DECL);
static int SpitfireInternalScreenInit(ScreenPtr pScreen);


static ModeStatus SpitfireValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode,
                                  Bool verbose, int flags);
static Bool SpitfireSaveScreen(ScreenPtr pScreen, int mode);
static Bool SpitfireCloseScreen(CLOSE_SCREEN_ARGS_DECL);

static Bool Spitfire107ClockSelect(ScrnInfoPtr pScrn, int no);

enum OAKCHIPTAGS {
    OAK_UNKNOWN = 0,
    OAK_64107,
    OAK_64111,
    OAK_LAST
};

/* Supported chipsets */
#ifndef PCI_CHIP_OTI111
#define PCI_CHIP_OTI111 0x0111
#endif
static SymTabRec SpitfireChips[] = {
#if 1
    { PCI_CHIP_OTI107,          "Oak Spitfire 64107" },
#endif    
    { PCI_CHIP_OTI111,          "Oak Spitfire 64111" },
    { -1,                        NULL }
};

static SymTabRec SpitfireChipsets[] = {
#if 1
    { OAK_64107,        "64107" },
#endif    
    { OAK_64111,        "64111" },
    { -1,                NULL }
};

#ifndef XSERVER_LIBPCIACCESS
/* This table maps a PCI device ID to a chipset family identifier. */

static PciChipsets SpitfirePciChipsets[] = {
    { OAK_64107,    PCI_CHIP_OTI107,    RES_SHARED_VGA },
    { OAK_64111,    PCI_CHIP_OTI111,    RES_SHARED_VGA },
    { -1,                -1,                        RES_UNDEFINED }
};
#endif


#ifdef XSERVER_LIBPCIACCESS
#define SPITFIRE_DEVICE_MATCH(d, i) \
    { PCI_VENDOR_OAK, (d), PCI_MATCH_ANY, PCI_MATCH_ANY, 0, 0, (i) }

static const struct pci_id_match spitfire_device_match[] = {
    SPITFIRE_DEVICE_MATCH(PCI_CHIP_OTI107,         OAK_64107),
    SPITFIRE_DEVICE_MATCH(PCI_CHIP_OTI111,         OAK_64111),

    { 0, 0, 0 },
};
#endif

typedef enum {
     OPTION_NOACCEL
    ,OPTION_ACCELMETHOD
    ,OPTION_SHADOW_FB
    ,OPTION_ROTATE
    ,OPTION_USEBIOS
    ,OPTION_INIT_BIOS
    ,OPTION_IGNORE_EDID
} SpitfireOpts;


static const OptionInfoRec SpitfireOptions[] =
{
    { OPTION_SHADOW_FB,     "ShadowFB",     OPTV_BOOLEAN,   {0}, FALSE },
    { OPTION_USEBIOS,       "UseBIOS",      OPTV_BOOLEAN,   {0}, FALSE },
    { OPTION_ROTATE,        "Rotate",       OPTV_ANYSTR,    {0}, FALSE },
    { OPTION_IGNORE_EDID,   "IgnoreEDID",   OPTV_BOOLEAN,   {0}, FALSE },
    { OPTION_NOACCEL,       "NoAccel",      OPTV_BOOLEAN,   {0}, FALSE },
    { OPTION_ACCELMETHOD,   "AccelMethod",  OPTV_STRING,    {0}, FALSE },
    { OPTION_INIT_BIOS,     "InitBIOS",     OPTV_BOOLEAN,    {0}, FALSE },

    { -1,                NULL,                OPTV_NONE,    {0}, FALSE }
};

/* This is the main driver record, which identifies the probe functions and
   the driver version.
 */
_X_EXPORT DriverRec SPITFIRE =
{
    SPITFIRE_VERSION_CURRENT,
    SPITFIRE_DRIVER_NAME,
    SpitfireIdentify,
#ifdef XSERVER_LIBPCIACCESS
    NULL,
#else
    SpitfireProbe,
#endif
    SpitfireAvailableOptions,
    NULL,
    0,
    NULL,

#ifdef XSERVER_LIBPCIACCESS
    spitfire_device_match,
    SpitfirePciProbe
#endif
};

#ifdef XFree86LOADER

static MODULESETUPPROTO(SpitfireSetup);

static XF86ModuleVersionInfo SpitfireVersRec = {
    "spitfire",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    SPITFIRE_VERSION_MAJOR, SPITFIRE_VERSION_MINOR, SPITFIRE_PATCHLEVEL,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData spitfireModuleData = {
    &SpitfireVersRec,
    SpitfireSetup,
    NULL
};

static pointer SpitfireSetup(pointer module, pointer opts, int *errmaj,
                           int *errmin)
{
    static Bool setupDone = FALSE;

    if (!setupDone) {
        setupDone = TRUE;
        xf86AddDriver(&SPITFIRE, module, 1);
        return (pointer) 1;
    } else {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }
}

#endif /* XFree86LOADER */


static Bool SpitfireGetRec(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate)
        return TRUE;

    pScrn->driverPrivate = xnfcalloc(sizeof(SpitfireRec), 1);
    return TRUE;
}


static void SpitfireFreeRec(ScrnInfoPtr pScrn)
{
    TRACE(( "SpitfireFreeRec(%p)\n", pScrn->driverPrivate ));
    if (!pScrn->driverPrivate)
        return;
    SpitfireUnmapMem(pScrn, 1);
    free(pScrn->driverPrivate);
    pScrn->driverPrivate = NULL;
}

/* Return available recognized options for Device section of xorg.conf */
static const OptionInfoRec * SpitfireAvailableOptions(int chipid, int busid)
{
    return SpitfireOptions;
}

/* Function to identify driver in the logfile */
static void SpitfireIdentify(int flags)
{
    xf86PrintChipsets(SPITFIRE_NAME, 
                      "driver (version " SPITFIRE_VERSION_NAME ") for Oak Spitfire chipsets",
                      SpitfireChips);
}

/* Probe functions */
#ifdef XSERVER_LIBPCIACCESS
static Bool SpitfirePciProbe(DriverPtr drv, int entity_num,
                           struct pci_device *dev, intptr_t match_data)
{
    ScrnInfoPtr pScrn;

    if ((match_data < OAK_64107) || (match_data > OAK_64111)) {
        return FALSE;
    }

    pScrn = xf86ConfigPciEntity(NULL, 0, entity_num, NULL,
            RES_SHARED_VGA, NULL, NULL, NULL, NULL);
    if (pScrn != NULL) {
        EntityInfoPtr pEnt;
        SpitfirePtr pdrv;


        pScrn->driverVersion = SPITFIRE_VERSION_CURRENT;
        pScrn->driverName = SPITFIRE_DRIVER_NAME;
        pScrn->name = SPITFIRE_NAME;
        pScrn->Probe = NULL;
        pScrn->PreInit = SpitfirePreInit;
        pScrn->ScreenInit = SpitfireScreenInit;
        pScrn->SwitchMode = SpitfireSwitchMode;
        pScrn->AdjustFrame = SpitfireAdjustFrame;
        pScrn->EnterVT = SpitfireEnterVT;
        pScrn->LeaveVT = SpitfireLeaveVT;
        pScrn->FreeScreen = NULL;
        pScrn->ValidMode = SpitfireValidMode;

        if (!SpitfireGetRec(pScrn))
            return FALSE;

        pdrv = DEVPTR(pScrn);

        pdrv->PciInfo = dev;
        pdrv->Chipset = match_data;

        pEnt = xf86GetEntityInfo(entity_num);
    }

    return (pScrn != NULL);
}

#else

static Bool SpitfireProbe(DriverPtr drv, int flags)
{
    int i;
    GDevPtr *devSections = NULL;
    int *usedChips;
    int numDevSections;
    int numUsed;
    Bool foundScreen = FALSE;

    /* sanity checks */
    if ((numDevSections = xf86MatchDevice(SPITFIRE_DRIVER_NAME, &devSections)) <= 0)
                return FALSE;
    if (xf86GetPciVideoInfo() == NULL) {
        if (devSections) free(devSections);
        return FALSE;
    }

    numUsed = xf86MatchPciInstances(SPITFIRE_NAME, PCI_VENDOR_OAK,
                                    SpitfireChipsets, SpitfirePciChipsets,
                                    devSections, numDevSections, drv,
                                    &usedChips);
    if (devSections) free(devSections);
    devSections = NULL;
    if (numUsed <= 0) return FALSE;

    if (flags & PROBE_DETECT) {
        foundScreen = TRUE;
    } else {
        for (i=0; i<numUsed; i++) {
            EntityInfoPtr pEnt = xf86GetEntityInfo(usedChips[i]);;
            ScrnInfoPtr pScrn = xf86ConfigPciEntity(NULL, 0, usedChips[i],
                NULL, RES_SHARED_VGA, 
                NULL, NULL, NULL, NULL);

            if (pScrn != NULL) {
                SpitfirePtr pdrv;

                pScrn->driverVersion = SPITFIRE_VERSION_CURRENT;
                pScrn->driverName = SPITFIRE_DRIVER_NAME;
                pScrn->name = SPITFIRE_NAME;
                pScrn->Probe = SpitfireProbe;
                pScrn->PreInit = SpitfirePreInit;
                pScrn->ScreenInit = SpitfireScreenInit;
                pScrn->SwitchMode = SpitfireSwitchMode;
                pScrn->AdjustFrame = SpitfireAdjustFrame;
                pScrn->EnterVT = SpitfireEnterVT;
                pScrn->LeaveVT = SpitfireLeaveVT;
                pScrn->FreeScreen = NULL;
                pScrn->ValidMode = SpitfireValidMode;
                foundScreen = TRUE;

                if (!SpitfireGetRec(pScrn)) return FALSE;

                pdrv = DEVPTR(pScrn);

                pdrv->PciInfo = xf86GetPciInfoForEntity(pEnt->index);
                if (pEnt->device->chipset && *pEnt->device->chipset) {
                    pdrv->Chipset = xf86StringToToken(SpitfireChipsets,
                        pEnt->device->chipset);
                } else if (pEnt->device->chipID >= 0) {
                    pdrv->Chipset = LookupChipID(SpitfirePciChipsets,
                        pEnt->device->chipID);
                } else {
                    pdrv->Chipset = LookupChipID(SpitfirePciChipsets, 
                        pdrv->PciInfo->chipType);
                }
            }
            free(pEnt);
        }
    }

    free(usedChips);
    return foundScreen;
}

static int LookupChipID( PciChipsets* pset, int ChipID )
{
    /* Is there a function to do this for me? */
    while( pset->numChipset >= 0 )
    {
        if( pset->PCIid == ChipID )
            return pset->numChipset;
        pset++;
    }

    return -1;
}
#endif

#ifdef ENABLE_DDC

#define VerticalRetraceWait()           \
do {                                    \
        int vgaCRIndex = pdrv->vgaIOBase + 4; \
        int vgaCRReg = vgaCRIndex + 1; \
        int vgaSysCtlReg = pdrv->vgaIOBase + 0xA; \
\
	outb(vgaCRIndex, 0x17);     \
	if (inb(vgaCRReg) & 0x80) {  \
		int i = 0x10000;                \
		while ((inb(vgaSysCtlReg) & 0x08) == 0x08 && i--) ; \
		i = 0x10000;                                                  \
		while ((inb(vgaSysCtlReg) & 0x08) == 0x00 && i--) ; \
	} \
} while (0)

static unsigned int
SpitfireDDC1Read(ScrnInfoPtr pScrn)
{
    register unsigned char tmp;
    SpitfirePtr pdrv = DEVPTR(pScrn);

    VerticalRetraceWait();
    InI2CREG(tmp,pdrv->I2CPort);
    return ((unsigned int) (tmp & 0x20));
}

static Bool
SpitfireDDC1(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned char byte;
    xf86MonPtr pMon;

    InI2CREG(byte, pdrv->I2CPort);
    OutI2CREG(byte | 0x02, pdrv->I2CPort);

    pMon=xf86DoEDID_DDC1(XF86_SCRN_ARG(pScrn),vgaHWddc1SetSpeedWeak(),SpitfireDDC1Read);
    if (!pMon)
        return FALSE;
    
    xf86PrintEDID(pMon);
    
    if (!pdrv->IgnoreEDID)
        xf86SetDDCproperties(pScrn,pMon);

    OutI2CREG(byte, pdrv->I2CPort);

    return TRUE;
}

static void
SpitfireI2CPutBits(I2CBusPtr b, int clock,  int data)
{
    ScrnInfoPtr pScrn = (ScrnInfoPtr)(xf86Screens[b->scrnIndex]);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned char reg = 0x00;

    if(clock) reg |= 0x1;
    if(data)  reg |= 0x2;

    OutI2CREG(reg, pdrv->I2CPort);
}

static void
SpitfireI2CGetBits(I2CBusPtr b, int *clock, int *data)
{
    ScrnInfoPtr pScrn = (ScrnInfoPtr)(xf86Screens[b->scrnIndex]);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned char reg = 0x00;

    InI2CREG(reg, pdrv->I2CPort);

    *clock = reg & 0x10;
    *data = reg & 0x20;
}

Bool 
SpitfireI2CInit(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    I2CBusPtr I2CPtr;

    I2CPtr = xf86CreateI2CBusRec();
    if(!I2CPtr) return FALSE;

    pdrv->I2C = I2CPtr;

    I2CPtr->BusName    = "I2C bus";
    I2CPtr->scrnIndex  = pScrn->scrnIndex;
    I2CPtr->I2CPutBits = SpitfireI2CPutBits;
    I2CPtr->I2CGetBits = SpitfireI2CGetBits;

    if (!xf86I2CBusInit(I2CPtr))
	return FALSE;

    return TRUE;
}

static void SpitfireDoDDC(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    pointer ddc;

    /* Do the DDC dance. */
    ddc = xf86LoadSubModule(pScrn, "ddc");
    if (ddc) {
        pdrv->I2CPort = 0x0c;
        ErrorF("Trying reading EDID with DDC1...\n");
        if (!SpitfireDDC1(pScrn)) {
            /* DDC1 failed,switch to DDC2 */
            if (xf86LoadSubModule(pScrn, "i2c")) {
                ErrorF("Trying reading EDID with DDC2...\n");
                if (SpitfireI2CInit(pScrn)) {
                    unsigned char tmp;
                    xf86MonPtr pMon;
                    
                    InI2CREG(tmp, pdrv->I2CPort);
                    OutI2CREG(tmp | 0x03, pdrv->I2CPort);
                    pMon = xf86PrintEDID(xf86DoEDID_DDC2(XF86_SCRN_ARG(pScrn),pdrv->I2C));
                    if (!pdrv->IgnoreEDID) xf86SetDDCproperties(pScrn, pMon);
                    OutI2CREG(tmp, pdrv->I2CPort);
                }
            }
        }
    }
}
#endif

static void SpitfireProbeDDC(ScrnInfoPtr pScrn, int index)
{
    vbeInfoPtr pVbe;

    if (xf86LoadSubModule(pScrn, "vbe")) {
        pVbe = VBEInit(NULL, index);
        ConfiguredMonitor = vbeDoEDID(pVbe, NULL);
        vbeFree(pVbe);
    }
}

static unsigned int SpitfireProbeVRAM()
{
    unsigned char status;

    /* NOTE: this register is initialized by the Oak VGA BIOS */
    outb(SPITFIRE_INDEX, SPITFIRE_STATUS);
    status = inb(SPITFIRE_DATA);

    return 1 << (((status & 0x0E) >> 1) + 8);
}

/**
 * PreInit implementation for this driver. Here the xorg.conf options are 
 * parsed and stored, and the hardware is probed to check that it can comply
 * with the required configuration.
 *
 * Returns TRUE if hardware can be succesfully probed/preinitialized, 
 * FALSE otherwise.
 */
static Bool SpitfirePreInit(ScrnInfoPtr pScrn, int flags)
{
    EntityInfoPtr pEnt;
    SpitfirePtr pdrv;
    int i,j;
    vgaHWPtr hwp;
    const char *s = NULL;
    MessageType from = X_DEFAULT;
    ClockRangePtr clockRanges;

    TRACE(("SpitfirePreInit(%d)\n", flags));

    if (flags & PROBE_DETECT) {
        SpitfireProbeDDC( pScrn, xf86GetEntityInfo(pScrn->entityList[0])->index );
        return TRUE;
    }

    /* Load vgahw module and request functions, since they are needed here */
    if (!xf86LoadSubModule(pScrn, "vgahw")) return FALSE;

    if (!vgaHWGetHWRec(pScrn)) return FALSE;

    pScrn->monitor = pScrn->confScreen->monitor;

    /*
     * We support depths of 8, 15, 16 and 24.
     * We support bpp of 8, 16, 24, 32. But prefer 24 for depth 24.
     */
    /* TODO: add SupportConvert24to32 if driver could possibly copy 24-bit 
     * pixmap into 32-bit framebuffer, or SupportConvert32to24 for the reverse 
     * case */
    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb | Support24bppFb))
        return FALSE;
    else {
        int requiredBpp;
        int altBpp = 0;

        switch (pScrn->depth) {
        case 8:
        case 16:
            requiredBpp = pScrn->depth;
            break;
        case 15:
            requiredBpp = 16;
            break;
        case 24:
            requiredBpp = 24;
            altBpp = 32;
            break;
        default:
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Given depth (%d) is not supported by this driver\n",
                        pScrn->depth);
            return FALSE;
        }

        if( 
            (pScrn->bitsPerPixel != requiredBpp) &&
            (pScrn->bitsPerPixel != altBpp) 
        ) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Depth %d must specify %d bpp; %d was given\n",
                       pScrn->depth, requiredBpp, pScrn->bitsPerPixel );
            return FALSE;
        }
    }

    xf86PrintDepthBpp(pScrn);

    if (pScrn->depth > 8) {
        rgb zeros = {0, 0, 0};

        if (!xf86SetWeight(pScrn, zeros, zeros))
            return FALSE;
        else {
            /* TODO check weight returned is supported */
            ;
        }
    }

    if (!xf86SetDefaultVisual(pScrn, -1)) {
        return FALSE;
    } else {
        /* We don't currently support DirectColor at 16bpp */
        if (pScrn->bitsPerPixel == 16 && pScrn->defaultVisual != TrueColor) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Given default visual"
                       " (%s) is not supported at depth %d\n",
                       xf86GetVisualName(pScrn->defaultVisual), pScrn->depth);
            return FALSE;
        }
    }

    pScrn->progClock = TRUE;

    if (!SpitfireGetRec(pScrn))
        return FALSE;
    pdrv = DEVPTR(pScrn);

    /* Enable PCI device (for non-boot video device) */
#ifdef XSERVER_LIBPCIACCESS
#if HAVE_PCI_DEVICE_ENABLE
    pci_device_enable(pdrv->PciInfo);
#endif
#endif

    hwp = VGAHWPTR(pScrn);
    vgaHWSetStdFuncs(hwp);
    vgaHWGetIOBase(hwp);
    pdrv->vgaIOBase = hwp->IOBase;

    xf86CollectOptions(pScrn, NULL);

    if (pScrn->depth == 8)
        pScrn->rgbBits = 8;

    if (!(pdrv->Options = calloc(1, sizeof(SpitfireOptions))))
        return FALSE;
    memcpy(pdrv->Options, SpitfireOptions, sizeof(SpitfireOptions));
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, pdrv->Options);

    xf86GetOptValBool(pdrv->Options, OPTION_IGNORE_EDID, &pdrv->IgnoreEDID);

    xf86GetOptValBool( pdrv->Options, OPTION_SHADOW_FB, &pdrv->shadowFB );
    if (pdrv->shadowFB) {
        xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "Option: shadow FB enabled\n");
    }

    if ((s = xf86GetOptValString(pdrv->Options, OPTION_ROTATE))) {
        if(!xf86NameCmp(s, "CW")) {
            /* accel is disabled below for shadowFB */
             /* RandR is disabled when the Rotate option is used (does
              * not work well together and scrambles the screen) */

            pdrv->shadowFB = TRUE;
            pdrv->rotate = 1;
            xf86DisableRandR();
            xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, 
                       "Rotating screen clockwise"
                       "- acceleration and RandR disabled\n");
        } else if(!xf86NameCmp(s, "CCW")) {
            pdrv->shadowFB = TRUE;
            pdrv->rotate = -1;
            xf86DisableRandR();
            xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
                   "Rotating screen counter clockwise"
                   " - acceleration and RandR disabled\n");

        } else {
            xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "\"%s\" is not a valid"
                       "value for Option \"Rotate\"\n", s);
            xf86DrvMsg(pScrn->scrnIndex, X_INFO, 
                       "Valid options are \"CW\" or \"CCW\"\n");
        }
    }

    if (xf86GetOptValBool(pdrv->Options, OPTION_NOACCEL, &pdrv->NoAccel))
        xf86DrvMsg( pScrn->scrnIndex, X_CONFIG,
                    "Option: NoAccel - Acceleration Disabled\n");

    if (pdrv->shadowFB && !pdrv->NoAccel) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "HW acceleration not supported with \"shadowFB\".\n");
        pdrv->NoAccel = TRUE;
    }

    if(!pdrv->NoAccel) {
        from = X_DEFAULT;
        char *strptr;
#ifdef HAVE_XAA_H
        if((strptr = (char *)xf86GetOptValString(pdrv->Options, OPTION_ACCELMETHOD))) {
            if(!xf86NameCmp(strptr,"XAA")) {
                from = X_CONFIG;
                pdrv->useEXA = FALSE;
            } else if(!xf86NameCmp(strptr,"EXA")) {
               from = X_CONFIG;
               pdrv->useEXA = TRUE;
            }
       }
#else
       pdrv->useEXA = TRUE;
#endif
       xf86DrvMsg(pScrn->scrnIndex, from, "Using %s acceleration architecture\n",
                pdrv->useEXA ? "EXA" : "XAA");
    }

    from = X_DEFAULT;
    pdrv->UseBIOS = FALSE;
    if (xf86GetOptValBool(pdrv->Options, OPTION_USEBIOS, &pdrv->UseBIOS) )
        from = X_CONFIG;
    xf86DrvMsg(pScrn->scrnIndex, from, "%ssing video BIOS to set modes\n",
        pdrv->UseBIOS ? "U" : "Not u" );

    from = X_DEFAULT;
    pdrv->InitBIOS = TRUE;
    if (!pdrv->UseBIOS) {
        if (xf86GetOptValBool(pdrv->Options, OPTION_INIT_BIOS, &pdrv->InitBIOS) )
            from = X_CONFIG;
    }
    xf86DrvMsg(pScrn->scrnIndex, from, "%snitializing video card using video BIOS\n",
        pdrv->InitBIOS ? "I" : "Not i" );

    if (pScrn->numEntities > 1) {
        SpitfireFreeRec(pScrn);
        return FALSE;
    }

    pEnt = xf86GetEntityInfo(pScrn->entityList[0]);
#ifndef XSERVER_LIBPCIACCESS
    if (pEnt->resources) {
        free(pEnt);
        SpitfireFreeRec(pScrn);
        return FALSE;
    }
#endif
    pdrv->EntityIndex = pEnt->index;

    pdrv->pVbe = NULL;
    if (pdrv->InitBIOS) {
        if (xf86LoadSubModule(pScrn, "vbe")) {
            pdrv->pVbe = VBEInit(NULL, pEnt->index);
        }
    }

#ifndef XSERVER_LIBPCIACCESS
    xf86RegisterResources(pEnt->index, NULL, /*ResNone*/ ResExclusive);
/*
    xf86SetOperatingState(resVgaIo, pEnt->index, ResUnusedOpr);
*/
    xf86SetOperatingState(resVgaMem, pEnt->index, ResDisableOpr);
#endif
    from = X_DEFAULT;
    if (pEnt->device->chipset && *pEnt->device->chipset) {
        pScrn->chipset = pEnt->device->chipset;
        pdrv->ChipId = pEnt->device->chipID;
        from = X_CONFIG;
    } else if (pEnt->device->chipID >= 0) {
        pdrv->ChipId = pEnt->device->chipID;
        pScrn->chipset = (char *)xf86TokenToString(SpitfireChipsets,
                                                   pdrv->Chipset);
        from = X_CONFIG;
        xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "ChipID override: 0x%04X\n",
                   pEnt->device->chipID);
    } else {
        from = X_PROBED;
        pdrv->ChipId = DEVICE_ID(pdrv->PciInfo);
        pScrn->chipset = (char *)xf86TokenToString(SpitfireChipsets,
                                                   pdrv->Chipset);
    }

    xf86DrvMsg(pScrn->scrnIndex, X_PROBED, "Chip: id %04x, \"%s\"\n",
               pdrv->ChipId, xf86TokenToString( SpitfireChips, pdrv->ChipId ) );

    if (pEnt->device->chipRev >= 0) {
        pdrv->ChipRev = pEnt->device->chipRev;
        xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "ChipRev override: %d\n",
                   pdrv->ChipRev);
    } else
        pdrv->ChipRev = CHIP_REVISION(pdrv->PciInfo);

    xf86DrvMsg(pScrn->scrnIndex, from, "Engine: \"%s\"\n", pScrn->chipset);

    if (pEnt->device->videoRam != 0)
        pScrn->videoRam = pEnt->device->videoRam;

    free(pEnt);

#ifndef XSERVER_LIBPCIACCESS
    pdrv->PciTag = pciTag(pdrv->PciInfo->bus, pdrv->PciInfo->device,
                          pdrv->PciInfo->func);
#endif

    /* Add more options here. */

    pdrv               = DEVPTR(pScrn);

    if (!SpitfireMapMem(pScrn)) {
        SpitfireFreeRec(pScrn);
        if (pdrv->pVbe) vbeFree(pdrv->pVbe);
        pdrv->pVbe = NULL;
        return FALSE;
    }

    {
        Gamma zeros = {0.0, 0.0, 0.0};

        if (!xf86SetGamma(pScrn, zeros)) {
            if (pdrv->pVbe) vbeFree(pdrv->pVbe);
            pdrv->pVbe = NULL;
            SpitfireFreeRec(pScrn);
            return FALSE;
        }
    }


    /* Measure discrete clocks */
    if (pdrv->Chipset == OAK_64107) {
        pScrn->numClocks = 4;
        xf86GetClocks(pScrn, pScrn->numClocks, Spitfire107ClockSelect,
                              vgaHWProtectWeak(),
                              vgaHWBlankScreenWeak(),
                          pdrv->vgaIOBase + 0x0A, 0x08, 1, 28322);
        from = X_PROBED;
        xf86ShowClocks(pScrn, from);
        for (i = 0; i < pScrn->numClocks; i++) {
            ErrorF("clock[%d] = %d\n", i, pScrn->clock[i]);
        }
    } else if (pdrv->Chipset == OAK_64111) {
        pScrn->numClocks = 0;
        pScrn->progClock = TRUE;
    }

    /* Next go on to detect amount of installed ram */
    if (!pScrn->videoRam) {
        pScrn->videoRam = SpitfireProbeVRAM();
        pdrv->videoRambytes = pScrn->videoRam * 1024;

        xf86DrvMsg(pScrn->scrnIndex, X_PROBED, 
                "probed videoram:  %dk\n",
                pScrn->videoRam);
    } else {
        pdrv->videoRambytes = pScrn->videoRam * 1024;

        xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
               "videoram =  %dk\n",
                pScrn->videoRam);
    }

    /* Fallback: get video RAM from VBE */
    if( !pScrn->videoRam && pdrv->pVbe )
    {
        /* If VBE is available, ask it about onboard memory. */

        VbeInfoBlock* vib;

        vib = VBEGetVBEInfo( pdrv->pVbe );
        pScrn->videoRam = vib->TotalMemory * 64;
        VBEFreeVBEInfo( vib );

        /* VBE often cuts 64k off of the RAM total. */

        if( pScrn->videoRam & 64 )
            pScrn->videoRam += 64;

        pdrv->videoRambytes = pScrn->videoRam * 1024;
    }
    pdrv->endfb = pdrv->videoRambytes;
#ifdef ENABLE_DDC
    SpitfireDoDDC(pScrn);
#endif
    pScrn->maxHValue = 2048 << 3;        /* 11 bits of h_total 8-pixel units */
    pScrn->maxVValue = 2048;                /* 11 bits of v_total */
    pScrn->virtualX = pScrn->display->virtualX;
    pScrn->virtualY = pScrn->display->virtualY;

    if ( pdrv->UseBIOS )
    {
        /* Go probe the BIOS for all the modes and refreshes at this depth. */

        if( pdrv->ModeTable )
        {
            SpitfireFreeBIOSModeTable( pdrv, &pdrv->ModeTable );
        }

        pdrv->ModeTable = SpitfireGetBIOSModeTable( pdrv,  pScrn->depth);

        if( !pdrv->ModeTable || !pdrv->ModeTable->NumModes ) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
                       "Failed to fetch any BIOS modes.  Disabling BIOS.\n");
            pdrv->UseBIOS = FALSE;
        }
        else
        {
            SpitfireModeEntryPtr pmt;

            xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
                       "Found %d modes at this depth:\n",
                       pdrv->ModeTable->NumModes);

            for(
                i = 0, pmt = pdrv->ModeTable->Modes; 
                i < pdrv->ModeTable->NumModes; 
                i++, pmt++ )
            {
                int j;
                ErrorF( "    [%03x] %d x %d", 
                        pmt->VesaMode, pmt->Width, pmt->Height );
                for( j = 0; j < pmt->RefreshCount; j++ )
                {
                    ErrorF( ", %dHz", pmt->RefreshRate[j] );
                }
                ErrorF( "\n");
            }
        }
    }

    /* Prepare clock range information */
    clockRanges = xnfalloc(sizeof(ClockRange));
    clockRanges->next = NULL;
    clockRanges->minClock = 25175; /* VGA minimum */
    clockRanges->maxClock = 28322; /* VGA maximum */
    clockRanges->clockIndex = -1;
    clockRanges->interlaceAllowed = TRUE;
    clockRanges->doubleScanAllowed = TRUE;
    clockRanges->ClockDivFactor = 1.0;
    clockRanges->ClockMulFactor = 1.0;

    /* Look minimum and maximum discrete clock from previous probing. */
    clockRanges->minClock = 12210;
    clockRanges->maxClock = 135000;
        
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        "Using minClock = %d MHz maxClock = %d MHz\n",
        clockRanges->minClock / 1000, clockRanges->maxClock / 1000);

    /* Validate the screen modes. */
    i = xf86ValidateModes(pScrn, pScrn->monitor->Modes,
                          pScrn->display->modes, clockRanges, NULL, 
                          256, 2048, 16,
                          128, 2048, 
                          pScrn->virtualX, pScrn->virtualY,
                          pdrv->videoRambytes, 
                          LOOKUP_BEST_REFRESH);

    if (i == -1) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "xf86ValidateModes failure\n");
        SpitfireFreeRec(pScrn);
        if (pdrv->pVbe) vbeFree(pdrv->pVbe);
        pdrv->pVbe = NULL;
        return FALSE;
    }

    xf86PruneDriverModes(pScrn);

    if (i == 0 || pScrn->modes == NULL) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
        SpitfireFreeRec(pScrn);
        if (pdrv->pVbe) vbeFree(pdrv->pVbe);
        pdrv->pVbe = NULL;
        return FALSE;
    }

    xf86SetCrtcForModes(pScrn, INTERLACE_HALVE_V);
    pScrn->currentMode = pScrn->modes;
    xf86PrintModes(pScrn);
    xf86SetDpi(pScrn, 0, 0);

    if (xf86LoadSubModule(pScrn, "fb") == NULL) {
        SpitfireFreeRec(pScrn);
        if (pdrv->pVbe) vbeFree(pdrv->pVbe);
        pdrv->pVbe = NULL;
        return FALSE;
    }

    if( !pdrv->NoAccel ) {

        char *modName = NULL;

        if (pdrv->useEXA) {
            modName = "exa";
            XF86ModReqInfo req;
            int errmaj, errmin;
            memset(&req, 0, sizeof(req));
            req.majorversion = 2;
            req.minorversion = 0;
            
            if( !LoadSubModule(pScrn->module, modName, 
                NULL, NULL, NULL, &req, &errmaj, &errmin) ) {
                LoaderErrorMsg(NULL, modName, errmaj, errmin);
                    SpitfireFreeRec(pScrn);
                    if (pdrv->pVbe) vbeFree(pdrv->pVbe);
                    pdrv->pVbe = NULL;
                    return FALSE;
            }
        } else {
            modName = "xaa";
            if( !xf86LoadSubModule(pScrn, modName) ) {
                    SpitfireFreeRec(pScrn);
                    if (pdrv->pVbe) vbeFree(pdrv->pVbe);
                    pdrv->pVbe = NULL;
                    return FALSE;
            } 
        }
    }

    if (pdrv->shadowFB) {
        if (!xf86LoadSubModule(pScrn, "shadowfb")) {
            SpitfireFreeRec(pScrn);
            if (pdrv->pVbe) vbeFree(pdrv->pVbe);
            pdrv->pVbe = NULL;
            return FALSE;
        }
    }
    if (pdrv->pVbe) vbeFree(pdrv->pVbe);

    pdrv->pVbe = NULL;

    return TRUE;
}

static int SpitfireGetRefresh(DisplayModePtr mode)
{
    int refresh = (mode->Clock * 1000) / (mode->HTotal * mode->VTotal);
    if (mode->Flags & V_INTERLACE)
        refresh *= 2.0;
    if (mode->Flags & V_DBLSCAN)
        refresh /= 2.0;
    if (mode->VScan > 1)
        refresh /= mode->VScan;
    return refresh;
}

static ModeStatus SpitfireValidMode(SCRN_ARG_TYPE arg, DisplayModePtr pMode,
                                  Bool verbose, int flags)
{
    SCRN_INFO_PTR(arg);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int refresh;

    TRACE(("SpitfireValidMode\n"));

    if (pdrv->UseBIOS) {
        refresh = SpitfireGetRefresh(pMode);
        return (SpitfireMatchBiosMode(pScrn,pMode->HDisplay,
                                   pMode->VDisplay,
                                   refresh,NULL,NULL));
    }

    return MODE_OK;
}

static Bool SpitfireModeInit(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    SpitfireRegPtr new = &pdrv->ModeReg;
    vgaRegPtr vganew = &hwp->ModeReg;

    TRACE(("SpitfireModeInit(%dx%d, %dkHz)\n", 
        mode->HDisplay, mode->VDisplay, mode->Clock));
#ifdef DUMP_REGISTERS
    SpitfirePrintRegs(pScrn);
#endif
    if (!vgaHWInit(pScrn, mode))
        return FALSE;

    new->mode = 0;

    if( pdrv->UseBIOS ) {
        int refresh;
        unsigned int newmode=0, newrefresh=0;

        refresh = SpitfireGetRefresh(mode);

        SpitfireMatchBiosMode(pScrn, mode->HDisplay, mode->VDisplay, refresh,
                            &newmode,&newrefresh);
        new->mode = newmode;
        new->refresh = newrefresh;
    }

    if( !new->mode ) {
        unsigned int pitch; /* display width, in bytes */
        
        /* 
         * Either BIOS use is disabled, or we failed to find a suitable
         * match.  Fall back to traditional register-crunching.
         */

        new->OR03 = 0x00;
        new->OR04 = 0x60;
        new->OR0F = 0x80;
        new->OR10 = 0x55;
        new->OR13 = 0xC0; /* Enable 16-bit memory (0x80) and I/O (0x40) access */
        new->OR14 = 0x0C | 0x1; /* Memory mapping, enable linear framebuffer */
        new->OR21 = 0x0C; /* Mode Select, common bits (see below) */
        new->OR22 = 0x60; /* Feature Select, enable write mode 4, read mode 4 */
        new->OR22 |= 0x04; /* Enable coprocessor command buffer */
        new->OR22 |= 0x08; /* Enable write cache */
        new->OR25 = 0x00; /* Read/write segment */
        new->OR26 = 0x09; /* 0x08 enable half clock 0x01 */
        new->OR28 = 0x0B; /* RAS-only refresh */
        new->OR29 = 0x02; /* Hardware window arbitration */
        new->MM0A = 0xa5;

        if (mode->Flags & V_INTERLACE ) {
            if (!mode->CrtcVAdjusted) {
                mode->CrtcVTotal >>= 1;
                mode->CrtcVDisplay >>= 1;
                mode->CrtcVSyncStart >>= 1;
                mode->CrtcVSyncEnd >>= 1;
                mode->CrtcVAdjusted = TRUE;
            }
        }

        /* Program selected clock index */
        //outb(SPITFIRE_INDEX, SPITFIRE_CLOCKSEL);
        if (pdrv->Chipset == OAK_64111) {
            unsigned int m, n, r;

            /* Legacy values, currently unused */
            new->EX0C = 0x4f;
            new->EX0D = 0x31;
        
            new->OR06 = 3; /* Choose clock set 3 at EX0E,EX0F */

            SpitfireCalcClock(mode->Clock, 8, 1, 63, 0, 3, 12210, 135000, &m, &n, &r);

            new->EX0E = m;
            new->EX0F = ((r << 6) & 0xc0) | (n & 0x3f);
        } else {
            new->OR06 = mode->ClockIndex;
            TRACE(("SpitfireModeInit: chosen clock index %d\n", mode->ClockIndex));
        }


        /* OTI CRT Overflow */
        new->OR30 = (((mode->CrtcVTotal - 2) & 0x400) >> 10) |  /* bit 10 of VTotal to bit 0 */
                    (((mode->CrtcVDisplay - 1) & 0x400) >> 9) | /* bit 10 of VDisplay to bit 1 */
                    (((mode->CrtcVSyncStart) & 0x400) >> 8) |   /* bit 10 of VSyncStart to bit 2 */
                    ((mode->Flags & V_INTERLACE) ? 0x80 : 0);   /* bit 7 enables interlace */

        new->OR31 = 0; /* CRT Start Address High */
        new->OR33 = 0;

        /* HSYNC/2 Start */
        new->OR32 = (mode->Flags & V_INTERLACE) ? (mode->CrtcVTotal >> 3) : 0;

        pitch = (pScrn->displayWidth * (pScrn->bitsPerPixel / 8)) >> 4;
        vganew->CRTC[0x13] = pitch & 0xff;

        /* For Mode Select, it has been observed that 8-bit modes have 0 in the 
         * high nibble, 15 and 16-bit modes have 4, and 24-bit modes have 2. 
         * In all cases the low nibble has 0x0C with bits 0 and 1 set to 0, 
         * which leaves clock undivided. */
        new->EX31 = 0x0c; /* Required for hi-color and true-color */
        switch (pScrn->depth) {
        case 8:
            new->OR21 |= 0x00;
            new->OR38 = 0x02;
            new->OR20 = 0xC4;
            new->EX30 = 0x00;
            new->EX31 = 0x2c; /* Enable 8-bit DAC */
            break;
        case 15:
            new->OR21 |= 0x40;
            new->OR38 = 0x05 | 0x20;
            new->OR20 = 0xC6;
            new->EX30 = 0x22;
            break;
        case 16:
            new->OR21 |= 0x40;
            new->OR38 = 0x05 | 0x20;
            new->OR20 = 0xC6;
            new->EX30 = 0x22;
            break;
        case 24:
        case 32:
            new->OR21 |= 0x20;
            new->OR20 = 0xCA;
            new->EX30 = 0x33;
            vganew->Attribute[0x10] &= ~0x40;
            if (pScrn->bitsPerPixel == 24) {
                new->OR38 = 0x87 | 0x40;
            } else if (pScrn->bitsPerPixel == 32) {
                new->OR38 = 0x88 | 0x60;
            }
            break;
        }
    }
    
    pScrn->vtSema = TRUE;

    /* do it! */
    SpitfireWriteMode(pScrn, vganew, new, TRUE);
    SpitfireAdjustFrame(ADJUST_FRAME_ARGS(pScrn, pScrn->frameX0, pScrn->frameY0));
#ifdef DUMP_REGISTERS
    SpitfirePrintRegs(pScrn);
#endif
    return TRUE;
}

static Bool SpitfireMapMem(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int err;

    TRACE(("SpitfireMapMem()\n"));

    /* On the 64111 (probably on the 64107 too) the MMIO is at PCI base 0,
       the framebuffer is at PCI base 1, and the PIO base is at PCI base 2. */

    /* TODO: currently the 64111 reports a MMIO region of 4096, which seems
       a bit excessive to map in its entirety, since just a fraction of the
       first 128 bytes are documented. */
#ifdef XSERVER_LIBPCIACCESS
    pdrv->MmioRegion.base = pdrv->PciInfo->regions[0].base_addr;
    pdrv->MmioRegion.size = pdrv->PciInfo->regions[0].size;
    pdrv->FbRegion.base = pdrv->PciInfo->regions[1].base_addr;
    pdrv->extIOBase = pdrv->PciInfo->regions[2].base_addr;
#else
    pdrv->MmioRegion.base = pdrv->PciInfo->memBase[0];
    pdrv->MmioRegion.size = pdrv->PciInfo->size[0];
    pdrv->FbRegion.base = pdrv->PciInfo->memBase[1];
    pdrv->extIOBase = pdrv->PciInfo->ioBase[2];
#endif
    pdrv->FbRegion.size = pdrv->videoRambytes; /* Might be 0 if videoram is not yet probed */

    if (pdrv->FbRegion.size != 0 && pdrv->FbRegion.memory == NULL) {
#ifdef XSERVER_LIBPCIACCESS
        err = pci_device_map_range(pdrv->PciInfo, pdrv->FbRegion.base,
                                   pdrv->FbRegion.size,
                                   (PCI_DEV_MAP_FLAG_WRITABLE
                                    | PCI_DEV_MAP_FLAG_WRITE_COMBINE),
                                   & pdrv->FbRegion.memory);
#else
        pdrv->FbRegion.memory = 
            xf86MapPciMem(pScrn->scrnIndex, VIDMEM_FRAMEBUFFER,
                          pdrv->PciTag, pdrv->FbRegion.base,
                          pdrv->FbRegion.size);
        err = (pdrv->FbRegion.memory == NULL) ? errno : 0;
#endif
        if (err) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Internal error: could not map framebuffer range (%d, %s).\n",
                       err, strerror(err));
            return FALSE;
        }

        pdrv->FBBase = pdrv->FbRegion.memory;
        pdrv->FBStart = pdrv->FBBase;
    }

    if (pdrv->MmioRegion.memory == NULL) {
        unsigned int pagemask;

#ifdef XSERVER_LIBPCIACCESS
        err = pci_device_map_range(pdrv->PciInfo, pdrv->MmioRegion.base,
                                   pdrv->MmioRegion.size,
                                   (PCI_DEV_MAP_FLAG_WRITABLE),
                                   & pdrv->MmioRegion.memory);
#else
        pdrv->MmioRegion.memory = 
            xf86MapPciMem(pScrn->scrnIndex, VIDMEM_MMIO,
                          pdrv->PciTag, pdrv->MmioRegion.base,
                          pdrv->MmioRegion.size);
        err = (pdrv->MmioRegion.memory == NULL) ? errno : 0;
#endif
        if (err) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Internal error: could not map MMIO range (%d, %s).\n",
                       err, strerror(err));
            return FALSE;
        } else {
            TRACE(("MMIO at 0x%08Lx size 0x%04Lx mapped at %p\n",
            pdrv->MmioRegion.base, pdrv->MmioRegion.size, pdrv->MmioRegion.memory));
	}

        pdrv->MapBase = pdrv->MmioRegion.memory;
        pdrv->MapOffset = 0;

        pagemask = getpagesize() - 1;
        /* Ugly hack to locate MMIO area within a 4096-byte mapping */
        if (pdrv->MmioRegion.base & pagemask) {
            pdrv->MapOffset = pdrv->MmioRegion.base & pagemask;
            ErrorF("MMIO assumed at offset 0x%03x from map\n", pdrv->MapOffset);
        }
    }

    pScrn->memPhysBase = pdrv->FbRegion.base;

    return TRUE;
}

static void SpitfireUnmapMem(ScrnInfoPtr pScrn, int All)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);

    TRACE(("SpitfireUnmapMem(%p,%p)\n", pdrv->MapBase, pdrv->FBBase));

    if (All && (pdrv->MmioRegion.memory != NULL)) {
#ifdef XSERVER_LIBPCIACCESS
        pci_device_unmap_range(pdrv->PciInfo,
                               pdrv->MmioRegion.memory,
                               pdrv->MmioRegion.size);
#else
        xf86UnMapVidMem(pScrn->scrnIndex, (pointer)pdrv->MapBase,
                        pdrv->MmioRegion.size);
#endif

        pdrv->MmioRegion.memory = NULL;
        pdrv->MapBase = 0;
        pdrv->MapOffset = 0;
    }

    if (pdrv->FbRegion.memory != NULL) {
#ifdef XSERVER_LIBPCIACCESS
        pci_device_unmap_range(pdrv->PciInfo,
                               pdrv->FbRegion.memory,
                               pdrv->FbRegion.size);
#else
        xf86UnMapVidMem(pScrn->scrnIndex, (pointer)pdrv->FbRegion.base,
                        pdrv->FbRegion.size);
#endif
    }

    pdrv->FbRegion.memory = NULL;
    pdrv->FBBase = 0;
    pdrv->FBStart = 0;
}

static void SpitfireEnableMMIO(ScrnInfoPtr pScrn)
{
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned short val;

    TRACE(("SpitfireEnableMMIO\n"));
    vgaHWSetStdFuncs(hwp);

    /* For the Oak Spitfire, MMIO is required to implement acceleration,
       even if the chipset lacks any duplicate of the standard VGA ports.
     */
    outb(SPITFIRE_INDEX, SPITFIRE_MEM_MAP_ENABLE);
    val = inb(SPITFIRE_DATA);
    /* 0x80 enables MMIO response, 0x40 enables graphics memory response */
    OTI_OUTB((val | 0x80 | 0x40), SPITFIRE_MEM_MAP_ENABLE);

    /* The Oak Spitfire requires a switch to enable the linear framebuffer.
       When enabling the linear framebuffer, the legacy VGA range at 0xA000
       will become unresponsive until the linear framebuffer is disabled 
     */
    outb(SPITFIRE_INDEX, SPITFIRE_VIDMEM_MAP);
    val = inb(SPITFIRE_DATA);
    /* 0x01 enables linear framebuffer, 
       0x02 disables DMA to framebuffer (untested),
       0x0C should select size of aperture mapping (untested) */
    OTI_OUTB((val | 0x01), SPITFIRE_VIDMEM_MAP);
}

static void SpitfireDisableMMIO(ScrnInfoPtr pScrn)
{
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned short val;

    TRACE(("SpitfireDisableMMIO\n"));

    /* Disable linear framebuffer, to restore legacy VGA range */
    outb(SPITFIRE_INDEX, SPITFIRE_VIDMEM_MAP);
    val = inb(SPITFIRE_DATA);
    OTI_OUTB((val & 0x0e), SPITFIRE_VIDMEM_MAP);

    outb(SPITFIRE_INDEX, SPITFIRE_MEM_MAP_ENABLE);
    val = inb(SPITFIRE_DATA);
    /* Just disable MMIO response, graphics response MUST remain enabled */
    OTI_OUTB((val & 0x7f), SPITFIRE_MEM_MAP_ENABLE);

    vgaHWSetStdFuncs(hwp);
}

void SpitfireLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indicies,
               LOCO *colors, VisualPtr pVisual)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int i, index;
    int updateKey = -1;
    int bitsPerDAC;

    TRACE(("SpitfireLoadPalette\n"));

    /* Query bits of palette via VESA */
    if (pdrv->UseBIOS && pdrv->pVbe) {
        bitsPerDAC = VBESetGetDACPaletteFormat(pdrv->pVbe, 0);
        if (bitsPerDAC == 6) {
            int r;
            r = VBESetGetDACPaletteFormat(pdrv->pVbe, 8);
            TRACE(("VBESetGetDACPaletteFormat(pVbe, 8) returned %d\n", r));
#ifdef DUMP_REGISTERS
            SpitfirePrintRegs(pScrn);
#endif
        } else if (bitsPerDAC == 8) {
            TRACE(("bitsPerDAC==8, 8-bit DAC already enabled.\n"));
        } else {
            TRACE(("bitsPerDAC==%d, possibly no support!\n", bitsPerDAC));
        }
    }

    /* NOTE: this assumes 8-bit DAC has been previously enabled */
    for (i=0; i<numColors; i++) {
        index = indicies[i];
        outb(0x3c8, index);
        outb(0x3c9, colors[index].red);
        outb(0x3c9, colors[index].green);
        outb(0x3c9, colors[index].blue);
        if (index == pScrn->colorKey) updateKey = index;
    }
}

#define BASE_FREQ			14.31818
static void SpitfireCalcClock(long freq, int min_m, int min_n1, int max_n1,
			   int min_n2, int max_n2, long freq_min,
			   long freq_max, unsigned int *mdiv,
			   unsigned int *ndiv, unsigned int *r)
{
    double ffreq, ffreq_min, ffreq_max;
    double div, diff, best_diff;
    unsigned int m;
    unsigned char n1, n2, best_n1=16, best_n2=0, best_m=0x47;

    ffreq = freq / 1000.0 / BASE_FREQ;
    ffreq_max = freq_max / 1000.0 / BASE_FREQ;
    ffreq_min = freq_min / 1000.0 / BASE_FREQ;

    if (ffreq < ffreq_min / (1 << max_n2)) {
	    ErrorF("invalid frequency %1.3f Mhz\n",
		   ffreq*BASE_FREQ);
	    ffreq = ffreq_min / (1 << max_n2);
    }
    if (ffreq > ffreq_max / (1 << min_n2)) {
	    ErrorF("invalid frequency %1.3f Mhz\n",
		   ffreq*BASE_FREQ);
	    ffreq = ffreq_max / (1 << min_n2);
    }

    /* work out suitable timings */

    best_diff = ffreq;

    for (n2=min_n2; n2<=max_n2; n2++) {
        for (n1=min_n1; n1<=max_n1; n1++) {
            m = (int)(ffreq * n1 * (1 << n2) + 0.5);
            if (m < min_m || m > 255) continue;
            div = (double)(m) / (double)(n1);
            if ((div >= ffreq_min) && (div <= ffreq_max)) {
                diff = ffreq - div / (1 << n2);
                if (diff < 0.0) diff = -diff;
                if (diff < best_diff) {
                    best_diff = diff;
                    best_m = m;
                    best_n1 = n1;
                    best_n2 = n2;
                }
            }
        }
    }

    *ndiv = best_n1;
    *r = best_n2;
    *mdiv = best_m;
}

static void SpitfireDPMS(ScrnInfoPtr pScrn, int mode, int flags)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int val;

    TRACE(("SpitfireDPMS(%d,%x)\n", mode, flags));

    outb(SPITFIRE_INDEX, SPITFIRE_DPMS);
    val = inb(SPITFIRE_DATA) & ~0x03;
    switch (mode) {
    case DPMSModeOn:
        val |= 0x00;
        break;
    case DPMSModeStandby:
        val |= 0x01;
        break;
    case DPMSModeSuspend:
        val |= 0x02;
        break;
    case DPMSModeOff:
        val |= 0x03;
        break;
    default:
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Invalid DPMS mode %d\n", mode);
        break;
    }
    OTI_OUTB(val, SPITFIRE_DPMS);
}

static Bool SpitfireScreenInit(SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    EntityInfoPtr pEnt;
    int ret;
    int colormapFlags;

    TRACE(("SpitfireScreenInit()\n"));

    pEnt = xf86GetEntityInfo(pScrn->entityList[0]); 

    if (!SpitfireMapMem(pScrn))
        return FALSE;

    SpitfireSave(pScrn);

    vgaHWBlankScreen(pScrn, TRUE);

    /* Set up mode NOW! */
    if (!SpitfireModeInit(pScrn, pScrn->currentMode))
        return FALSE;

    /* This disables legacy VGA memory range, should be done *after* setting mode */
    SpitfireEnableMMIO(pScrn);

    /* Reset the Visual list */
    miClearVisualTypes();

    /* Driver does not support anything other than TrueColor when bpp > 8 */
    if (!miSetVisualTypes(
        pScrn->depth, 
        (pScrn->bitsPerPixel > 8) ? TrueColorMask : miGetDefaultVisualMask(pScrn->depth), 
        pScrn->rgbBits, 
        pScrn->defaultVisual))
        return FALSE;

    if (!miSetPixmapDepths ()) return FALSE;
    if (!SpitfireInternalScreenInit(pScreen)) return FALSE;

    xf86SetBlackWhitePixels(pScreen);

    {
        VisualPtr visual;
        visual = pScreen->visuals + pScreen->numVisuals;
        while (--visual >= pScreen->visuals) {
            if ((visual->class | DynamicClass) == DirectColor
                && visual->nplanes > MAX_PSEUDO_DEPTH) {
                if (visual->nplanes == pScrn->depth) {
                    visual->offsetRed = pScrn->offset.red;
                    visual->offsetGreen = pScrn->offset.green;
                    visual->offsetBlue = pScrn->offset.blue;
                    visual->redMask = pScrn->mask.red;
                    visual->greenMask = pScrn->mask.green;
                    visual->blueMask = pScrn->mask.blue;
                } else if (visual->offsetRed > 8 
                       || visual->offsetGreen > 8
                       || visual->offsetBlue > 8) {
                    /*
                     * mi has set these wrong. fix it here -- we cannot use pScrn
                     * as this is set up for the default depth 8.
                     */
                    int tmp;
                    int c_s = 0;

                    tmp = visual->offsetBlue;
                    visual->offsetBlue = visual->offsetRed;
                    visual->offsetRed = tmp;
                    tmp = visual->blueMask;
                    visual->blueMask = visual->redMask;
                    visual->redMask = tmp;
                }
            }
        }
    }

    /* must be after RGB ordering fixed */
    fbPictureInit (pScreen, 0, 0);

    if( !pdrv->NoAccel ) {
        SpitfireInitAccel(pScreen);
    }

    /*miInitializeBackingStore(pScreen);*/
    xf86SetBackingStore(pScreen);

    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (pdrv->shadowFB) {
        RefreshAreaFuncPtr refreshArea = SpitfireRefreshArea;
      
        if(pdrv->rotate) {
            if (!pdrv->PointerMoved) {
                pdrv->PointerMoved = pScrn->PointerMoved;
                pScrn->PointerMoved = SpitfirePointerMoved;
            }

            switch(pScrn->bitsPerPixel) {
            case 8: refreshArea = SpitfireRefreshArea8;	break;
            case 16:refreshArea = SpitfireRefreshArea16;	break;
            case 24:refreshArea = SpitfireRefreshArea24;	break;
            case 32:refreshArea = SpitfireRefreshArea32;	break;
            }
        }
        ShadowFBInit(pScreen, refreshArea);
    }
    if (!miCreateDefColormap(pScreen)) return FALSE;
    colormapFlags =  CMAP_RELOAD_ON_MODE_SWITCH | CMAP_PALETTED_TRUECOLOR;

    if (!xf86HandleColormaps(pScreen, 256, pScrn->rgbBits, SpitfireLoadPalette, NULL,
         colormapFlags ))
    return FALSE;

    vgaHWBlankScreen(pScrn, FALSE);

    pdrv->CloseScreen = pScreen->CloseScreen;
    pScreen->SaveScreen = SpitfireSaveScreen;
    pScreen->CloseScreen = SpitfireCloseScreen;

    if (xf86DPMSInit(pScreen, SpitfireDPMS, 0) == FALSE)
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DPMS initialization failed\n");

    if (serverGeneration == 1)
        xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

    return TRUE;
}

static Bool SpitfireCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    vgaRegPtr vgaSavePtr = &hwp->SavedReg;
    SpitfireRegPtr SpitfireSavePtr = &pdrv->SavedReg;

    TRACE(("SpitfireCloseScreen\n"));

    if (pdrv->EXADriverPtr) {
        exaDriverFini(pScreen);
        pdrv->EXADriverPtr = NULL;
    }

#ifdef HAVE_XAA_H
    if( pdrv->AccelInfoRec ) {
        XAADestroyInfoRec( pdrv->AccelInfoRec );
        pdrv->AccelInfoRec = NULL;
    }
#endif

    if( pdrv->DGAModes ) {
        free( pdrv->DGAModes );
        pdrv->DGAModes = NULL;
        pdrv->numDGAModes = 0;
    }

    if (pScrn->vtSema) {
        SpitfireWriteMode(pScrn, vgaSavePtr, SpitfireSavePtr, FALSE);
        vgaHWLock(hwp);
        SpitfireUnmapMem(pScrn, 0);
    }

    if (pdrv->pVbe)
      vbeFree(pdrv->pVbe);
    pdrv->pVbe = NULL;

    pScrn->vtSema = FALSE;
    pScreen->CloseScreen = pdrv->CloseScreen;

    return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}


static int SpitfireInternalScreenInit(ScreenPtr pScreen)
{
    int ret = TRUE;
    ScrnInfoPtr pScrn;
    SpitfirePtr pdrv;
    int width, height, displayWidth;
    unsigned char *FBStart;

    TRACE(("SpitfireInternalScreenInit()\n"));

    pScrn = xf86ScreenToScrn(pScreen);
    pdrv = DEVPTR(pScrn);

    displayWidth = pScrn->displayWidth;

    if (pdrv->rotate) {
        height = pScrn->virtualX;
        width = pScrn->virtualY;
    } else {
        width = pScrn->virtualX;
        height = pScrn->virtualY;
    }
  
  
    if(pdrv->shadowFB) {
        pdrv->ShadowPitch = BitmapBytePad(pScrn->bitsPerPixel * width);
        pdrv->ShadowPtr = calloc(1, pdrv->ShadowPitch * height);
        displayWidth = pdrv->ShadowPitch / (pScrn->bitsPerPixel >> 3);
        FBStart = pdrv->ShadowPtr;
    } else {
        pdrv->ShadowPtr = NULL;
        FBStart = pdrv->FBStart;
    }

    ret = fbScreenInit(pScreen, FBStart, width, height,
                       pScrn->xDpi, pScrn->yDpi,
                       pScrn->displayWidth,
                       pScrn->bitsPerPixel);

    return ret;
}


static Bool SpitfireEnterVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);

    TRACE(("SpitfireEnterVT(%d)\n", flags));

    SpitfireSave(pScrn);
    if(SpitfireModeInit(pScrn, pScrn->currentMode)) {
        SpitfireEnableMMIO(pScrn);
        return TRUE;
    }
    return FALSE;
}

static void SpitfireLeaveVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    vgaRegPtr vgaSavePtr = &hwp->SavedReg;
    SpitfireRegPtr SpitfireSavePtr = &pdrv->SavedReg;

    TRACE(("SpitfireLeaveVT(%d)\n", flags));

    SpitfireWriteMode(pScrn, vgaSavePtr, SpitfireSavePtr, FALSE);
    SpitfireDisableMMIO(pScrn);
}

static void SpitfireSave(ScrnInfoPtr pScrn)
{
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    vgaRegPtr vgaSavePtr = &hwp->SavedReg;
    SpitfirePtr pdrv = DEVPTR(pScrn);
    SpitfireRegPtr save = &pdrv->SavedReg;

    TRACE(("SpitfireSave()\n"));

    if (xf86IsPrimaryPci(pdrv->PciInfo))
        vgaHWSave(pScrn, vgaSavePtr, VGA_SR_ALL);
    else
        vgaHWSave(pScrn, vgaSavePtr, VGA_SR_MODE);

    /* Save all relevant registers of port 0x3de */
    outb(SPITFIRE_INDEX, 0x03); save->OR03 = inb(SPITFIRE_DATA); /* OTI Test Register 1 */
    outb(SPITFIRE_INDEX, 0x04); save->OR04 = inb(SPITFIRE_DATA); /* OTI Test Register 2 */
    outb(SPITFIRE_INDEX, 0x06); save->OR06 = inb(SPITFIRE_DATA) & 3; /* Clock select */
    outb(SPITFIRE_INDEX, 0x0f); save->OR0F = inb(SPITFIRE_DATA);
    outb(SPITFIRE_INDEX, 0x10); save->OR10 = inb(SPITFIRE_DATA); /* Local bus control */
    outb(SPITFIRE_INDEX, 0x13); save->OR13 = inb(SPITFIRE_DATA); /* ISA bus control */
    outb(SPITFIRE_INDEX, 0x14); save->OR14 = inb(SPITFIRE_DATA); /* Memory mapping */
    outb(SPITFIRE_INDEX, 0x20); save->OR20 = inb(SPITFIRE_DATA); /* FIFO depth */
    outb(SPITFIRE_INDEX, 0x21); save->OR21 = inb(SPITFIRE_DATA); /* Mode select */
    outb(SPITFIRE_INDEX, 0x22); save->OR22 = inb(SPITFIRE_DATA); /* Feature select */
    outb(SPITFIRE_INDEX, 0x25); save->OR25 = inb(SPITFIRE_DATA); /* Extended common read/write segment */
    outb(SPITFIRE_INDEX, 0x26); save->OR26 = inb(SPITFIRE_DATA);
    outb(SPITFIRE_INDEX, 0x28); save->OR28 = inb(SPITFIRE_DATA);
    outb(SPITFIRE_INDEX, 0x29); save->OR29 = inb(SPITFIRE_DATA); /* Hardware window arbitration */
    outb(SPITFIRE_INDEX, 0x30); save->OR30 = inb(SPITFIRE_DATA); /* OTI CRT overflow */
    outb(SPITFIRE_INDEX, 0x31); save->OR31 = inb(SPITFIRE_DATA); /* CRT Start Address High */
    outb(SPITFIRE_INDEX, 0x32); save->OR32 = inb(SPITFIRE_DATA); /* HSYNC/2 Start */
    outb(SPITFIRE_INDEX, 0x33); save->OR33 = inb(SPITFIRE_DATA); /* CRT Address Compatibility */
    outb(SPITFIRE_INDEX, 0x38); save->OR38 = inb(SPITFIRE_DATA); /* Pixel Interface */

    save->MM0A = SPITFIRE_MMIO[0x0a];

    if (pdrv->Chipset == OAK_64111) {
        /* Save previous clock settings. The 64111 selects one of four clock settings 
         * via OR06, and the actual programmings are done in EX08/EX09 for clock 0,
         * EX0A/EX0B for clock 1, EX0C/EX0D for clock 2, and EX0E/EX0F for clock 3.
         * On POST, clock 0 through 2 have VGA legacy values, so we are free to 
         * manipulate clock 3. */
        EX_INB(save->EX0C, 0x0c);
        EX_INB(save->EX0D, 0x0d);
        EX_INB(save->EX0E, 0x0e);
        EX_INB(save->EX0F, 0x0f);

        /* Hicolor/Truecolor settings and 8-bit DAC state */
        EX_INB(save->EX30, 0x30);
        EX_INB(save->EX31, 0x31);
    }

    if (!pdrv->ModeStructInit) {
        vgaHWCopyReg(&hwp->ModeReg, vgaSavePtr);
        memcpy(&pdrv->ModeReg, save, sizeof(SpitfireRegRec));
        pdrv->ModeStructInit = TRUE;
    }
}

static void SpitfireWriteMode(ScrnInfoPtr pScrn, vgaRegPtr vgaSavePtr,
                            SpitfireRegPtr restore, Bool Entering)
{
    vgaHWPtr hwp = VGAHWPTR(pScrn);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    int vgaCRIndex, vgaCRReg, vgaIOBase;
    int i, temp;

    vgaIOBase = hwp->IOBase;
    vgaCRIndex = vgaIOBase + 4;
    vgaCRReg = vgaIOBase + 5;
    
    TRACE(("SpitfireWriteMode(%x)\n", restore->mode));

    /*
     * If we figured out a VESA mode number for this timing, just use
     * the VESA BIOS to do the switching, with a few additional tweaks.
     */
    if (pdrv->UseBIOS && restore->mode > 0x13)
    {
        /* Set up the mode.  Don't clear video RAM. */
        SpitfireSetVESAMode( pdrv, restore->mode | 0x8000);

        /* Restore the DAC. */
        vgaHWRestore(pScrn, vgaSavePtr, VGA_SR_CMAP);

        return;
    }

    vgaHWProtect(pScrn, TRUE);

    vgaSavePtr->MiscOutReg = (vgaSavePtr->MiscOutReg & 0xF3) | ((restore->OR06 &0x03) << 2);

    if (pdrv->Chipset == OAK_64111) {
        /* Program clock frequency */
        EX_OUTB(restore->EX0C, 0x0c);
        EX_OUTB(restore->EX0D, 0x0d);
        EX_OUTB(restore->EX0E, 0x0e);
        EX_OUTB(restore->EX0F, 0x0f);
    }

    OTI_OUTB(restore->OR10, 0x10); /* Local bus control */
    OTI_OUTB(restore->OR13, 0x13); /* ISA bus control */
    OTI_OUTB(restore->OR04, 0x04); /* OTI Test Register 2 */
    OTI_OUTB(restore->OR03, 0x03); /* OTI Test Register 1 */
    OTI_OUTB(restore->OR33, 0x33); /* CRT Address Compatibility */
    OTI_OUTB(restore->OR26, 0x26);
    OTI_OUTB(restore->OR28, 0x28);
    OTI_OUTB(restore->OR29, 0x29); /* Hardware window arbitration */
    OTI_OUTB(restore->OR20, 0x20); /* FIFO depth */
    OTI_OUTB(restore->OR30, 0x30); /* OTI CRT overflow */
    OTI_OUTB(restore->OR32, 0x32); /* HSYNC/2 Start */
    OTI_OUTB(restore->OR38, 0x38); /* Pixel Interface */
    OTI_OUTB(restore->OR22, 0x22); /* Feature select */
    OTI_OUTB(restore->OR14, 0x14); /* Memory mapping */
    OTI_OUTB(restore->OR25, 0x25); /* Extended common read/write segment */

    OTI_OUTB(restore->OR31, 0x31); /* CRT Start Address High */
    OTI_OUTB(restore->OR0F, 0x0f);

    /* According to documentation, a software reset must be made in order
       to update Mode Select */
    OTI_OUTB(restore->OR21, 0x21); /* Mode select */

    if (pdrv->Chipset == OAK_64111) {
        EX_OUTB(restore->EX30, 0x30);
        EX_OUTB(restore->EX31, 0x31);
    }

    /* Restore standard VGA registers BEFORE selecting a clock */
    vgaHWRestore(pScrn, vgaSavePtr, VGA_SR_ALL);

    OTI_OUTB(restore->OR06 | 0xe0, 0x06);
    OTI_OUTB(restore->OR06 | 0x60, 0x06);

    SPITFIRE_MMIO[0x0a] = restore->MM0A;

    if (Entering) {
        /* Need to do this on entering graphics mode. Otherwise, adjacent
         * horizontal pixels get mixed up */
        inb(0x3da);
        outb(0x3c0, 0x10 | 0x20);
        inb(0x3c1);
        outb(0x3c0, 0x11 | 0x20);
        inb(0x3c1);
    }

    vgaHWProtect(pScrn, FALSE);
}

static Bool SpitfireSaveScreen(ScreenPtr pScreen, int mode)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

    TRACE(("SpitfireSaveScreen(0x%x)\n", mode));

    return vgaHWSaveScreen(pScreen, mode);
}


/**
 * SpitfireAdjustFrame
 *
 * Adjust driver variables for a new framebuffer size.
 */
void SpitfireAdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int pixelStart;
    unsigned int byteStart;
    unsigned int progAddr = 0;

    pixelStart = (pScrn->displayWidth * y + x);
    byteStart = pixelStart * (pScrn->bitsPerPixel / 8);
    if (pScrn->bitsPerPixel == 24) {
        /* This fixup is required to avoid color changes due to pixel component misalignment */
        byteStart -= (byteStart % 24);
    }
    progAddr = byteStart >> 3;

    outw(SPITFIRE_INDEX, SPITFIRE_DISPLAY_ADDR_HIGH | ((progAddr & 0x00ff0000) >> 8)); /* (16-22) */
    outw(pdrv->vgaIOBase + 4, 0x0c | ((progAddr & 0xff00)));        /* Start address high (8-15) */
    outw(pdrv->vgaIOBase + 4, 0x0d | ((progAddr & 0x00ff) << 8));   /* Start address low  (0-7) */
}

/* Helper routine to implement clock probing */
static Bool Spitfire107ClockSelect(ScrnInfoPtr pScrn, int no)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned char val;

    switch (no) {
    case CLK_REG_SAVE:
        outb(SPITFIRE_INDEX, SPITFIRE_CLOCKSEL);
        pdrv->saveClock = inb(SPITFIRE_DATA) & 0x0f;
        break;
    case CLK_REG_RESTORE:
        outb(SPITFIRE_INDEX, SPITFIRE_CLOCKSEL);
        val = (inb(SPITFIRE_DATA) & 0xf0) | (pdrv->saveClock & 0x0f);
        OTI_OUTB(val, SPITFIRE_CLOCKSEL);
        break;
    default:
        outb(SPITFIRE_INDEX, SPITFIRE_CLOCKSEL);
        val = (inb(SPITFIRE_DATA) & 0xf0) | (no & 0x03);
        OTI_OUTB(val, SPITFIRE_CLOCKSEL);
        break;
    }
    return TRUE;
}

/* This function is used to debug, it prints out the contents of video regs */

static void SpitfirePrintRegs(ScrnInfoPtr pScrn)
{
    SpitfirePtr pdrv = DEVPTR(pScrn);
    unsigned int i, j;
    unsigned char save;
    int vgaCRIndex = pdrv->vgaIOBase + 4;
    int vgaCRReg = vgaCRIndex + 1;

    ErrorF( "\n\nATTR  x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    inb(0x3da);

    for( i = 0; i <= 0x1f; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nAT%xx ", i >> 4 );
        outb(0x3c0, i | 0x20);
        ErrorF( " %02x", inb(0x3c1) );
    }

    ErrorF( "\n\nSR    x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    for( i = 0; i < 0x10; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nSR%xx ", i >> 4 );
        outb(0x3c4, i);
        ErrorF( " %02x", inb(0x3c5) );
    }


    ErrorF( "\n\nGR    x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    for( i = 0; i < 0x10; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nGR%xx ", i >> 4 );
        outb(0x3ce, i);
        ErrorF( " %02x", inb(0x3cf) );
    }


    ErrorF( "\n\nCR    x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    for( i = 0; i <= 0x3f; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nCR%xx ", i >> 4 );
        outb(vgaCRIndex, i);
        ErrorF( " %02x", inb(vgaCRReg) );
    }


    ErrorF( "\n\nOAK   x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    for( i = 0; i <= 0xFF; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nOR%xx ", i >> 4 );
        outb(SPITFIRE_INDEX, i);
        ErrorF( " %02x", inb(SPITFIRE_DATA) );
    }

    ErrorF ("\n\nOTI011 x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF");
    for (i = 0; i < 64; i++) {
        if( !(i % 16) )
            ErrorF( "\nOTI%xx ", i >> 4 );
        outb(pdrv->extIOBase + SPITFIRE_EX_INDEX, i);
        ErrorF( " %02x", inb(pdrv->extIOBase + SPITFIRE_EX_DATA) );

    }

/*
	ErrorF( "\n\nPALETTE");
	outb(0x3c7, 0);
	for (i = 0; i <= 0xFF; i++) {
		ErrorF("\nPAL[0x%02x] %02x", i, inb(0x3c9));
		ErrorF(" %02x", inb(0x3c9));
		ErrorF(" %02x", inb(0x3c9));
	}
*/
    /* Need to enable MMIO in order to dump its contents */

    outb(SPITFIRE_INDEX, SPITFIRE_MEM_MAP_ENABLE);
    save = inb(SPITFIRE_DATA);
    outb(SPITFIRE_INDEX, SPITFIRE_MEM_MAP_ENABLE);
    outb(SPITFIRE_DATA, save | 0x80);

    ErrorF( "\n\nMEM    x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF" );

    for( i = 0; i < pdrv->MmioRegion.size; i++ ) {
        if( !(i % 16) )
            ErrorF( "\nMM%02xx ", i >> 4 );
        ErrorF( " %02x", SPITFIRE_MMIO[i] );
    }

    outb(SPITFIRE_INDEX, SPITFIRE_MEM_MAP_ENABLE);
    outb(SPITFIRE_DATA, save);

    ErrorF("\n\n");
}

Bool SpitfireSwitchMode(SWITCH_MODE_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    SpitfirePtr pdrv = DEVPTR(pScrn);
    Bool success;

    TRACE(("SpitfireSwitchMode\n"));
    success = SpitfireModeInit(pScrn, mode);

    return success;
}



