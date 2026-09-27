//
// usbfloppydevice.cpp
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
#include <circle/usb/usbfloppydevice.h>
#include <circle/usb/usbrequest.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <circle/macros.h>
#include <circle/new.h>
#include <assert.h>

#define BLOCK_SIZE		512
#define BLOCK_MASK		(BLOCK_SIZE-1)
#define BLOCK_SHIFT		9

#define MAX_OFFSET		0x1FFFFFFFFFFULL	// 2TB

#define MAX_TRIES		5			// max. attempts

// USB Mass Storage Control/Bulk/Interrupt (CBI) Transport

// Command Block Status
//
// For bInterfaceSubClass 04h - which is what a UFI floppy drive is -
// the two interrupt bytes are not a status at all.  CBI 1.1, 3.4.3.1.3:
// "The first byte the device returns contains a device-specific ASC
// code and the second byte contains a device-specific ASCQ code."  So
// every failed command says why it failed, at no cost, and the driver
// does not have to guess or go asking with REQUEST SENSE - which is
// what made an empty drive move its heads until it could be heard.
#define STATUS_PASS			0x00
#define STATUS_FAIL			0x01
#define STATUS_PHASE_ERROR		0x02
#define STATUS_PERSISTENT_FAILURE	0x03

// The additional sense codes this driver acts on, from the UFI Command
// Specification.  Anything else is simply an error.
#define ASC_NO_MEDIUM		0x3A	// nothing in the drive
#define ASC_NOT_READY		0x04	// there is, and it is coming up
#define ASC_MEDIUM_CHANGED	0x28	// a different diskette
#define ASC_RESET		0x29	// powered on, or reset, since last asked
#define ASC_INCOMPATIBLE	0x30	// a diskette this drive will not read
#define ASC_UNKNOWN			0xFF	// a device that does not say

// UFI Command Set

struct TSCSIInquiry
{
	u8		OperationCode,
#define SCSI_OP_INQUIRY		0x12
			LogicalUnitNumberEVPD,
			PageCode,
			Reserved1,
			AllocationLength,
			Reserved2[7];
}
PACKED;

struct TSCSIInquiryResponse
{
	u8		PeripheralDeviceType	: 5,
#define SCSI_PDT_DIRECT_ACCESS_FLOPPY	0x00			// Direct access device (floppy)
#define SCSI_PDT_NONE			0x1F			// No FDD connected to this unit
			Reserved1		: 3,
			Reserved2		: 7,
			RMB			: 1,		// 1: removable media
			ANSIApprovedVersion	: 3,		// 0
			ECMAVersion		: 3,		// 0
			ISOVersion		: 2,		// 0
			ResponseDataFormat	: 4,		// 1: UFI device
			Reserved3		: 4,
			AdditionalLength,			// 31
			Reserved4[3],
			VendorIdentification[8],
			ProductIdentification[16],
			ProductRevisionLevel[4];
}
PACKED;

struct TSCSITestUnitReady
{
	u8		OperationCode,
#define SCSI_OP_TEST_UNIT_READY		0x00
			Reserved1		: 5,
			LogicalUnitNumber	: 3,
			Reserved2[10];
}
PACKED;

struct TSCSIRequestSense
{
	u8		OperationCode,
#define SCSI_REQUEST_SENSE		0x03
			Reserved1		: 5,
			LogicalUnitNumber	: 3,
			Reserved2[2],
			AllocationLength,
			Reserved3[7];
}
PACKED;

struct TSCSIRequestSenseResponse
{
	u8		ErrorCode		: 7,		// 0x70
			Valid			: 1,
			Reserved1,
			SenseKey		: 4,
#define SCSI_SENSE_KEY_NOT_READY	0x02
			Reserved2		: 4,
			Information[4],				// big endian
			AdditionalSenseLength,			// 10
			Reserved3[4],
			AdditionalSenseCode,
			AdditionalSenseCodeQualifier,
			Reserved4[4];
}
PACKED;

/*
 * READ FORMAT CAPACITIES, and why a floppy drive needs it.
 *
 * This is the command a UFI drive expects the host to use to find out what
 * is in it, and it is the one this driver never sent.  A diskette has no way
 * of saying how it is formatted; the drive works that out by reading it, and
 * this command is what makes it do so and report the answer.  Without it the
 * drive is left guessing, and a drive that is guessing refuses a read of a
 * 1.44M diskette with "incompatible medium installed" - sense key 3, asc 30 -
 * on a diskette that is perfectly good and reads anywhere else.
 *
 * UFI Command Specification, 1.7.  The answer is a four byte header whose
 * last byte counts the bytes of descriptors, then descriptors of eight bytes:
 * the block count, a code saying whether the drive found a format, and the
 * block length.
 */
struct TSCSIReadFormatCapacities
{
	u8		OperationCode,
#define SCSI_OP_READ_FORMAT_CAPACITIES	0x23
			Reserved1		: 5,
			LogicalUnitNumber	: 3,
			Reserved2[5];
	u16		AllocationLength;			// big endian
	u8		Reserved3[3];
}
PACKED;

struct TSCSIFormatCapacitiesResponse
{
	u8		Reserved1[3],
			CapacityListLength;			// bytes of descriptors
	u32		NumberOfBlocks;				// big endian
	u8		DescriptorCode		: 2,
#define UFI_FORMAT_UNFORMATTED		0x01
#define UFI_FORMAT_FORMATTED		0x02
#define UFI_FORMAT_NO_MEDIUM		0x03
			Reserved2		: 6,
			BlockLength[3];				// big endian
}
PACKED;

