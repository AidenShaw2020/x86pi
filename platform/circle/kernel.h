// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#ifdef TINY386_SMOKE
#include "smoke/bios_probe.h"
#endif

class CKernel {
public:
    CKernel();
    boolean Initialize();
    void Run();
private:
#ifdef TINY386_SMOKE
    bool ProbeRuntime(Tiny386BiosProbe *probe);
#else
    bool ProbeRuntime();
#endif
    // Construction order matters for Circle singleton services.
    CActLED m_LED;
    CDeviceNameService m_Devices;
    CScreenDevice m_Screen;
    CSerialDevice m_Serial;
    CExceptionHandler m_Exceptions;
    CInterruptSystem m_Interrupt;
    CTimer m_Timer;
    CLogger m_Log;
    CEMMCDevice m_SD;
    FATFS m_FS;
};
