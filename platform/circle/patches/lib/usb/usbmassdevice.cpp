//
// usbmassdevice.cpp
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
#include <circle/usb/usbmassdevice.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <circle/macros.h>
#include <circle/new.h>
#include <assert.h>

#define MAX_TRIES	8				// max. read / write attempts

// USB Mass Storage Bulk-Only Transport

// Class-specific requests
#define GET_MAX_LUN			0xFE
#define BULK_ONLY_MASS_STORAGE_RESET	0xFF

// Command Block Wrapper
struct TCBW
{
	u32		dCWBSignature,
#define CBWSIGNATURE		0x43425355
			dCWBTag,
			dCBWDataTransferLength;		// number of bytes
	u8		bmCBWFlags,
#define CBWFLAGS_DATA_IN	0x80
			bCBWLUN		: 4,
#define CBWLUN			0
			Reserved1	: 4,
			bCBWCBLength	: 5,		// valid length of the CBWCB in bytes
			Reserved2	: 3;
	u8		CBWCB[16];
}
PACKED;

// Command Status Wrapper
struct TCSW
{
	u32		dCSWSignature,
#define CSWSIGNATURE		0x53425355
			dCSWTag,
			dCSWDataResidue;		// difference in amount of data processed
	u8		bCSWStatus;
#define CSWSTATUS_PASSED	0x00
#define CSWSTATUS_FAILED	0x01
#define CSWSTATUS_PHASE_ERROR	0x02
}
PACKED;

// SCSI Transparent Command Set

#define SCSI_CONTROL		0x00

struct TSCSIInquiry
{
	u8		OperationCode,
#define SCSI_OP_INQUIRY		0x12
			LogicalUnitNumberEVPD,
			PageCode,
			Reserved,
			AllocationLength,
			Control;
}
PACKED;

struct TSCSIInquiryResponse
{
	u8		PeripheralDeviceType	: 5,
#define SCSI_PDT_DIRECT_ACCESS_BLOCK	0x00			// SBC-2 command set (or above)
#define SCSI_PDT_DIRECT_ACCESS_RBC	0x0E			// RBC command set
#define SCSI_PDT_CD_DVD			0x05			// MMC command set
			PeripheralQualifier	: 3,		// 0: device is connected to this LUN
			DeviceTypeModifier	: 7,
			RMB			: 1,		// 1: removable media
			ANSIApprovedVersion	: 3,
			ECMAVersion		: 3,
			ISOVersion		: 2,
			Reserved1,
			AdditionalLength,
			Reserved2[3],
			VendorIdentification[8],
			ProductIdentification[16],
			ProductRevisionLevel[4];
}
PACKED;

struct TSCSITestUnitReady
{
	u8		OperationCode;
#define SCSI_OP_TEST_UNIT_READY		0x00
	u32		Reserved;
	u8		Control;
}
PACKED;

struct TSCSIRequestSense
{
	u8		OperationCode;
#define SCSI_REQUEST_SENSE		0x03
	u8		DescriptorFormat	: 1,		// set to 0
			Reserved1		: 7;
	u16		Reserved2;
	u8		AllocationLength;
	u8		Control;
}
PACKED;

struct TSCSIRequestSenseResponse7x
{
	u8		ResponseCode		: 7,
			Valid			: 1;
	u8		Obsolete;
	u8		SenseKey		: 4,
			Reserved		: 1,
			ILI			: 1,
			EOM			: 1,
			FileMark		: 1;
	u32		Information;				// big endian
	u8		AdditionalSenseLength;
	u32		CommandSpecificInformation;		// big endian
	u8		AdditionalSenseCode;
	u8		AdditionalSenseCodeQualifier;
	u8		FieldReplaceableUnitCode;
	u8		SenseKeySpecificHigh	: 7,
			SKSV			: 1;
	u16		SenseKeySpecificLow;
}
PACKED;

struct TSCSIReadCapacity10
{
	u8		OperationCode;
#define SCSI_OP_READ_CAPACITY10		0x25
	u8		Obsolete		: 1,
			Reserved1		: 7;
	u32		LogicalBlockAddress;			// set to 0
	u16		Reserved2;
	u8		PartialMediumIndicator	: 1,		// set to 0
			Reserved3		: 7;
	u8		Control;
}
PACKED;

struct TSCSIReadCapacityResponse
{
	u32		ReturnedLogicalBlockAddress;		// big endian
	u32		BlockLengthInBytes;			// big endian
}
PACKED;