struct TSCSIReadCapacity
{
	u8		OperationCode,
#define SCSI_OP_READ_CAPACITY		0x25
			RelAdr			: 1,
			Reserved1		: 4,
			LogicalUnitNumber	: 3;
	u32		LogicalBlockAddress;			// set to 0
	u16		Reserved2;
	u8		PartialMediumIndicator	: 1,		// set to 0
			Reserved3		: 7,
			Reserved4[3];
}
PACKED;

struct TSCSIReadCapacityResponse
{
	u32		LastLogicalBlockAddress;		// big endian
	u32		BlockLengthInBytes;			// big endian
}
PACKED;

struct TSCSIRead10
{
	u8		OperationCode,
#define SCSI_OP_READ		0x28
			RelAdr			: 1,
			Reserved1		: 2,
			FUA			: 1,
			DPO			: 1,
			LogicalUnitNumber	: 3;
	u32		LogicalBlockAddress;			// big endian
	u8		Reserved2;
	u16		TransferLength;				// block count, big endian
	u8		Reserved3[3];
}
PACKED;

struct TSCSIWrite10
{
	u8		OperationCode,
#define SCSI_OP_WRITE		0x2A
			RelAdr			: 1,
			Reserved1		: 2,
			FUA			: 1,
			DPO			: 1,
			LogicalUnitNumber	: 3;
	u32		LogicalBlockAddress;			// big endian
	u8		Reserved2;
	u16		TransferLength;				// block count, big endian
	u8		Reserved3[3];
}
PACKED;

struct TSCSISendDiagnostic
{
	u8		OperationCode,
#define SCSI_OP_SEND_DIAGNOSTIC	0x1D
			UnitOfl			: 1,
			DefOfl			: 1,
			SelfTest		: 1,
			Reserved1		: 1,
			PF			: 1,
			LogicalUnitNumber	: 3,
			Reserved2[10];
}
PACKED;

CNumberPool CUSBFloppyDiskDevice::s_DeviceNumberPool (1);

LOGMODULE ("ufd");

static const char DevicePrefix[] = "ufd";

CUSBFloppyDiskDevice::CUSBFloppyDiskDevice (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEndpointIn (0),
	m_pEndpointOut (0),
	m_pEndpointInterrupt (0),
	m_nBlockCount (0),
	m_ucASC (0),
	m_ucASCQ (0),
	m_bMediumChanged (FALSE),
	m_ucLastComplaint (0xFE),
	m_nRefusedCommands (0),
	m_bLimbered (FALSE),
	m_bClearingFailure (FALSE),
	m_nLastMediaCheck (0),
	m_nLastPoll (0),
	m_ullOffset (0),
	m_nDeviceNumber (0)
{
}

CUSBFloppyDiskDevice::~CUSBFloppyDiskDevice (void)
{
	if (m_nDeviceNumber != 0)
	{
		CDeviceNameService::Get ()->RemoveDevice (DevicePrefix, m_nDeviceNumber, TRUE);

		s_DeviceNumberPool.FreeNumber (m_nDeviceNumber);

		m_nDeviceNumber = 0;
	}
	delete m_pEndpointInterrupt;
	m_pEndpointInterrupt = 0;

	delete m_pEndpointOut;
	m_pEndpointOut =  0;
	
	delete m_pEndpointIn;
	m_pEndpointIn = 0;
}

boolean CUSBFloppyDiskDevice::Configure (void)
{
	if (GetNumEndpoints () < 2)
	{
		ConfigurationError (From);

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
					ConfigurationError (From);

					return FALSE;
				}

				m_pEndpointIn = new CUSBEndpoint (GetDevice (), pEndpointDesc);
			}
			else							// Output
			{
				if (m_pEndpointOut != 0)
				{
					ConfigurationError (From);

					return FALSE;
				}

				m_pEndpointOut = new CUSBEndpoint (GetDevice (), pEndpointDesc);
			}
		}
		else if ((pEndpointDesc->bmAttributes & 0x3F) == 0x03)		// Interrupt
		{
			if ((pEndpointDesc->bEndpointAddress & 0x80) == 0x80)	// Input
			{
				if (m_pEndpointInterrupt != 0)
				{
					ConfigurationError (From);

					return FALSE;
				}

				m_pEndpointInterrupt = new CUSBEndpoint (GetDevice (),
									 pEndpointDesc);
			}
		}
	}

	if (   m_pEndpointIn == 0
	    || m_pEndpointOut == 0
	    || (   GetInterfaceProtocol () == 0
	        && m_pEndpointInterrupt == 0))
	{
		ConfigurationError (From);

		return FALSE;
	}

	if (!CUSBFunction::Configure ())
	{
		LOGERR ("Cannot set interface");

		return FALSE;
	}

	TSCSIInquiry SCSIInquiry;
	memset (&SCSIInquiry, 0, sizeof SCSIInquiry);
	SCSIInquiry.OperationCode = SCSI_OP_INQUIRY;
	SCSIInquiry.AllocationLength = sizeof (TSCSIInquiryResponse);

	TSCSIInquiryResponse SCSIInquiryResponse;
	if (Command (&SCSIInquiry, sizeof SCSIInquiry,
		     &SCSIInquiryResponse, sizeof SCSIInquiryResponse,
		     TRUE) != (int) sizeof SCSIInquiryResponse)
	{
		LOGERR ("Device does not respond");

		return FALSE;
	}

	if (SCSIInquiryResponse.PeripheralDeviceType != SCSI_PDT_DIRECT_ACCESS_FLOPPY)
	{
		LOGERR ("Unsupported device type: 0x%02X",
			(unsigned) SCSIInquiryResponse.PeripheralDeviceType);
		
		return FALSE;
	}

	// Registered whether or not there is a diskette in it, and without
	// looking for one.
	//
	// This used to require media: WaitUnitReady() and READ CAPACITY had
	// to succeed first, so a drive that was empty when the machine
	// started never appeared at all and a diskette put in later did
	// nothing.  It also used to look, which is worse than useless - an
	// empty drive answers a readiness test by moving its heads, and a
	// drive nobody has asked for anything should be silent.  The media
	// is looked for when a read or a write wants it, and not before.

	unsigned nDeviceNumber = s_DeviceNumberPool.AllocateNumber (FALSE);
	if (nDeviceNumber == CNumberPool::Invalid)
	{
		LOGERR ("Too many devices");

		return FALSE;
	}

	assert (m_nDeviceNumber == 0);
	m_nDeviceNumber = nDeviceNumber;

	CDeviceNameService::Get ()->AddDevice (DevicePrefix, m_nDeviceNumber, this, TRUE);
	
	return TRUE;
}

