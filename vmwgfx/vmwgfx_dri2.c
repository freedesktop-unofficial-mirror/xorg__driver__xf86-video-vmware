/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2011 VMWare, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Author: Alan Hourihane <alanh@tungstengraphics.com>
 * Author: Jakob Bornecrantz <wallbraker@gmail.com>
 * Author: Thomas Hellstrom <thellstrom@vmware.com>
 *
 */

#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"

#include "vmwgfx_driver.h"
#include "../saa/saa.h"

#include "dri2.h"
#include "gcstruct.h"
#include "gc.h"
#include "vmwgfx_saa.h"
#include "wsbm_util.h"

#ifdef DRI2
typedef struct {
    int refcount;
    PixmapPtr pPixmap;
    struct xa_surface *srf;
} *BufferPrivatePtr;


/*
 * Attempt to guess what the dri state tracker is up to.
 * Currently it sends only bpp as format.
 */

static unsigned int
vmwgfx_color_format_to_depth(unsigned int format)
{
    return format;
}

static unsigned int
vmwgfx_zs_format_to_depth(unsigned int format)
{
    if (format == 24)
	return 32;
    return format;
}

static unsigned int
vmwgfx_z_format_to_depth(unsigned int format)
{
    return format;
}

static Bool
dri2_do_create_buffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer, unsigned int format)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    BufferPrivatePtr private = buffer->driverPrivate;
    PixmapPtr pPixmap;
    struct vmwgfx_saa_pixmap *vpix;
    struct xa_surface *srf = NULL;
    unsigned int depth;


    if (pDraw->type == DRAWABLE_PIXMAP)
	pPixmap = (PixmapPtr) pDraw;
    else
	pPixmap = (*pScreen->GetWindowPixmap)((WindowPtr) pDraw);

    vpix = vmwgfx_saa_pixmap(pPixmap);
    private->refcount = 0;

    switch (buffer->attachment) {
    default:
	depth = (format) ? vmwgfx_color_format_to_depth(format) :
	    pDraw->depth;

	if (buffer->attachment != DRI2BufferFakeFrontLeft ||
	    &pPixmap->drawable != pDraw) {

	    pPixmap = (*pScreen->CreatePixmap)(pScreen,
					       pDraw->width,
					       pDraw->height,
					       depth,
					       0);
	    if (pPixmap == NullPixmap)
		return FALSE;

	    private->pPixmap = pPixmap;
	    vpix = vmwgfx_saa_pixmap(pPixmap);
	}
	break;
    case DRI2BufferFrontLeft:
      if (&pPixmap->drawable == pDraw)
	  break;
      buffer->name = 0;
      buffer->pitch = 0;
      buffer->cpp = pDraw->bitsPerPixel / 8;
      buffer->driverPrivate = private;
      buffer->flags = 0; /* not tiled */
      buffer->format = pDraw->bitsPerPixel;
      if (!private->pPixmap) {
	private->pPixmap = pPixmap;
	pPixmap->refcnt++;
      }
      return TRUE;
    case DRI2BufferStencil:
    case DRI2BufferDepthStencil:

	depth = (format) ? vmwgfx_zs_format_to_depth(format) : 32;

	/*
	 * The SVGA device uses the zs ordering.
	 */

	srf = xa_surface_create(ms->xat, pDraw->width, pDraw->height,
				depth, xa_type_zs, xa_format_unknown,
				XA_FLAG_SHARED );
	if (!srf)
	    return FALSE;

       break;
    case DRI2BufferDepth:
	depth = (format) ? vmwgfx_z_format_to_depth(format) :
	    pDraw->bitsPerPixel;
	srf = xa_surface_create(ms->xat, pDraw->width, pDraw->height,
				depth,
				xa_type_z, xa_format_unknown,
				XA_FLAG_SHARED);
	if (!srf)
	    return FALSE;
	break;
    }

    if (!private->pPixmap) {
	private->pPixmap = pPixmap;
	pPixmap->refcnt++;
    }

    if (!srf) {
	depth = (format) ? vmwgfx_color_format_to_depth(format) :
	    pDraw->depth;

	if (!vmwgfx_hw_dri2_validate(pPixmap, depth))
	    return FALSE;

	srf = vpix->hw;

	/*
	 * Compiz workaround. See vmwgfx_dirty();
	 */

	vpix->hw_is_dri2_fronts++;
	private->refcount++;
    }

    private->srf = srf;
    if (xa_surface_handle(srf, &buffer->name, &buffer->pitch) != 0)
	return FALSE;

    buffer->cpp = xa_format_depth(xa_surface_format(srf)) / 8;
    buffer->driverPrivate = private;
    buffer->flags = 0; /* not tiled */
    buffer->format = format;
    private->refcount++;

    return TRUE;
}

static void
dri2_do_destroy_buffer(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
    BufferPrivatePtr private = buffer->driverPrivate;
    struct xa_surface *srf = private->srf;
    ScreenPtr pScreen = pDraw->pScreen;

    if (--private->refcount == 0 && srf) {
	xa_surface_destroy(srf);
    }

    /*
     * Compiz workaround. See vmwgfx_dirty();
     */

    if (private->refcount == 1) {
	struct vmwgfx_saa_pixmap *vpix = vmwgfx_saa_pixmap(private->pPixmap);
	if (--vpix->hw_is_dri2_fronts == 0)
	    WSBMLISTDELINIT(&vpix->sync_x_head);
    }

    private->srf = NULL;
    pScreen->DestroyPixmap(private->pPixmap);
}