struct TSCSIRead10
{
	u8		OperationCode,
#define SCSI_OP_READ		0x28
			Reserved1;
	u32		LogicalBlockAddress;			// big endian
	u8		Reserved2;
	u16		TransferLength;				// block count, big endian
	u8		Control;
#define SCSI_READ_CONTROL	0x00
}
PACKED;

struct TSCSIWrite10
{
	u8		OperationCode,
#define SCSI_OP_WRITE		0x2A
			Flags;
#define SCSI_WRITE_FUA		0x08
	u32		LogicalBlockAddress;			// big endian
	u8		Reserved;
	u16		TransferLength;				// block count, big endian
	u8		Control;
#define SCSI_WRITE_CONTROL	0x00
}
PACKED;

CNumberPool CUSBBulkOnlyMassStorageDevice::s_DeviceNumberPool (1);
CNumberPool CUSBBulkOnlyMassStorageDevice::s_CDNumberPool (1);

/*
 * Why the sense codes are here at all.
 *
 * A CD drive is empty most of the time, and says so; a disc that has just
 * been put in is announced once, as a unit attention, to whoever asks first.
 * None of that is a fault, and a driver that treats every refusal as one -
 * which this did, resetting the device after each - can neither find a disc
 * nor notice that it has been swapped.  The same lesson as the floppy.
 */
#define ASC_NOT_READY			0x04	// there is a disc, coming up
#define ASC_MEDIUM_CHANGED		0x28	// a different disc
#define ASC_RESET			0x29	// powered on, or reset
#define ASC_NO_MEDIUM			0x3A	// the drawer is empty

static const char FromUmsd[] = "umsd";