/*
 * What Linux does after any failed command, and what this driver did not.
 *
 * A CBI device reports a failed command by stalling - the control pipe if it
 * will not take the command block, the data pipe if the command itself
 * fails - and the failure sticks to the device until the host asks for
 * sense.  drivers/usb/storage/transport.c calls that auto-sense and does it
 * for every failure; the CBI specification calls the condition a Persistent
 * Command Block Failure and says the host "shall send an op 03h REQUEST
 * SENSE command block".  Without it the drive refuses everything that
 * follows, which is a floppy drive that has apparently died and that only
 * unplugging appears to revive.
 *
 * The answer is not thrown away.  For most failures UFI has already put the
 * reason in m_ucASC, because the interrupt status carries it - but not for a
 * command that failed in its data phase, which is reported by a stall and
 * whose status is deliberately never read (see Command()).  For those this is
 * the only account of what went wrong, so it is kept.
 */
void CUSBFloppyDiskDevice::ClearFailure (u8 ucOpCode)
{
	if (m_bClearingFailure)
	{
		return;
	}
	m_bClearingFailure = TRUE;

	// Kept, because REQUEST SENSE has a status of its own and UFI says
	// that its status does not describe the command that failed.
	u8 ucASC = m_ucASC;
	u8 ucASCQ = m_ucASCQ;

	TSCSIRequestSense SCSIRequestSense;
	memset (&SCSIRequestSense, 0, sizeof SCSIRequestSense);
	SCSIRequestSense.OperationCode = SCSI_REQUEST_SENSE;
	SCSIRequestSense.AllocationLength = sizeof (TSCSIRequestSenseResponse);

	TSCSIRequestSenseResponse Response;
	memset (&Response, 0, sizeof Response);
	int nResult = Command (&SCSIRequestSense, sizeof SCSIRequestSense,
			       &Response, sizeof Response, TRUE);

	if (   ucASC == ASC_UNKNOWN
	    && nResult == (int) sizeof Response
	    && (Response.ErrorCode & 0x7E) == 0x70)
	{
		// The command that failed said nothing for itself, so what the
		// drive says here is all there is.
		ucASC = Response.AdditionalSenseCode;
		ucASCQ = Response.AdditionalSenseCodeQualifier;
	}

	m_ucASC = ucASC;
	m_ucASCQ = ucASCQ;

	// Once per reason.  A drive with no diskette in it is asked over and
	// over by a BIOS looking for something to boot, and the answer is the
	// same every time; it is worth one line, not thousands.
	if (m_ucASC != m_ucLastComplaint)
	{
		m_ucLastComplaint = m_ucASC;

		LOGWARN ("%02X failed: sense key %u, asc %02X qualifier %02X",
			 (unsigned) ucOpCode, (unsigned) Response.SenseKey,
			 (unsigned) m_ucASC, (unsigned) m_ucASCQ);
	}

	m_bClearingFailure = FALSE;
}

boolean CUSBFloppyDiskDevice::MediumChanged (void)
{
	boolean bChanged = m_bMediumChanged;
	m_bMediumChanged = FALSE;

	return bChanged;
}

/*
 * Has the diskette been swapped for another one?
 *
 * A drive on a cable tells the controller over a wire - the change line goes
 * true the moment the door opens and stays true until the head steps - and
 * the controller passes that to the guest, which then knows to re-read the
 * directory.  A USB drive has no such wire.  The only way to find out is to
 * send it a command and see whether it answers "medium may have changed",
 * which it does exactly once per change, to whoever asks first.
 *
 * So it is asked here, on the caller's ordinary reads, four times a second at
 * most: any more and the guest pays for it, any less and a diskette could be
 * swapped and read before anyone noticed.  A drive that has gone empty counts
 * as a change too - the diskette that was cached is in somebody's hand.
 *
 * The capacity is forgotten, because the new diskette need not be the size of
 * the old one, and the head is no longer known to be limber: a diskette that
 * has just been put in has never been stepped across.  The next read
 * establishes both again.
 */
