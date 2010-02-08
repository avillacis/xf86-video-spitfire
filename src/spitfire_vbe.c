
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "spitfire_driver.h"
#include "spitfire_vbe.h"

#define iabs(a)	((int)(a)>0?(a):(-(a)))

#if X_BYTE_ORDER == X_LITTLE_ENDIAN
#define B_O16(x)  (x) 
#define B_O32(x)  (x)
#else
#define B_O16(x)  ((((x) & 0xff) << 8) | (((x) & 0xff) >> 8))
#define B_O32(x)  ((((x) & 0xff) << 24) | (((x) & 0xff00) << 8) \
                  | (((x) & 0xff0000) >> 8) | (((x) & 0xff000000) >> 24))
#endif
#define L_ADD(x)  (B_O32(x) & 0xffff) + ((B_O32(x) >> 12) & 0xffff00)

void SpitfireSetVESAModeCrtc1( SpitfirePtr pdrv, int n, int Refresh );
void SpitfireSetVESAModeCrtc2( SpitfirePtr pdrv, int n, int Refresh );

static void
SpitfireClearVM86Regs( xf86Int10InfoPtr pInt )
{
    pInt->ax = 0;
    pInt->bx = 0;
    pInt->cx = 0;
    pInt->dx = 0;
    pInt->si = 0;
    pInt->di = 0;
    pInt->es = 0xc000;
    pInt->num = 0x10;
}

void
SpitfireSetTextMode( SpitfirePtr pdrv )
{
#if 0
    /* Restore display device if changed. */
    if( pdrv->iDevInfo != pdrv->iDevInfoPrim ) {
	SpitfireClearVM86Regs( pdrv->pVbe->pInt10 );
	pdrv->pVbe->pInt10->ax = 0x4f14;
	pdrv->pVbe->pInt10->bx = 0x0003;
	pdrv->pVbe->pInt10->cx = pdrv->iDevInfoPrim;
	xf86ExecX86int10( pdrv->pVbe->pInt10 );
    }
#endif
    SpitfireClearVM86Regs( pdrv->pVbe->pInt10 );

    pdrv->pVbe->pInt10->ax = 0x03;

    xf86ExecX86int10( pdrv->pVbe->pInt10 );
}

void
SpitfireSetVESAMode( SpitfirePtr pdrv, int n)
{
    if( xf86LoaderCheckSymbol( "VBESetVBEMode" ) )
    {
	if( !VBESetVBEMode( pdrv->pVbe, n, NULL ) )
	{
	    ErrorF("Set video mode failed\n");
	}
    }
}

void
SpitfireFreeBIOSModeTable( SpitfirePtr pdrv, SpitfireModeTablePtr* ppTable )
{
    int i;
    SpitfireModeEntryPtr pMode = (*ppTable)->Modes;

    for( i = (*ppTable)->NumModes; i--; )
    {
	if( pMode->RefreshRate )
	{
	    xfree( pMode->RefreshRate );
	    pMode->RefreshRate = NULL;
	}
	pMode++;
    }

    xfree( *ppTable );
}


SpitfireModeTablePtr
SpitfireGetBIOSModeTable( SpitfirePtr pdrv, int iDepth )
{
    int nModes = SpitfireGetBIOSModes( pdrv, iDepth, NULL );
    SpitfireModeTablePtr pTable;

    pTable = (SpitfireModeTablePtr) 
	xcalloc( 1, sizeof(SpitfireModeTableRec) + 
		    (nModes-1) * sizeof(SpitfireModeEntry) );
    if( pTable ) {
	pTable->NumModes = nModes;
	SpitfireGetBIOSModes( pdrv, iDepth, pTable->Modes );
    }

    return pTable;
}