CUSBBulkOnlyMassStorageDevice::CUSBBulkOnlyMassStorageDevice (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEndpointIn (0),
	m_pEndpointOut (0),
	m_nCWBTag (0),
	m_nBlockCount (0),
	m_nBlockSize (UMSD_BLOCK_SIZE),
	m_nBlockShift (UMSD_BLOCK_SHIFT),
	m_bCD (FALSE),
	m_bMediumChanged (FALSE),
	m_bRecheck (FALSE),
	m_ucASC (0),
	m_ucASCQ (0),
	m_nLastMediaCheck (0),
	m_nLastPoll (0),
	m_ullOffset (0),
	m_pPartitionManager (0),
	m_nDeviceNumber (0)
{
}

CUSBBulkOnlyMassStorageDevice::~CUSBBulkOnlyMassStorageDevice (void)
{
	if (m_nDeviceNumber != 0)
	{
		CDeviceNameService::Get ()->RemoveDevice (m_bCD ? "ucd" : "umsd",
							 m_nDeviceNumber, TRUE);

		if (m_bCD)
		{
			s_CDNumberPool.FreeNumber (m_nDeviceNumber);
		}
		else
		{
			s_DeviceNumberPool.FreeNumber (m_nDeviceNumber);
		}

		m_nDeviceNumber = 0;
	}

	delete m_pPartitionManager;
	m_pPartitionManager = 0;

	delete m_pEndpointOut;
	m_pEndpointOut =  0;
	
	delete m_pEndpointIn;
	m_pEndpointIn = 0;
}

boolean CUSBBulkOnlyMassStorageDevice::Configure (void)
{
	if (GetNumEndpoints () < 2)
	{
		ConfigurationError (FromUmsd);

		return FALSE;
	}

	const TUSBEndpointDescriptor *pEndpointDesc;
	while ((pEndpointDesc = (TUSBEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT)) != 0)
	{
		if ((pEndpointDesc->bmAttributes & 0x3F) == 0x02)		// Bulk
		{
			if ((pEndpointDesc->bEndpointAddress & 0x80) == 0x80)	// Input
			{
				if (m_pEndpointIn != 0)
				{
					ConfigurationError (FromUmsd);

					return FALSE;
				}

				m_pEndpointIn = new CUSBEndpoint (GetDevice (), pEndpointDesc);
			}
			else							// Output
			{
				if (m_pEndpointOut != 0)
				{
					ConfigurationError (FromUmsd);

					return FALSE;
				}

				m_pEndpointOut = new CUSBEndpoint (GetDevice (), pEndpointDesc);
			}
		}
	}

	if (   m_pEndpointIn  == 0
	    || m_pEndpointOut == 0)
	{
		ConfigurationError (FromUmsd);

		return FALSE;
	}

	if (!CUSBFunction::Configure ())
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Cannot set interface");

		return FALSE;
	}

	TSCSIInquiry SCSIInquiry;
	SCSIInquiry.OperationCode	  = SCSI_OP_INQUIRY;
	SCSIInquiry.LogicalUnitNumberEVPD = 0;
	SCSIInquiry.PageCode		  = 0;
	SCSIInquiry.Reserved		  = 0;
	SCSIInquiry.AllocationLength	  = sizeof (TSCSIInquiryResponse);
	SCSIInquiry.Control		  = SCSI_CONTROL;

	TSCSIInquiryResponse SCSIInquiryResponse;
	if (Command (&SCSIInquiry, sizeof SCSIInquiry,
		     &SCSIInquiryResponse, sizeof SCSIInquiryResponse,
		     TRUE) != (int) sizeof SCSIInquiryResponse)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Device does not respond");

		return FALSE;
	}

	m_bCD = SCSIInquiryResponse.PeripheralDeviceType == SCSI_PDT_CD_DVD;

	if (   !m_bCD
	    && SCSIInquiryResponse.PeripheralDeviceType != SCSI_PDT_DIRECT_ACCESS_BLOCK)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Unsupported device type: 0x%02X", (unsigned) SCSIInquiryResponse.PeripheralDeviceType);
		
		return FALSE;
	}

	/*
	 * A CD drive is registered whether or not there is a disc in it.
	 *
	 * Everything below - waiting for the unit to come ready, then READ
	 * CAPACITY - describes a disk that is always there.  A drive with an
	 * empty drawer never comes ready, so insisting on it means the drive
	 * appears only if a disc happened to be in it at power on, and a disc
	 * put in later does nothing at all.  The disc is looked for when a
	 * read wants it; see CheckMedia().
	 */
	if (m_bCD)
	{
		m_nBlockSize = 2048;
		m_nBlockShift = 11;

		unsigned nDeviceNumber = s_CDNumberPool.AllocateNumber (FALSE);
		if (nDeviceNumber == CNumberPool::Invalid)
		{
			CLogger::Get ()->Write (FromUmsd, LogError, "Too many devices");

			return FALSE;
		}

		assert (m_nDeviceNumber == 0);
		m_nDeviceNumber = nDeviceNumber;

		CString DeviceName;
		DeviceName.Format ("ucd%u", m_nDeviceNumber);

		/* No partition manager: a CD has no partition table, and asking
		   an empty drive for sector zero only makes it seek. */
		CDeviceNameService::Get ()->AddDevice (DeviceName, this, TRUE);

		CLogger::Get ()->Write (FromUmsd, LogNotice, "%s is a CD/DVD drive",
					(const char *) DeviceName);

		return TRUE;
	}

	unsigned nTries = 100;
	while (--nTries)
	{
		CTimer::Get ()->MsDelay (100);

		TSCSITestUnitReady SCSITestUnitReady;
		SCSITestUnitReady.OperationCode = SCSI_OP_TEST_UNIT_READY;
		SCSITestUnitReady.Reserved	= 0;
		SCSITestUnitReady.Control	= SCSI_CONTROL;

		if (Command (&SCSITestUnitReady, sizeof SCSITestUnitReady, 0, 0, FALSE) >= 0)
		{
			break;
		}

		TSCSIRequestSense SCSIRequestSense;
		SCSIRequestSense.OperationCode	  = SCSI_REQUEST_SENSE;
		SCSIRequestSense.DescriptorFormat = 0;
		SCSIRequestSense.Reserved1	  = 0;
		SCSIRequestSense.Reserved2	  = 0;
		SCSIRequestSense.AllocationLength = sizeof (TSCSIRequestSenseResponse7x);
		SCSIRequestSense.Control	  = SCSI_CONTROL;

		TSCSIRequestSenseResponse7x SCSIRequestSenseResponse7x;
		if (Command (&SCSIRequestSense, sizeof SCSIRequestSense,
			     &SCSIRequestSenseResponse7x, sizeof SCSIRequestSenseResponse7x,
			     TRUE) < 0)
		{
			CLogger::Get ()->Write (FromUmsd, LogError, "Request sense failed");

			return FALSE;
		}
	}

	if (nTries == 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Unit is not ready");

		return FALSE;
	}

	TSCSIReadCapacity10 SCSIReadCapacity;
	SCSIReadCapacity.OperationCode		= SCSI_OP_READ_CAPACITY10;
	SCSIReadCapacity.Obsolete		= 0;
	SCSIReadCapacity.Reserved1		= 0;
	SCSIReadCapacity.LogicalBlockAddress	= 0;
	SCSIReadCapacity.Reserved2		= 0;
	SCSIReadCapacity.PartialMediumIndicator	= 0;
	SCSIReadCapacity.Reserved3		= 0;
	SCSIReadCapacity.Control		= SCSI_CONTROL;

	TSCSIReadCapacityResponse SCSIReadCapacityResponse;
	if (Command (&SCSIReadCapacity, sizeof SCSIReadCapacity,
		     &SCSIReadCapacityResponse, sizeof SCSIReadCapacityResponse,
		     TRUE) != (int) sizeof SCSIReadCapacityResponse)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Read capacity failed");

		return FALSE;
	}

	unsigned nBlockSize = le2be32 (SCSIReadCapacityResponse.BlockLengthInBytes);
	if (nBlockSize != UMSD_BLOCK_SIZE)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Unsupported block size: %u", nBlockSize);

		return FALSE;
	}

	m_nBlockCount = le2be32 (SCSIReadCapacityResponse.ReturnedLogicalBlockAddress);
	if (m_nBlockCount == (u32) -1)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Unsupported disk size > 2TB");

		return FALSE;
	}

	m_nBlockCount++;

	CLogger::Get ()->Write (FromUmsd, LogDebug, "Capacity is %u MByte", m_nBlockCount / (0x100000 / UMSD_BLOCK_SIZE));

	unsigned nDeviceNumber = s_DeviceNumberPool.AllocateNumber (FALSE);
	if (nDeviceNumber == CNumberPool::Invalid)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Too many devices");

		return FALSE;
	}

	assert (m_nDeviceNumber == 0);
	m_nDeviceNumber = nDeviceNumber;

	CString DeviceName;
	DeviceName.Format ("umsd%u", m_nDeviceNumber);

	assert (m_pPartitionManager == 0);
	m_pPartitionManager = new CPartitionManager (this, DeviceName);
	assert (m_pPartitionManager != 0);
	if (!m_pPartitionManager->Initialize ())
	{
		s_DeviceNumberPool.FreeNumber (m_nDeviceNumber);
		m_nDeviceNumber = 0;

		return FALSE;
	}

	CDeviceNameService::Get ()->AddDevice (DeviceName, this, TRUE);
	
	return TRUE;
}