boolean CUSBFloppyDiskDevice::PollMedium (void)
{
	unsigned nNow = CTimer::Get ()->GetTicks ();
	if (   m_nLastPoll != 0
	    && nNow - m_nLastPoll < HZ / 4)
	{
		return FALSE;
	}
	m_nLastPoll = nNow;

	TSCSITestUnitReady SCSITestUnitReady;
	memset (&SCSITestUnitReady, 0, sizeof SCSITestUnitReady);
	SCSITestUnitReady.OperationCode = SCSI_OP_TEST_UNIT_READY;

	if (Command (&SCSITestUnitReady, sizeof SCSITestUnitReady, 0, 0, FALSE) < 0)
	{
		switch (m_ucASC)
		{
		case ASC_MEDIUM_CHANGED:
			m_bMediumChanged = TRUE;
			break;

		case ASC_NO_MEDIUM:
			// An empty drive is only news if it was not empty
			// before: the diskette that was cached is in
			// somebody's hand.
			if (m_nBlockCount != 0)
			{
				m_bMediumChanged = TRUE;
			}
			break;

		default:
			// The drive's own business - a motor coming up, a
			// reset it has just reported, a command that failed
			// on the wire.  None of them is a new diskette.
			break;
		}
	}

	// Asked this way round so that a change the drive mentioned to some
	// other command is picked up here too; MediumChanged() clears it.
	if (!MediumChanged ())
	{
		return FALSE;
	}

	m_nBlockCount = 0;
	m_bLimbered = FALSE;
	m_ucLastComplaint = 0xFE;

	LOGNOTE ("the diskette has been changed");

	return TRUE;
}

boolean CUSBFloppyDiskDevice::CheckMedia (void)
{
	if (m_nBlockCount > 0)
	{
		return TRUE;
	}

	// Throttled only while the drive is known to be empty.
	//
	// An empty drive costs one command to recognise - it answers 3A -
	// and there is no point asking again for a while.  Every other
	// failure is transient: a drive whose motor is coming up says so,
	// and the guest is retrying precisely because it expects that to
	// pass.  Throttling those meant DOS's first read failed, every
	// Retry fell inside the half second and returned without asking
	// the drive anything at all, and the diskette looked unreadable.
	unsigned nNow = CTimer::Get ()->GetTicks ();
	if (   m_ucASC == ASC_NO_MEDIUM
	    && m_nLastMediaCheck != 0
	    && nNow - m_nLastMediaCheck < HZ / 2)
	{
		return FALSE;
	}
	m_nLastMediaCheck = nNow;

	if (WaitUnitReady () < 0)
	{
		// Said once per reason, so that a drive nobody has put a
		// diskette in does not fill the log with the same line.
		if (m_ucASC != m_ucLastComplaint)
		{
			m_ucLastComplaint = m_ucASC;

			LOGNOTE ("%s (asc %02X qualifier %02X)",
				 m_ucASC == ASC_NO_MEDIUM ? "no diskette in the drive"
								 : "not ready",
				 (unsigned) m_ucASC, (unsigned) m_ucASCQ);
		}

		return FALSE;
	}
	m_ucLastComplaint = 0xFE;

	// First, what the drive makes of the diskette.
	//
	// READ CAPACITY alone is not enough: it answers from whatever the drive
	// currently believes, and after a reset or a change of diskette it
	// believes nothing in particular.  This is the command that makes it
	// look, and until it has looked it will refuse to read - which is the
	// "incompatible medium" this driver used to hit on a good diskette.
	TSCSIReadFormatCapacities SCSIFormatCapacities;
	memset (&SCSIFormatCapacities, 0, sizeof SCSIFormatCapacities);
	SCSIFormatCapacities.OperationCode = SCSI_OP_READ_FORMAT_CAPACITIES;
	SCSIFormatCapacities.AllocationLength =
		le2be16 (sizeof (TSCSIFormatCapacitiesResponse));

	TSCSIFormatCapacitiesResponse Formats;
	memset (&Formats, 0, sizeof Formats);
	if (Command (&SCSIFormatCapacities, sizeof SCSIFormatCapacities,
		     &Formats, sizeof Formats, TRUE) == (int) sizeof Formats)
	{
		if (Formats.DescriptorCode == UFI_FORMAT_NO_MEDIUM)
		{
			m_ucASC = ASC_NO_MEDIUM;

			return FALSE;
		}

		LOGNOTE ("the drive reads the diskette as %u blocks, %s",
			 le2be32 (Formats.NumberOfBlocks),
			 Formats.DescriptorCode == UFI_FORMAT_FORMATTED
				? "formatted" : "unformatted");
	}

	TSCSIReadCapacity SCSIReadCapacity;
	memset (&SCSIReadCapacity, 0, sizeof SCSIReadCapacity);
	SCSIReadCapacity.OperationCode = SCSI_OP_READ_CAPACITY;

	TSCSIReadCapacityResponse SCSIReadCapacityResponse;
	unsigned nTries;
	for (nTries = MAX_TRIES; nTries; nTries--)
	{
		if (Command (&SCSIReadCapacity, sizeof SCSIReadCapacity,
			     &SCSIReadCapacityResponse,
			     sizeof SCSIReadCapacityResponse,
			     TRUE) == (int) sizeof SCSIReadCapacityResponse)
		{
			break;
		}
	}

	if (nTries == 0)
	{
		return FALSE;
	}

	if (le2be32 (SCSIReadCapacityResponse.BlockLengthInBytes) != BLOCK_SIZE)
	{
		LOGERR ("Unsupported block size");

		return FALSE;
	}

	unsigned nBlocks = le2be32 (SCSIReadCapacityResponse.LastLogicalBlockAddress);
	if (nBlocks == (u32) -1)
	{
		return FALSE;
	}

	m_nBlockCount = nBlocks + 1;

	// Whatever change was outstanding has been accounted for: the capacity
	// just read is this diskette's.  Without this the unit attention the
	// drive reports at power on - "medium may have changed", perfectly
	// true and already acted on here - was still waiting for PollMedium(),
	// which took it for a swap and made the machine establish the medium
	// all over again a quarter of a second into the guest's first read.
	MediumChanged ();

	LOGNOTE ("Capacity is %u KBytes", m_nBlockCount / (0x400 / BLOCK_SIZE));

	// Once here as well, so that the head is already moving before the BIOS
	// asks for the boot sector rather than after it has given up on it.
	if (!m_bLimbered)
	{
		m_bLimbered = TRUE;

		LimberUp ();
	}

	return TRUE;
}