static DRI2Buffer2Ptr
dri2_create_buffer(DrawablePtr pDraw, unsigned int attachment, unsigned int format)
{
    DRI2Buffer2Ptr buffer;
    BufferPrivatePtr private;

    buffer = calloc(1, sizeof *buffer);
    if (!buffer)
	return NULL;

    private = calloc(1, sizeof *private);
    if (!private) {
	goto fail;
    }

    buffer->attachment = attachment;
    buffer->driverPrivate = private;

    if (dri2_do_create_buffer(pDraw, buffer, format))
	return buffer;

    free(private);
fail:
    free(buffer);
    return NULL;
}

static void
dri2_destroy_buffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer)
{
    /* So far it is safe to downcast a DRI2Buffer2Ptr to DRI2BufferPtr */
    dri2_do_destroy_buffer(pDraw, (DRI2BufferPtr)buffer);

    free(buffer->driverPrivate);
    free(buffer);
}

static void
dri2_copy_region(DrawablePtr pDraw, RegionPtr pRegion,
                 DRI2Buffer2Ptr pDestBuffer, DRI2Buffer2Ptr pSrcBuffer)
{


    ScreenPtr pScreen = pDraw->pScreen;
    BufferPrivatePtr dst_priv = pDestBuffer->driverPrivate;
    BufferPrivatePtr src_priv = pSrcBuffer->driverPrivate;
    DrawablePtr src_draw;
    DrawablePtr dst_draw;
    RegionPtr myClip;
    GCPtr gc;

    /*
     * In driCreateBuffers we dewrap windows into the
     * backing pixmaps in order to get to the texture.
     * We need to use the real drawable in CopyArea
     * so that cliprects and offsets are correct.
     */
    src_draw = (pSrcBuffer->attachment == DRI2BufferFrontLeft) ? pDraw :
       &src_priv->pPixmap->drawable;
    dst_draw = (pDestBuffer->attachment == DRI2BufferFrontLeft) ? pDraw :
       &dst_priv->pPixmap->drawable;

    /*
     * The clients implements glXWaitX with a copy front to fake and then
     * waiting on the server to signal its completion of it. While
     * glXWaitGL is a client side flush and a copy from fake to front.
     * This is how it is done in the DRI2 protocol, how ever depending
     * which type of drawables the server does things a bit differently
     * then what the protocol says as the fake and front are the same.
     *
     * for pixmaps glXWaitX is a server flush.
     * for pixmaps glXWaitGL is a client flush.
     * for windows glXWaitX is a copy from front to fake then a server flush.
     * for windows glXWaitGL is a client flush then a copy from fake to front.
     *
     * XXX in the windows case this code always flushes but that isn't a
     * must in the glXWaitGL case but we don't know if this is a glXWaitGL
     * or a glFlush/glFinish call.
     */
    if (dst_priv->pPixmap == src_priv->pPixmap) {
	/* pixmap glXWaitX */
	if (pSrcBuffer->attachment == DRI2BufferFrontLeft &&
	    pDestBuffer->attachment == DRI2BufferFakeFrontLeft) {

	    if (!vmwgfx_hw_dri2_validate(src_priv->pPixmap, 0))
		return;
	}
	/* pixmap glXWaitGL */
	if (pDestBuffer->attachment == DRI2BufferFrontLeft &&
	    pSrcBuffer->attachment == DRI2BufferFakeFrontLeft) {
	    return;
	} else {
	    vmwgfx_flush_dri2(pScreen);
	    return;
	}
    }

    gc = GetScratchGC(pDraw->depth, pScreen);
    myClip = REGION_CREATE(pScreen, REGION_RECTS(pRegion),
			   REGION_NUM_RECTS(pRegion));
    (*gc->funcs->ChangeClip) (gc, CT_REGION, myClip, 0);
    ValidateGC(dst_draw, gc);

    /*
     * Damage the src drawable in order for damageCopyArea to pick up
     * that something changed.
     */
    DamageRegionAppend(src_draw, pRegion);
    if (pSrcBuffer->attachment != DRI2BufferFrontLeft)
	saa_drawable_dirty(src_draw, TRUE, pRegion);
    DamageRegionProcessPending(src_draw);

    /*
     * Call CopyArea. This usually means a call to damageCopyArea that
     * is wrapping saa_copy_area. The damageCopyArea function will make
     * sure the destination drawable is appropriately damaged.
     */
    (*gc->ops->CopyArea)(src_draw, dst_draw, gc,
			 0, 0, pDraw->width, pDraw->height, 0, 0);

    /*
     * FreeScratchGC will free myClip as well.
     */
    myClip = NULL;
    FreeScratchGC(gc);
}

Bool
xorg_dri2_init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    modesettingPtr ms = modesettingPTR(pScrn);
    DRI2InfoRec dri2info;
    int major, minor;

    if (xf86LoaderCheckSymbol("DRI2Version")) {
	DRI2Version(&major, &minor);
    } else {
	/* Assume version 1.0 */
	major = 1;
	minor = 0;
    }

    dri2info.version = min(DRI2INFOREC_VERSION, 3);
    dri2info.fd = ms->fd;

    dri2info.driverName = pScrn->driverName;
    dri2info.deviceName = "/dev/dri/card0"; /* FIXME */

    dri2info.CreateBuffer = dri2_create_buffer;
    dri2info.DestroyBuffer = dri2_destroy_buffer;

    dri2info.CopyRegion = dri2_copy_region;
    dri2info.Wait = NULL;

    return DRI2ScreenInit(pScreen, &dri2info);
}

void
xorg_dri2_close(ScreenPtr pScreen)
{
    DRI2CloseScreen(pScreen);
}
#endif

/* vim: set sw=4 ts=8 sts=4: */