/*
 * Why the last command failed, in the drive's own words.
 *
 * A failed command leaves a condition on the device that only REQUEST SENSE
 * clears - so this is not only how the reason is learned, it is what lets the
 * next command through at all.  Linux calls it auto-sense and does it after
 * every failure.
 */
void CUSBBulkOnlyMassStorageDevice::RequestSense (void)
{
	m_ucASC = 0;
	m_ucASCQ = 0;

	TSCSIRequestSense SCSIRequestSense;
	SCSIRequestSense.OperationCode	  = SCSI_REQUEST_SENSE;
	SCSIRequestSense.DescriptorFormat = 0;
	SCSIRequestSense.Reserved1	  = 0;
	SCSIRequestSense.Reserved2	  = 0;
	SCSIRequestSense.AllocationLength = sizeof (TSCSIRequestSenseResponse7x);
	SCSIRequestSense.Control	  = SCSI_CONTROL;

	TSCSIRequestSenseResponse7x Sense;
	memset (&Sense, 0, sizeof Sense);

	if (Command (&SCSIRequestSense, sizeof SCSIRequestSense,
		     &Sense, sizeof Sense, TRUE) < 0)
	{
		return;
	}

	m_ucASC = Sense.AdditionalSenseCode;
	m_ucASCQ = Sense.AdditionalSenseCodeQualifier;

	if (m_ucASC == ASC_MEDIUM_CHANGED)
	{
		m_bMediumChanged = TRUE;
	}
}

/*
 * Is there a disc in the drawer, and how big is it?
 *
 * Asked by the first read rather than at configuration time, because a CD
 * drive is empty most of the time and a disc can be put in at any moment.
 * An empty drive costs one command to recognise and is then left alone for
 * half a second, so that a guest asking over and over does not make the
 * drive rattle; anything else is transient and worth retrying at once.
 */