/*
 * Get the head moving, before the guest asks for anything.
 *
 * This drive comes up with its head where it left it and will not move it on
 * demand: a read of any other track fails, and it reports that as sense key 3,
 * "incompatible medium installed", which sounds like a verdict on the diskette
 * and is not one.  It said so on a diskette it had just described, correctly,
 * as 2880 blocks and formatted; the sectors under the head read perfectly and
 * every other place on the diskette failed, on the nose, every time - the
 * readable window was exactly one cylinder wide.
 *
 * What frees it is being asked repeatedly.  So it is asked here, alternately
 * at the two ends of the diskette, until both answer - at which point the head
 * is demonstrably crossing the whole of it.  Doing it while the medium is
 * being established means it is over before the BIOS reads the boot sector;
 * left to happen by itself it took a failed boot and a keypress, which is what
 * "Replace and press any key" was really fixing.
 *
 * Three seconds at the outside.  Every attempt runs inside the guest's own
 * execution, so this is time the emulated machine is stopped, and a drive that
 * will not come right in three seconds will not come right at all.
 */
void CUSBFloppyDiskDevice::LimberUp (void)
{
	static const u32 Ends[] = { 0, 1440 };

	const u64 ullSave = m_ullOffset;
	const unsigned nBegan = CTimer::Get ()->GetTicks ();

	DMA_BUFFER (u8, Sector, BLOCK_SIZE);

	unsigned nTries = 0;
	unsigned nInARow = 0;
	unsigned nAt = 0;

	while (   nInARow < 2
	       && CTimer::Get ()->GetTicks () - nBegan < 3*HZ)
	{
		m_ullOffset = (u64) Ends[nAt] << BLOCK_SHIFT;
		nTries++;

		if (TryRead (Sector, BLOCK_SIZE) == BLOCK_SIZE)
		{
			nInARow++;
			nAt ^= 1;
		}
		else
		{
			nInARow = 0;
		}
	}

	m_ullOffset = ullSave;

	if (nTries > 1)
	{
		LOGNOTE ("the head took %u attempts to start moving%s", nTries,
			 nInARow < 2 ? ", and has not" : "");
	}
}

int CUSBFloppyDiskDevice::Read (void *pBuffer, size_t nCount)
{
	// Not before every transfer.
	//
	// WaitUnitReady() is a loop of TEST UNIT READY and REQUEST SENSE,
	// and a drive that answers "not ready" a few times before it
	// settles makes that loop cost two tenths of a second - on every
	// single sector, which is a diskette read in ten minutes and a DOS
	// that never finishes booting.  The data was never the problem: a
	// sweep of all 2880 sectors refused none of them.  So read first
	// and ask afterwards, which is only the failing path paying for it.
	if (!CheckMedia ())
	{
		return -1;
	}

	int nResult;
	unsigned nTries = MAX_TRIES;
	boolean bLimberedNow = FALSE;

	do
	{
		nResult = TryRead (pBuffer, nCount);

		if (nResult != (int) nCount)
		{
			// How much this drive will read in one command.
			//
			// It says "incompatible medium installed" - sense key
			// 3, asc 30 - for a read it will not do, which sounds
			// like a verdict on the diskette and is not one: the
			// same drive answers READ FORMAT CAPACITIES on the same
			// diskette with "2880 blocks, formatted" in between the
			// refusals.  What it is objecting to is the length of
			// the transfer, and how much it will take depends on
			// what it has been doing - after the motor has stopped
			// it wants them one at a time.
			//
			// So the complaint is taken as what it is, and the
			// transfer is cut down until the drive accepts it.
			// Nothing is assumed about where the limit is; it is
			// found once and remembered, and a request bigger than
			// that is split by the loop above this one.
			// The head has stuck, not the diskette gone unreadable;
			// see LimberUp().  It sticks again whenever the motor
			// has been stopped for a while, so this is not
			// something that can be done once at the start and
			// forgotten - it belongs here, where the drive has just
			// said so.  Once per request, because it costs the
			// guest real time and five of them would be absurd.
			if (   m_ucASC == ASC_INCOMPATIBLE
			    && !bLimberedNow)
			{
				bLimberedNow = TRUE;

				LimberUp ();

				continue;
			}

			// No SEND DIAGNOSTIC here.  It was sent to put the drive
			// straight, but a drive that has just refused a command
			// refuses that one too - so the recovery was itself a
			// failure, and the log filled with stalls that this
			// driver had caused.  Linux does not send it either:
			// after a failed command it asks for sense, which has
			// already happened inside Command(), and reissues the
			// command.  That is what the loop below now does.

			// Once, on the first failure only.  A drive whose motor has
			// stopped fails the first read and succeeds on the next, so
			// without this the first DIR after a pause always errored and
			// only Retry worked.  Once and no more: every attempt blocks
			// the emulator mid-instruction, and a read that takes seconds
			// to fail is a machine that has frozen to anyone using it.
			if (nTries == MAX_TRIES)
			{
				WaitUnitReady ();
			}
		}
	}
	while (   nResult != (int) nCount
	       && --nTries > 0);

	return nResult;
}