unsigned short
SpitfireGetBIOSModes( 
    SpitfirePtr pdrv,
    int iDepth,
    SpitfireModeEntryPtr oakModeTable )
{
    unsigned short iModeCount = 0;
    unsigned short int *mode_list;
    pointer vbeLinear = NULL;
    VbeInfoBlock *vbe;
    int vbeReal;
    struct vbe_mode_info_block * vmib;

    if( !pdrv->pVbe )
	return 0;

    vbeLinear = xf86Int10AllocPages( pdrv->pVbe->pInt10, 1, &vbeReal );
    if( !vbeLinear )
    {
	ErrorF( "Cannot allocate scratch page in real mode memory." );
	return 0;
    }
    vmib = (struct vbe_mode_info_block *) vbeLinear;
    
    if (!(vbe = VBEGetVBEInfo(pdrv->pVbe)))
	return 0;

    for (mode_list = vbe->VideoModePtr; *mode_list != 0xffff; mode_list++) {

	SpitfireClearVM86Regs( pdrv->pVbe->pInt10 );

	pdrv->pVbe->pInt10->ax = 0x4f01;
	pdrv->pVbe->pInt10->cx = *mode_list;
	pdrv->pVbe->pInt10->es = SEG_ADDR(vbeReal);
	pdrv->pVbe->pInt10->di = SEG_OFF(vbeReal);
	pdrv->pVbe->pInt10->num = 0x10;

	xf86ExecX86int10( pdrv->pVbe->pInt10 );

	if( 
	   (vmib->bits_per_pixel == iDepth) &&
	   (
	      (vmib->memory_model == VBE_MODEL_256) ||
	      (vmib->memory_model == VBE_MODEL_PACKED) ||
	      (vmib->memory_model == VBE_MODEL_RGB)
	   )
	)
	{
	    /* This mode is a match. */

	    iModeCount++;

	    /* If we're supposed to fetch information, do it now. */

	    if( oakModeTable )
	    {
	        int iRefresh = 0;

		oakModeTable->Width = vmib->x_resolution;
		oakModeTable->Height = vmib->y_resolution;
		oakModeTable->VesaMode = *mode_list;
		
		/* Query the refresh rates at this mode. */

		pdrv->pVbe->pInt10->cx = *mode_list;
		pdrv->pVbe->pInt10->dx = 0;
#if 0
		do
		{
		    if( (iRefresh % 8) == 0 )
		    {
			if( oakModeTable->RefreshRate )
			{
			    oakModeTable->RefreshRate = (unsigned char *)
				xrealloc( 
				    oakModeTable->RefreshRate,
				    (iRefresh+8) * sizeof(unsigned char)
				);
			}
			else
			{
			    oakModeTable->RefreshRate = (unsigned char *)
				xcalloc( 
				    sizeof(unsigned char),
				    (iRefresh+8)
				);
			}
		    }

		    pdrv->pVbe->pInt10->ax = 0x4f14;	/* S3 extended functions */
		    pdrv->pVbe->pInt10->bx = 0x0201;	/* query refresh rates */
		    pdrv->pVbe->pInt10->num = 0x10;
		    xf86ExecX86int10( pdrv->pVbe->pInt10 );

		    oakModeTable->RefreshRate[iRefresh++] = pdrv->pVbe->pInt10->di;
		}
		while( pdrv->pVbe->pInt10->dx );

		oakModeTable->RefreshCount = iRefresh;
#endif
		oakModeTable->RefreshCount = 0;
	    	oakModeTable++;
	    }
	}
    }

    VBEFreeVBEInfo(vbe);

    xf86Int10FreePages( pdrv->pVbe->pInt10, vbeLinear, 1 );

    return iModeCount;
}

ModeStatus SpitfireMatchBiosMode(ScrnInfoPtr pScrn,int width,int height,int refresh,
                              unsigned int *vesaMode,unsigned int *newRefresh)
{
    SpitfireModeEntryPtr pmt;
    Bool found = FALSE;
    SpitfirePtr pdrv = DEVPTR(pScrn);    
    int i,j;
    unsigned int chosenVesaMode = 0;
    unsigned int chosenRefresh = 0;
    
    /* Scan through our BIOS list to locate the closest valid mode. */
    
    /*
     * If we ever break 4GHz clocks on video boards, we'll need to
     * change this.
     * refresh = (mode->Clock * 1000) / (mode->HTotal * mode->VTotal);
     * now we use VRefresh directly,instead of by calculating from dot clock
     */

    for( i = 0, pmt = pdrv->ModeTable->Modes; 
	i < pdrv->ModeTable->NumModes;
	i++, pmt++ )
    {
	if( (pmt->Width == width) && 
	    (pmt->Height == height) )
	{
	    int jDelta = 99;
	    int jBest = 0;

	    /* We have an acceptable mode.  Find a refresh rate. */
	    chosenVesaMode = pmt->VesaMode;
            if (vesaMode)
                *vesaMode = chosenVesaMode;
	    for( j = 0; j < pmt->RefreshCount; j++ )
	    {
		if( pmt->RefreshRate[j] == refresh )
		{
		    /* Exact match. */
		    jBest = j;
		    break;
		}
		else if( iabs(pmt->RefreshRate[j] - refresh) < jDelta )
		{
		    jDelta = iabs(pmt->RefreshRate[j] - refresh);
		    jBest = j;
		}
	    }
	    if (pmt->RefreshRate && pmt->RefreshCount > 0)
		chosenRefresh = pmt->RefreshRate[jBest];
	    else chosenRefresh = 0;
            if (newRefresh)
                *newRefresh = chosenRefresh;
            found = TRUE;
	    break;
	}
    }

    if( found ) {
	/* Success: we found a match in the BIOS. */
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED, 
		  "Chose mode %x at %dHz.\n", chosenVesaMode, chosenRefresh );
        return MODE_OK;
    } else {
	xf86DrvMsg(pScrn->scrnIndex, X_PROBED, 
		  "No suitable BIOS mode found for %dx%d %dHz.\n",
		  width, height, refresh);
        return MODE_NOMODE;
    }
}