boolean CUSBBulkOnlyMassStorageDevice::CheckMedia (void)
{
	if (   m_nBlockCount > 0
	    && !m_bRecheck)
	{
		return TRUE;
	}

	/*
	 * Asked at most four times a second while the answer is no.
	 *
	 * The guest asks again as fast as it likes - a driver that has just
	 * been told "not ready" retries in a tight loop - and answering each
	 * of those by going out to the drive is a machine that crawls while a
	 * disc spins up.  The last answer stands for a quarter of a second,
	 * which is far shorter than a spin-up and costs the guest nothing it
	 * would not have waited for anyway.
	 */
	unsigned nNow = CTimer::Get ()->GetTicks ();
	if (   m_nLastMediaCheck != 0
	    && nNow - m_nLastMediaCheck < HZ / 4)
	{
		return m_nBlockCount > 0 && !m_bRecheck;
	}
	m_nLastMediaCheck = nNow;

	// Up to four times: a drive that has just been given a disc answers
	// "powered on or reset" and "medium may have changed" before it
	// answers anything else, and both clear by being reported.
	boolean bReady = FALSE;
	for (unsigned nTry = 0; nTry < 4 && !bReady; nTry++)
	{
		TSCSITestUnitReady SCSITestUnitReady;
		SCSITestUnitReady.OperationCode = SCSI_OP_TEST_UNIT_READY;
		SCSITestUnitReady.Reserved	= 0;
		SCSITestUnitReady.Control	= SCSI_CONTROL;

		if (Command (&SCSITestUnitReady, sizeof SCSITestUnitReady, 0, 0, FALSE) >= 0)
		{
			bReady = TRUE;
			break;
		}

		RequestSense ();

		if (m_ucASC == ASC_NO_MEDIUM)
		{
			/* Really empty: forget what was in it. */
			m_nBlockCount = 0;
			m_bRecheck = FALSE;

			return FALSE;
		}

		if (m_ucASC == ASC_NOT_READY)
		{
			// Not waited out here.
			//
			// A drive that is spinning up answers "not ready" for a
			// second or more, and this used to sleep through it - inside
			// the guest's own read, so the machine was stopped and USB
			// was not being polled either.  On a disc change that was
			// long enough for Circle to drop the keyboard off the bus.
			// The guest is told what the drive said instead, and asks
			// again in its own time, which is what a real one would get.
			break;
		}
	}

	if (!bReady)
	{
		return FALSE;
	}

	TSCSIReadCapacity10 SCSIReadCapacity;
	SCSIReadCapacity.OperationCode		= SCSI_OP_READ_CAPACITY10;
	SCSIReadCapacity.Obsolete		= 0;
	SCSIReadCapacity.Reserved1		= 0;
	SCSIReadCapacity.LogicalBlockAddress	= 0;
	SCSIReadCapacity.Reserved2		= 0;
	SCSIReadCapacity.PartialMediumIndicator	= 0;
	SCSIReadCapacity.Reserved3		= 0;
	SCSIReadCapacity.Control		= SCSI_CONTROL;

	TSCSIReadCapacityResponse SCSIReadCapacityResponse;
	if (Command (&SCSIReadCapacity, sizeof SCSIReadCapacity,
		     &SCSIReadCapacityResponse, sizeof SCSIReadCapacityResponse,
		     TRUE) != (int) sizeof SCSIReadCapacityResponse)
	{
		RequestSense ();

		return FALSE;
	}

	unsigned nBlockSize = le2be32 (SCSIReadCapacityResponse.BlockLengthInBytes);
	unsigned nShift = 0;
	while ((1U << nShift) < nBlockSize)
	{
		nShift++;
	}

	if (   (1U << nShift) != nBlockSize
	    || nBlockSize < 512
	    || nBlockSize > 4096)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Unsupported block size: %u",
					nBlockSize);

		return FALSE;
	}

	unsigned nBlocks = le2be32 (SCSIReadCapacityResponse.ReturnedLogicalBlockAddress);
	if (nBlocks == (u32) -1)
	{
		return FALSE;
	}

	m_nBlockSize = nBlockSize;
	m_nBlockShift = nShift;
	m_nBlockCount = nBlocks + 1;
	m_bRecheck = FALSE;

	// Accounted for: the capacity just read is this disc's.
	m_bMediumChanged = FALSE;

	CLogger::Get ()->Write (FromUmsd, LogNotice,
				"the drive reads the disc as %u blocks of %u bytes",
				m_nBlockCount, m_nBlockSize);

	return TRUE;
}