int CUSBFloppyDiskDevice::Write (const void *pBuffer, size_t nCount)
{
	// Not before every transfer.
	//
	// WaitUnitReady() is a loop of TEST UNIT READY and REQUEST SENSE,
	// and a drive that answers "not ready" a few times before it
	// settles makes that loop cost two tenths of a second - on every
	// single sector, which is a diskette read in ten minutes and a DOS
	// that never finishes booting.  The data was never the problem: a
	// sweep of all 2880 sectors refused none of them.  So read first
	// and ask afterwards, which is only the failing path paying for it.
	if (!CheckMedia ())
	{
		return -1;
	}

	int nResult;
	unsigned nTries = MAX_TRIES;

	do
	{
		nResult = TryWrite (pBuffer, nCount);

		if (nResult != (int) nCount)
		{
			// No SEND DIAGNOSTIC here.  It was sent to put the drive
			// straight, but a drive that has just refused a command
			// refuses that one too - so the recovery was itself a
			// failure, and the log filled with stalls that this
			// driver had caused.  Linux does not send it either:
			// after a failed command it asks for sense, which has
			// already happened inside Command(), and reissues the
			// command.  That is what the loop below now does.

			// Once, on the first failure only.  A drive whose motor has
			// stopped fails the first read and succeeds on the next, so
			// without this the first DIR after a pause always errored and
			// only Retry worked.  Once and no more: every attempt blocks
			// the emulator mid-instruction, and a read that takes seconds
			// to fail is a machine that has frozen to anyone using it.
			if (nTries == MAX_TRIES)
			{
				WaitUnitReady ();
			}
		}
	}
	while (   nResult != (int) nCount
	       && --nTries > 0);

	return nResult;
}

u64 CUSBFloppyDiskDevice::Seek (u64 ullOffset)
{
	m_ullOffset = ullOffset;

	return m_ullOffset;
}

u64 CUSBFloppyDiskDevice::GetSize (void) const
{
	// Zero when the drive is empty, which is a thing a caller has to be
	// able to ask about rather than an assertion failure.
	return (u64) m_nBlockCount << BLOCK_SHIFT;
}

/*
 * Wait for the drive, and know the difference between waiting and hoping.
 *
 * Every command that fails leaves its reason in m_ucASC; see the note by
 * STATUS_PASS.  That is what makes this cheap and quiet:
 *
 *   3A  there is no diskette.  Say so at once.  Nothing is retried, nothing
 *       is told to spin up - telling an empty drive to start is what drives
 *       its heads into the end stop, over and over, for as long as the BIOS
 *       keeps asking for drive A.
 *   04  there is one and it is coming up to speed.  Wait for it, and nudge
 *       the motor once, which is the one case where that is the right thing.
 *   28  the diskette has been changed.  That is not a failure: the next
 *       command will work, and the caller wants to know so it can drop
 *       whatever it had cached.
 *
 * Returns 0 when the drive is ready, or -1, with m_ucASC saying why.
 */
int CUSBFloppyDiskDevice::WaitUnitReady (void)
{
	boolean bAskedToStart = FALSE;
	unsigned nTicks = CTimer::Get ()->GetTicks ();

	while (CTimer::Get ()->GetTicks () - nTicks < 2*HZ)
	{
		TSCSITestUnitReady SCSITestUnitReady;
		memset (&SCSITestUnitReady, 0, sizeof SCSITestUnitReady);
		SCSITestUnitReady.OperationCode = SCSI_OP_TEST_UNIT_READY;

		if (Command (&SCSITestUnitReady, sizeof SCSITestUnitReady,
			     0, 0, FALSE) >= 0)
		{
			return 0;
		}

		switch (m_ucASC)
		{
		case ASC_NO_MEDIUM:
			return -1;

		case ASC_MEDIUM_CHANGED:
			m_bMediumChanged = TRUE;
			continue;

		case ASC_RESET:
			// The drive has been powered on or reset since anyone
			// last spoke to it.  It reports that once, to whoever
			// asks first, and the condition is gone by saying so -
			// so the only right response is to ask again.
			continue;

		case ASC_NOT_READY:
			if (!bAskedToStart)
			{
				bAskedToStart = TRUE;

				u8 StartStop[12];
				memset (StartStop, 0, sizeof StartStop);
				StartStop[0] = 0x1B;	// START STOP UNIT
				StartStop[4] = 0x01;	// start, do not eject
				Command (StartStop, sizeof StartStop, 0, 0, FALSE);
			}

			CTimer::Get ()->MsDelay (20);
			continue;

		default:
			// Nothing the drive said: the command failed on the wire
			// rather than in the device, so there is no condition to
			// wait out.  One more try for a drive that has just been
			// attached, and then leave it - the caller will ask again
			// soon enough, and a guest that says Retry deserves an
			// answer in less time than it takes to notice.
			if (CTimer::Get ()->GetTicks () - nTicks > HZ / 10)
			{
				return -1;
			}

			CTimer::Get ()->MsDelay (10);
			continue;
		}
	}

	return -1;
}

