//
// usbfloppydevice.h
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2024  R. Stange <rsta2@o2online.de>
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
#ifndef _circle_usb_usbfloppydevice_h
#define _circle_usb_usbfloppydevice_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/numberpool.h>
#include <circle/types.h>

class CUSBFloppyDiskDevice : public CUSBFunction /// Driver for USB floppy disk devices (CBI)
{
public:
	CUSBFloppyDiskDevice (CUSBFunction *pFunction);
	~CUSBFloppyDiskDevice (void);

	boolean Configure (void);

	int Read (void *pBuffer, size_t nCount);
	int Write (const void *pBuffer, size_t nCount);

	u64 Seek (u64 ullOffset);

	u64 GetSize (void) const;

private:
	int WaitUnitReady (void);

	// Look for a diskette, and read its capacity if there is one.
	// Cheap to call and rate limited, because an empty drive answers
	// by moving its heads.
	boolean CheckMedia (void);
	// Ask for sense after a command has failed, which is what clears
	// the failure on the device.  See where it is called.
	void ClearFailure (u8 ucOpCode);

public:
	// True once, when a different diskette has been noticed; the caller
	// asks so that it can throw away anything it had cached.
	boolean MediumChanged (void);
	// The same question, asked of the drive rather than of what it has
	// already said.  There is no change line on a USB floppy, so this
	// sends it a command and reads the answer; rate limited, because the
	// caller is a guest doing ordinary reads.
	boolean PollMedium (void);

private:

	// Work the head loose before the guest needs it; see the cpp.
	void LimberUp (void);
	int TryRead (void *pBuffer, size_t nCount);
	int TryWrite (const void *pBuffer, size_t nCount);

	int Command (void *pCmdBlk, size_t nCmdBlkLen, void *pBuffer, size_t nBufLen, boolean bIn);

	int Reset (void);

private:
	CUSBEndpoint *m_pEndpointIn;
	CUSBEndpoint *m_pEndpointOut;
	CUSBEndpoint *m_pEndpointInterrupt;

	unsigned m_nBlockCount;		// 0 when the drive is empty
	u8 m_ucASC, m_ucASCQ;		// why the last command failed
	boolean m_bMediumChanged;	// and whether it was a new diskette
	u8 m_ucLastComplaint;		// so a reason is logged once, not forever
	unsigned m_nRefusedCommands;	// in a row, on the control endpoint
	boolean m_bLimbered;		// the above has been done, once
	boolean m_bClearingFailure;	// no recursion while clearing one
	unsigned m_nLastMediaCheck;	// ticks, so an empty drive is left alone
	unsigned m_nLastPoll;		// ticks, so a swap is looked for now and then
	u64 m_ullOffset;

	static CNumberPool s_DeviceNumberPool;
	unsigned m_nDeviceNumber;
};

#endif