/*
 * Has the disc been changed, or taken out?
 *
 * As with the floppy, there is no wire that says so: the drive is asked, at
 * most four times a second, and answers "medium may have changed" once per
 * change.  The capacity is then forgotten, because the next disc is a
 * different size, and the caller throws away what it had cached.
 */
boolean CUSBBulkOnlyMassStorageDevice::PollMedium (void)
{
	unsigned nNow = CTimer::Get ()->GetTicks ();
	if (   m_nLastPoll != 0
	    && nNow - m_nLastPoll < HZ / 4)
	{
		return FALSE;
	}
	m_nLastPoll = nNow;

	TSCSITestUnitReady SCSITestUnitReady;
	SCSITestUnitReady.OperationCode = SCSI_OP_TEST_UNIT_READY;
	SCSITestUnitReady.Reserved	= 0;
	SCSITestUnitReady.Control	= SCSI_CONTROL;

	if (Command (&SCSITestUnitReady, sizeof SCSITestUnitReady, 0, 0, FALSE) < 0)
	{
		RequestSense ();

		if (   m_ucASC == ASC_NO_MEDIUM
		    && m_nBlockCount != 0)
		{
			// Taken out, where there was one before.
			m_bMediumChanged = TRUE;
		}
	}

	if (!m_bMediumChanged)
	{
		return FALSE;
	}

	m_bMediumChanged = FALSE;
	/*
	 * The size is not thrown away here.
	 *
	 * It used to be, and then a drive that was still spinning up answered
	 * nothing at all for a second or two - during which GetSize() was zero
	 * and the ATAPI emulation told the guest there was no disc in the
	 * drive.  Windows 95 setup, reading from that drive, stops there.  The
	 * disc is there; what it is not yet is ready.  So the last known size
	 * stands until the drive says the drawer is empty, and reads fail in
	 * the meantime, which is what a guest knows how to retry.
	 */
	m_bRecheck = TRUE;
	m_nLastMediaCheck = 0;

	CLogger::Get ()->Write (FromUmsd, LogNotice, "the disc has been changed");

	return TRUE;
}

int CUSBBulkOnlyMassStorageDevice::Read (void *pBuffer, size_t nCount)
{
	if (   m_bCD
	    && !CheckMedia ())
	{
		return -1;
	}

	unsigned nTries = MAX_TRIES;

	int nResult;

	do
	{
		nResult = TryRead (pBuffer, nCount);

		if (nResult != (int) nCount)
		{
			/*
			 * A CD drive is not reset for this.
			 *
			 * Resetting the device after every refusal is right
			 * for a disk, where a refusal means something has gone
			 * wrong.  A CD drive refuses for ordinary reasons - it
			 * is still spinning up, the disc has just been put in,
			 * somebody opened the drawer - and a reset costs
			 * seconds and loses the medium.  Asking for sense both
			 * says which of those it is and clears the condition,
			 * which is all that is needed to make the retry work.
			 */
			if (m_bCD)
			{
				RequestSense ();

				if (   m_ucASC == ASC_NO_MEDIUM
				    || m_bMediumChanged)
				{
					m_nBlockCount = 0;

					return -1;
				}

				continue;
			}

			int nStatus = Reset ();
			if (nStatus != 0)
			{
				return nStatus;
			}
		}
	}
	while (   nResult != (int) nCount
	       && --nTries > 0);

	return nResult;
}

int CUSBBulkOnlyMassStorageDevice::Write (const void *pBuffer, size_t nCount)
{
	if (m_bCD)
	{
		// Burning is not what this driver is for, and a guest that
		// tries deserves a refusal rather than a stalled drive.
		return -1;
	}

	unsigned nTries = MAX_TRIES;

	int nResult;

	do
	{
		nResult = TryWrite (pBuffer, nCount);

		if (nResult != (int) nCount)
		{
			int nStatus = Reset ();
			if (nStatus != 0)
			{
				return nStatus;
			}
		}
	}
	while (   nResult != (int) nCount
	       && --nTries > 0);

	return nResult;
}

u64 CUSBBulkOnlyMassStorageDevice::Seek (u64 ullOffset)
{
	m_ullOffset = ullOffset;

	return m_ullOffset;
}

