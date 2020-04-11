/////////////////////////////////////////////////////////////////////////////
//  Copyright (C) 2002-2020 UltraVNC Team Members. All Rights Reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
//  USA.
//
// If the source code for the program is not available from the place from
// which you received this file, check 
// http://www.uvnc.com/
//
////////////////////////////////////////////////////////////////////////////
 

// ZLIB Encoding
//
// The bits of the ClientConnection object to do with zlib.
#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "Exception.h"
#ifdef _INTERNALLIB
#include <zlib.h>
#include <zstd.h>
#else
//#include "../zlib/zlib.h"
#include "../zstd-1.4.4/lib/zstd.h"
#endif


void ClientConnection::ReadZlibRect(rfbFramebufferUpdateRectHeader *pfburh, bool zstd) {

	UINT numpixels = pfburh->r.w * pfburh->r.h;
    // this assumes at least one byte per pixel. Naughty.
	UINT numRawBytes = numpixels * m_minPixelBytes;
	UINT numCompBytes;

	rfbZlibHeader hdr;

	// Read in the rfbZlibHeader
	ReadExact((char *)&hdr, sz_rfbZlibHeader);

	numCompBytes = Swap32IfLE(hdr.nBytes);

	// Read in the compressed data
    CheckBufferSize(numCompBytes);
	ReadExact(m_netbuf, numCompBytes);

	// Verify enough buffer space for screen update.
	CheckZlibBufferSize(numRawBytes);
	if (ultraVncZlib->decompress(numCompBytes, numRawBytes, (unsigned char *)m_netbuf, m_zlibbuf, zstd) != Z_OK)
		return;

	SETUP_COLOR_SHORTCUTS;

	// No other threads can use bitmap DC
	omni_mutex_lock l(m_bitmapdcMutex);	

	// This big switch is untidy but fast
	switch (m_myFormat.bitsPerPixel) {
		case 8:
			SETPIXELS(m_zlibbuf, 8, pfburh->r.x, pfburh->r.y, pfburh->r.w, pfburh->r.h)
				break;
		case 16:
			SETPIXELS(m_zlibbuf, 16, pfburh->r.x, pfburh->r.y, pfburh->r.w, pfburh->r.h)
				break;
		case 24:
		case 32:
			SETPIXELS(m_zlibbuf, 32, pfburh->r.x, pfburh->r.y, pfburh->r.w, pfburh->r.h)            
				break;
		default:
			vnclog.Print(0, _T("Invalid number of bits per pixel: %d\n"), m_myFormat.bitsPerPixel);
			return;
	}
}
// Makes sure zlibbuf is at least as big as the specified size.
// Note that zlibbuf itself may change as a result of this call.
// Throws an exception on failure.
void ClientConnection::CheckZlibBufferSize(int bufsize)
{
	unsigned char *newbuf;

	if (m_zlibbufsize > bufsize + 256) return;

	omni_mutex_lock l(m_zlibBufferMutex);


	newbuf = (unsigned char *)new char[bufsize+256];
	ZeroMemory(newbuf, bufsize+256);
	
	if (newbuf == NULL) {
		throw ErrorException("Insufficient memory to allocate zlib buffer.");
	}

	// Only if we're successful...

	if (m_zlibbuf != NULL)
		delete [] m_zlibbuf;
	m_zlibbuf = newbuf;
	m_zlibbufsize=bufsize + 256;
	vnclog.Print(4, _T("zlibbufsize expanded to %d\n"), m_zlibbufsize);


}

void ClientConnection::ReadQueueZip(rfbFramebufferUpdateRectHeader *pfburh,HRGN *prgn, bool zstd)
{
	UINT nNbCacheRects = pfburh->r.x;
	UINT numRawBytes = pfburh->r.y+pfburh->r.w*65535;
	UINT numCompBytes;
	rfbZlibHeader hdr;
	// Read in the rfbZlibHeader
	ReadExact((char *)&hdr, sz_rfbZlibHeader);
	numCompBytes = Swap32IfLE(hdr.nBytes);
	// Check the net buffer
	CheckBufferSize(numCompBytes);
	// Read the compressed data
	ReadExact((char *)m_netbuf, numCompBytes);

	// Verify buffer space for cache rects list
	CheckZipBufferSize(numRawBytes+500);

	if (ultraVncZlib->decompress(numCompBytes, numRawBytes, (unsigned char *)m_netbuf, m_zlibbuf, zstd) != Z_OK)
		return;

	BYTE* pzipbuf = m_zipbuf;
	for (UINT ii = 0 ; ii < nNbCacheRects; ii++)
	{
		rfbFramebufferUpdateRectHeader surh;
		memcpy((char *) &surh,pzipbuf, sz_rfbFramebufferUpdateRectHeader);
		surh.r.x = Swap16IfLE(surh.r.x);
		surh.r.y = Swap16IfLE(surh.r.y);
		surh.r.w = Swap16IfLE(surh.r.w);
		surh.r.h = Swap16IfLE(surh.r.h);
		surh.encoding = Swap32IfLE(surh.encoding);
		pzipbuf += sz_rfbFramebufferUpdateRectHeader;

		RECT rect;
		rect.left = surh.r.x;
		rect.right = surh.r.x + surh.r.w;
		rect.top = surh.r.y;
		rect.bottom = surh.r.y + surh.r.h;

		SoftCursorLockArea(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
		SaveArea(rect);
		if ( surh.encoding==rfbEncodingRaw) {
			UINT numpixels = surh.r.w * surh.r.h;
			SETUP_COLOR_SHORTCUTS;
			omni_mutex_lock l(m_bitmapdcMutex);						  

			// This big switch is untidy but fast
			switch (m_myFormat.bitsPerPixel) {
				case 8:
					SETPIXELS(pzipbuf, 8, surh.r.x, surh.r.y, surh.r.w, surh.r.h)
						break;
				case 16:
					SETPIXELS(pzipbuf, 16, surh.r.x, surh.r.y, surh.r.w, surh.r.h)
						break;
				case 24:
				case 32:
					SETPIXELS(pzipbuf, 32, surh.r.x, surh.r.y, surh.r.w, surh.r.h)            
						break;
				default:
					vnclog.Print(0, _T("Invalid number of bits per pixel: %d\n"), m_myFormat.bitsPerPixel);
			}
			pzipbuf +=numpixels*m_myFormat.bitsPerPixel/8;
			if (!m_opts.m_Directx)InvalidateRegion(&rect,prgn);
		}
	}
}