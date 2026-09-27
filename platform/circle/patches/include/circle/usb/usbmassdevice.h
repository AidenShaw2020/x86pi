//
// usbmassdevice.h
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2022  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#ifndef _circle_usb_usbmassdevice_h
#define _circle_usb_usbmassdevice_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/fs/partitionmanager.h>
#include <circle/numberpool.h>
#include <circle/types.h>

#define UMSD_BLOCK_SIZE		512
#define UMSD_BLOCK_MASK		(UMSD_BLOCK_SIZE-1)
#define UMSD_BLOCK_SHIFT	9

#define UMSD_MAX_OFFSET		0x1FFFFFFFFFFULL		// 2TB

class CUSBBulkOnlyMassStorageDevice : public CUSBFunction
{
public:
	CUSBBulkOnlyMassStorageDevice (CUSBFunction *pFunction);
	~CUSBBulkOnlyMassStorageDevice (void);

	boolean Configure (void);

	int Read (void *pBuffer, size_t nCount);
	int Write (const void *pBuffer, size_t nCount);

	u64 Seek (u64 ullOffset);

	u64 GetSize (void) const;		// in bytes
	unsigned GetCapacity (void) const;	// in blocks

	// True once, when the disc has been changed or taken out, so that a
	// caller can throw away whatever it had cached.  There is no wire for
	// this on USB, so the drive is asked - rate limited, see the cpp.
	boolean PollMedium (void);

	// Find out whether there is a disc in the drive and how big it is.
	// A CD drive spends most of its life empty, so this is what the first
	// read does rather than something Configure() insists on - and a
	// caller that wants the size before reading has to ask for it, or the
	// drive is never looked in at all.  Rate limited while it is empty.
	boolean CheckMedia (void);

private:
	// Ask for sense after a failed command, which is what tells us why and
	// what clears the condition on the device.
	void RequestSense (void);
	int TryRead (void *pBuffer, size_t nCount);
	int TryWrite (const void *pBuffer, size_t nCount);

	int Command (void *pCmdBlk, size_t nCmdBlkLen, void *pBuffer, size_t nBufLen, boolean bIn);

	int Reset (void);

private:
	CUSBEndpoint *m_pEndpointIn;
	CUSBEndpoint *m_pEndpointOut;

	unsigned m_nCWBTag;
	unsigned m_nBlockCount;
	/*
	 * The medium's own block, not this driver's assumption.
	 *
	 * A disk reads in 512 byte blocks and a CD in 2048 byte ones, and the
	 * difference reaches every command: READ(10) counts blocks, and an
	 * offset in bytes has to be divided by the right thing.  This used to
	 * be the constant UMSD_BLOCK_SIZE throughout, which is why a CD drive
	 * could not be supported at all.
	 */
	unsigned m_nBlockSize;
	unsigned m_nBlockShift;
	boolean m_bCD;			// answers INQUIRY as a CD/DVD drive
	boolean m_bMediumChanged;	// and has said the disc is a new one
	boolean m_bRecheck;		// the disc may be another one; look again
	u8 m_ucASC, m_ucASCQ;		// why the last command failed
	unsigned m_nLastMediaCheck;	// ticks, so an empty drive is left alone
	unsigned m_nLastPoll;		// ticks, so a swap is looked for now and then
	u64 m_ullOffset;

	CPartitionManager *m_pPartitionManager;

	static CNumberPool s_DeviceNumberPool;
	static CNumberPool s_CDNumberPool;	// ucd1 is the first CD drive
	unsigned m_nDeviceNumber;
};

#endif