u64 CUSBBulkOnlyMassStorageDevice::GetSize (void) const
{
	// Zero when the drawer is empty, which is a thing a caller has to be
	// able to ask about rather than an assertion failure.
	assert (m_nBlockCount < (u32) -1);

	return (u64) m_nBlockCount << m_nBlockShift;
}

unsigned CUSBBulkOnlyMassStorageDevice::GetCapacity (void) const
{
	return m_nBlockCount;
}

int CUSBBulkOnlyMassStorageDevice::TryRead (void *pBuffer, size_t nCount)
{
	assert (pBuffer != 0);

	if (   (m_ullOffset & (m_nBlockSize - 1)) != 0
	    || m_ullOffset > UMSD_MAX_OFFSET)
	{
		return -1;
	}
	u32 nBlockAddress = (u32) (m_ullOffset >> m_nBlockShift);

	if ((nCount & (m_nBlockSize - 1)) != 0)
	{
		return -1;
	}
	u16 usTransferLength = (u16) (nCount >> m_nBlockShift);

	//CLogger::Get ()->Write (FromUmsd, LogDebug, "TryRead %u/0x%X/%u", nBlockAddress, (unsigned) pBuffer, (unsigned) usTransferLength);

	TSCSIRead10 SCSIRead;
	SCSIRead.OperationCode		= SCSI_OP_READ;
	SCSIRead.Reserved1		= 0;
	SCSIRead.LogicalBlockAddress	= le2be32 (nBlockAddress);
	SCSIRead.Reserved2		= 0;
	SCSIRead.TransferLength		= le2be16 (usTransferLength);
	SCSIRead.Control		= SCSI_CONTROL;

	if (Command (&SCSIRead, sizeof SCSIRead, pBuffer, nCount, TRUE) != (int) nCount)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "TryRead failed");

		return -1;
	}

	return nCount;
}

int CUSBBulkOnlyMassStorageDevice::TryWrite (const void *pBuffer, size_t nCount)
{
	assert (pBuffer != 0);

	if (   (m_ullOffset & (m_nBlockSize - 1)) != 0
	    || m_ullOffset > UMSD_MAX_OFFSET)
	{
		return -1;
	}
	u32 nBlockAddress = (u32) (m_ullOffset >> m_nBlockShift);

	if ((nCount & (m_nBlockSize - 1)) != 0)
	{
		return -1;
	}
	u16 usTransferLength = (u16) (nCount >> m_nBlockShift);

	//CLogger::Get ()->Write (FromUmsd, LogDebug, "TryWrite %u/0x%X/%u", nBlockAddress, (unsigned) pBuffer, (unsigned) usTransferLength);

	TSCSIWrite10 SCSIWrite;
	SCSIWrite.OperationCode		= SCSI_OP_WRITE;
	SCSIWrite.Flags			= SCSI_WRITE_FUA;
	SCSIWrite.LogicalBlockAddress	= le2be32 (nBlockAddress);
	SCSIWrite.Reserved		= 0;
	SCSIWrite.TransferLength	= le2be16 (usTransferLength);
	SCSIWrite.Control		= SCSI_CONTROL;

	if (Command (&SCSIWrite, sizeof SCSIWrite, (void *) pBuffer, nCount, FALSE) < 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "TryWrite failed");

		return -1;
	}

	return nCount;
}