int CUSBFloppyDiskDevice::TryRead (void *pBuffer, size_t nCount)
{
	assert (pBuffer != 0);

	if (   (m_ullOffset & BLOCK_MASK) != 0
	    || m_ullOffset > MAX_OFFSET)
	{
		return -1;
	}
	u32 nBlockAddress = (u32) (m_ullOffset >> BLOCK_SHIFT);

	if ((nCount & BLOCK_MASK) != 0)
	{
		return -1;
	}
	u16 usTransferLength = (u16) (nCount >> BLOCK_SHIFT);

	// LOGDBG ("TryRead %u/0x%lX/%u", nBlockAddress, (uintptr) pBuffer, (unsigned) usTransferLength);

	TSCSIRead10 SCSIRead;
	memset (&SCSIRead, 0, sizeof SCSIRead);
	SCSIRead.OperationCode		= SCSI_OP_READ;
	SCSIRead.LogicalBlockAddress	= le2be32 (nBlockAddress);
	SCSIRead.TransferLength		= le2be16 (usTransferLength);

	if (Command (&SCSIRead, sizeof SCSIRead, pBuffer, nCount, TRUE) != (int) nCount)
	{
		LOGERR ("TryRead failed");

		return -1;
	}

	return nCount;
}

int CUSBFloppyDiskDevice::TryWrite (const void *pBuffer, size_t nCount)
{
	assert (pBuffer != 0);

	if (   (m_ullOffset & BLOCK_MASK) != 0
	    || m_ullOffset > MAX_OFFSET)
	{
		return -1;
	}
	u32 nBlockAddress = (u32) (m_ullOffset >> BLOCK_SHIFT);

	if ((nCount & BLOCK_MASK) != 0)
	{
		return -1;
	}
	u16 usTransferLength = (u16) (nCount >> BLOCK_SHIFT);

	// LOGDBG ("TryWrite %u/0x%lX/%u", nBlockAddress, (uintptr) pBuffer, (unsigned) usTransferLength);

	TSCSIWrite10 SCSIWrite;
	memset (&SCSIWrite, 0, sizeof SCSIWrite);
	SCSIWrite.OperationCode		= SCSI_OP_WRITE;
	SCSIWrite.LogicalBlockAddress	= le2be32 (nBlockAddress);
	SCSIWrite.TransferLength	= le2be16 (usTransferLength);

	if (Command (&SCSIWrite, sizeof SCSIWrite, (void *) pBuffer, nCount, FALSE) < 0)
	{
		LOGERR ("TryWrite failed");

		return -1;
	}

	return nCount;
}

// Only so that a log line reads as something other than a number.
static const char *USBErrorName (TUSBError Error)
{
	static const char *Names[] =
	{
		"stall", "transaction", "babble", "frame overrun", "data toggle",
		"host bus", "split", "timeout", "aborted", "unknown"
	};

	return Error <= USBErrorUnknown ? Names[Error] : "unknown";
}