int CUSBBulkOnlyMassStorageDevice::Command (void *pCmdBlk, size_t nCmdBlkLen,
					    void *pBuffer, size_t nBufLen, boolean bIn)
{
	assert (pCmdBlk != 0);
	assert (6 <= nCmdBlkLen && nCmdBlkLen <= 16);
	assert (nBufLen == 0 || pBuffer != 0);

	DMA_BUFFER (u8, CBWBuffer, sizeof (TCBW));
	TCBW *pCBW = (TCBW *) CBWBuffer;
	memset (pCBW, 0, sizeof *pCBW);

	pCBW->dCWBSignature	     = CBWSIGNATURE;
	pCBW->dCWBTag		     = ++m_nCWBTag;
	pCBW->dCBWDataTransferLength = nBufLen;
	pCBW->bmCBWFlags	     = bIn ? CBWFLAGS_DATA_IN : 0;
	pCBW->bCBWLUN		     = CBWLUN;
	pCBW->bCBWCBLength	     = (u8) nCmdBlkLen;

	memcpy (pCBW->CBWCB, pCmdBlk, nCmdBlkLen);

	CUSBHostController *pHost = GetHost ();
	assert (pHost != 0);

	if (pHost->Transfer (m_pEndpointOut, pCBW, sizeof *pCBW) < 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "CBW transfer failed");

		return -1;
	}

	int nResult = 0;
	
	if (nBufLen > 0)
	{
		assert (pBuffer != 0);

		u8 *pDMABuffer = 0;
		if (!IS_CACHE_ALIGNED (pBuffer, nBufLen))
		{
			pDMABuffer = new (HEAP_DMA30) u8[nBufLen];
			assert (pDMABuffer != 0);

			if (!bIn)
			{
				memcpy (pDMABuffer, pBuffer, nBufLen);
			}
		}

		nResult = pHost->Transfer (bIn ? m_pEndpointIn : m_pEndpointOut,
					   pDMABuffer != 0 ? pDMABuffer : pBuffer, nBufLen);
		if (nResult < 0)
		{
			CLogger::Get ()->Write (FromUmsd, LogError, "Data transfer failed");

			delete [] pDMABuffer;

			return -1;
		}

		if (pDMABuffer != 0)
		{
			if (bIn)
			{
				memcpy (pBuffer, pDMABuffer, nBufLen);
			}

			delete [] pDMABuffer;
		}
	}

	DMA_BUFFER (u8, CSWBuffer, sizeof (TCSW));
	TCSW *pCSW = (TCSW *) CSWBuffer;

	if (pHost->Transfer (m_pEndpointIn, pCSW, sizeof *pCSW) != (int) sizeof *pCSW)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "CSW transfer failed");

		if (pHost->ControlMessage (GetEndpoint0 (),
					   REQUEST_TO_ENDPOINT | REQUEST_OUT, CLEAR_FEATURE,
					   ENDPOINT_HALT, m_pEndpointIn->GetNumber () | 0x80,
					   0, 0) < 0)
		{
			CLogger::Get ()->Write (FromUmsd, LogDebug,
						"Cannot clear halt on endpoint IN");

			return -1;
		}

		m_pEndpointIn->ResetPID ();

		if (pHost->Transfer (m_pEndpointIn, pCSW, sizeof *pCSW) != (int) sizeof *pCSW)
		{
			CLogger::Get ()->Write (FromUmsd, LogError, "CSW transfer failed twice");

			return -1;
		}
	}

	if (pCSW->dCSWSignature != CSWSIGNATURE)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "CSW signature is wrong");

		return -1;
	}

	if (pCSW->dCSWTag != m_nCWBTag)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "CSW tag is wrong");

		return -1;
	}

	if (pCSW->bCSWStatus != CSWSTATUS_PASSED)
	{
		return -1;
	}

	if (pCSW->dCSWDataResidue != 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogError, "Data residue is not 0");

		return -1;
	}

	return nResult;
}

int CUSBBulkOnlyMassStorageDevice::Reset (void)
{
	CUSBHostController *pHost = GetHost ();
	assert (pHost != 0);
	
	if (pHost->ControlMessage (GetEndpoint0 (),
				   REQUEST_CLASS | REQUEST_TO_INTERFACE | REQUEST_OUT,
				   BULK_ONLY_MASS_STORAGE_RESET, 0, GetInterfaceNumber (), 0, 0) < 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogDebug, "Cannot reset device");

		return -1;
	}

	CTimer::Get ()->MsDelay (100);

	if (pHost->ControlMessage (GetEndpoint0 (),
				   REQUEST_TO_ENDPOINT | REQUEST_OUT, CLEAR_FEATURE,
				   ENDPOINT_HALT, m_pEndpointIn->GetNumber () | 0x80, 0, 0) < 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogDebug, "Cannot clear halt on endpoint IN");

		return -1;
	}

	if (pHost->ControlMessage (GetEndpoint0 (),
				   REQUEST_TO_ENDPOINT | REQUEST_OUT, CLEAR_FEATURE,
				   ENDPOINT_HALT, m_pEndpointOut->GetNumber (), 0, 0) < 0)
	{
		CLogger::Get ()->Write (FromUmsd, LogDebug, "Cannot clear halt on endpoint OUT");

		return -1;
	}

	m_pEndpointIn->ResetPID ();
	m_pEndpointOut->ResetPID ();

	return 0;
}