int CUSBFloppyDiskDevice::Command (void *pCmdBlk, size_t nCmdBlkLen,
				   void *pBuffer, size_t nBufLen, boolean bIn)
{
	assert (pCmdBlk != 0);
	assert (nCmdBlkLen == 12);
	assert (nBufLen == 0 || pBuffer != 0);

	DMA_BUFFER (u8, CmdBuffer, nCmdBlkLen);
	memcpy (CmdBuffer, pCmdBlk, nCmdBlkLen);

	CUSBHostController *pHost = GetHost ();
	assert (pHost != 0);

	// Cleared first, so that a command which fails before the status
	// phase cannot be read as whatever the last one reported.
	m_ucASC = ASC_UNKNOWN;
	m_ucASCQ = 0;

	// The command block travels as a class request on endpoint zero, which
	// CBI 1.1 calls Accept Device-Specific Command.  It is sent here rather
	// than through ControlMessage() for one reason: what to do when it
	// fails depends entirely on how it failed, and ControlMessage() throws
	// that away.
	//
	//   A stall is the device talking.  It is refusing the command, which
	//   the specification calls a Persistent Command Block Failure, and the
	//   only thing that lifts it is being asked for sense: "In response to
	//   a Persistent Command Block Failure, the host shall send an op 03h
	//   REQUEST SENSE command block."  Until that is sent the device
	//   refuses everything - a drive that has apparently stopped listening
	//   and that nothing but unplugging appears to revive.
	//
	//   Anything else happened on the wire and the device knows nothing
	//   about it, so asking it for sense is beside the point.  This drive
	//   is full speed behind a high speed hub, so every transfer to it is a
	//   split one and the hub holds a buffer for it; a split that ends in
	//   an error leaves that buffer occupied, and then every SETUP that
	//   follows fails in the same way for ever.  USB 2.0 11.24.2.3 has the
	//   remedy and Linux's hub driver applies it in exactly this case:
	//   clear the translator's buffer, then send the block again.
	boolean bSent = FALSE;
	TUSBError Error = USBErrorUnknown;

	for (unsigned nTry = 0; nTry < 2; nTry++)
	{
		TSetupData *pSetup = new TSetupData;
		assert (pSetup != 0);
		pSetup->bmRequestType = REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE;
		pSetup->bRequest      = 0;			// ADSC
		pSetup->wValue	      = 0;
		pSetup->wIndex	      = GetInterfaceNumber ();
		pSetup->wLength	      = nCmdBlkLen;

		CUSBRequest URB (GetEndpoint0 (), CmdBuffer, nCmdBlkLen, pSetup);
		bSent = pHost->SubmitBlockingRequest (&URB);
		if (!bSent)
		{
			Error = URB.GetUSBError ();
			GetEndpoint0 ()->ResetPID ();
		}

		delete pSetup;

		if (   bSent
		    || Error == USBErrorStall)
		{
			break;
		}

		GetDevice ()->ClearTTBuffer (0, FALSE, EndpointTypeControl);
	}

	if (!bSent)
	{
		if (m_nRefusedCommands++ == 0)
		{
			LOGWARN ("the drive would not take command %02X (%s)",
				 (unsigned) *(u8 *) pCmdBlk, USBErrorName (Error));
		}

		if (Error == USBErrorStall)
		{
			ClearFailure (*(u8 *) pCmdBlk);
		}

		return -1;
	}

	m_nRefusedCommands = 0;

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
			if (m_ucLastComplaint != 0xFC)
			{
				m_ucLastComplaint = 0xFC;
				LOGWARN ("command %02X failed in its data phase",
					 (unsigned) *(u8 *) pCmdBlk);
			}

			// A CBI device reports a failed command by stalling the
			// endpoint, and a stalled endpoint stays stalled until the
			// host clears it - so without this the first failure
			// poisons the endpoint and every read after it fails the
			// same way.  The translator's buffer goes with it: this
			// drive is full speed behind a high speed hub, so the
			// transfer was a split one and the hub holds a buffer for
			// it which a stall leaves occupied.
			CUSBEndpoint *pEndpoint = bIn ? m_pEndpointIn : m_pEndpointOut;

			boolean bTT = GetDevice ()->ClearTTBuffer (
					pEndpoint->GetNumber (), bIn, EndpointTypeBulk);
			boolean bHalt = pHost->ControlMessage (GetEndpoint0 (),
					   REQUEST_TO_ENDPOINT | REQUEST_OUT, CLEAR_FEATURE,
					   ENDPOINT_HALT,
					   pEndpoint->GetNumber () | (bIn ? 0x80 : 0), 0, 0) >= 0;
			if (bHalt)
			{
				pEndpoint->ResetPID ();
			}

			static unsigned nClearSaid;
			if (nClearSaid < 8)
			{
				nClearSaid++;

				LOGWARN ("after %02X: translator %s, halt %s",
					 (unsigned) *(u8 *) pCmdBlk,
					 bTT ? "cleared" : "NOT cleared",
					 bHalt ? "cleared" : "NOT cleared");
			}

			delete [] pDMABuffer;
			pDMABuffer = 0;

			// Not a return.
			//
			// A stall on the data endpoint IS how a CBI device reports
			// that the command failed, and the reason follows on the
			// interrupt endpoint - for a floppy, as an ASC and an ASCQ.
			// Returning here threw that away, so every failure looked
			// alike and the driver had to guess: an empty drive, one
			// still coming up to speed and a diskette that had been
			// swapped were indistinguishable.  The status is collected
			// below and the failure is reported afterwards.
			// Linux returns here without reading the interrupt status, and
			// so does this: the status belongs to a command that failed, the
			// reason is asked for with REQUEST SENSE, and reading a status
			// nobody will use only risks taking the next command's instead.
			ClearFailure (*(u8 *) pCmdBlk);

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

	if (GetInterfaceProtocol () == 0)
	{
		DMA_BUFFER (u8, Status, 2);

		assert (m_pEndpointInterrupt != 0);
		if (pHost->Transfer (m_pEndpointInterrupt, Status, 2) != 2)
		{
			// The command's fate is now unknown, so nothing is
			// assumed about it.  The translator buffer is cleared
			// for the same reason as above: this endpoint is
			// reached by a split too, and one left occupied stops
			// the status of every later command from arriving.
			GetDevice ()->ClearTTBuffer (m_pEndpointInterrupt->GetNumber (),
						     TRUE, EndpointTypeInterrupt);

			if (m_ucLastComplaint != 0xFD)
			{
				m_ucLastComplaint = 0xFD;
				LOGWARN ("no status came back for command %02X",
					 (unsigned) *(u8 *) pCmdBlk);
			}

			return -1;
		}

		if (GetInterfaceSubClass () == 0x04)
		{
			const u8 ucOpCode = *(u8 *) pCmdBlk;

			// Two commands whose status is not about them.
			//
			// On a UFI device REQUEST SENSE and INQUIRY do not
			// disturb the sense data, so what comes back on the
			// interrupt endpoint after them still describes the
			// command that failed before.  Linux says so in
			// transport.c and acts on it: "REQUEST_SENSE and
			// INQUIRY don't affect the sense data on UFI devices,
			// so we ignore the information for those commands."
			//
			// Reading it as their own status is what made this
			// driver blind.  Every failure was followed by REQUEST
			// SENSE, the drive answered it perfectly - sense key 6,
			// asc 29, power on or reset - and the driver threw the
			// answer away because the status bytes repeated the
			// original complaint and made the command look failed.
			// So nothing ever learned why anything failed: the
			// reason came back as "unknown" and a drive that only
			// needed the same command sent twice looked dead.
			if (   ucOpCode == SCSI_REQUEST_SENSE
			    || ucOpCode == SCSI_OP_INQUIRY)
			{
				return nResult;
			}

			// ASC and ASCQ; see the note by STATUS_PASS.
			m_ucASC = Status[0];
			m_ucASCQ = Status[1];

			if (Status[0] != 0)
			{
				return -1;
			}

			return nResult;
		}

		m_ucASC = Status[0] != 0 ? ASC_UNKNOWN : 0;
		m_ucASCQ = 0;

		if (   Status[0] != 0
		    || Status[1] != STATUS_PASS)
		{
			return -1;
		}
	}

	return nResult;
}

int CUSBFloppyDiskDevice::Reset (void)
{
	TSCSISendDiagnostic SCSISendDiagnostic;
	memset (&SCSISendDiagnostic, 0, sizeof SCSISendDiagnostic);
	SCSISendDiagnostic.OperationCode = SCSI_OP_SEND_DIAGNOSTIC;
	SCSISendDiagnostic.SelfTest = 1;

	return Command (&SCSISendDiagnostic, sizeof SCSISendDiagnostic, 0, 0, FALSE);
}
