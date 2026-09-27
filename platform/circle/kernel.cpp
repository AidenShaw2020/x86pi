// SPDX-License-Identifier: GPL-3.0-or-later
#include "kernel.h"
#include <circle/util.h>
#if AARCH != 64 || RASPPI != 3 || DEPTH != 32
#error Build Circle and this kernel with AARCH=64 RASPPI=3 and DEPTH=32
#endif

#ifdef TINY386_SMOKE
extern "C" int tiny386_pm_smoke(void);
extern "C" int tiny386_real_smoke(void);
#endif

CKernel::CKernel()
: m_Screen(1920, 1080), m_Timer(&m_Interrupt),
  m_Log(LogDebug, &m_Timer), m_SD(&m_Interrupt, &m_Timer, &m_LED),
  m_FS{} {}

boolean CKernel::Initialize()
{
    // Circle's PL011 driver owns GPIO14/15. BOOTDIAG used mini-UART.
    // Keep BOOTDIAG intact: this is a separate kernel, not a replacement driver.
    if (!m_Serial.Initialize(115200)) return FALSE;
    if (!m_Log.Initialize(&m_Serial)) return FALSE;
    m_Log.Write("BOOT", LogNotice, "tiny386 Circle infrastructure v1 (no guest/JIT)");
    if (!m_Interrupt.Initialize() || !m_Timer.Initialize()) {
        m_Log.Write("CIRCLE", LogError, "Interrupt/timer initialization failed");
        return FALSE;
    }
    if (!m_Screen.Initialize()) {
        m_Log.Write("VIDEO", LogError, "Framebuffer initialization failed");
        return FALSE;
    }
    m_Log.Write("VIDEO", LogNotice, "Framebuffer %ux%u depth=%u",
                m_Screen.GetWidth(), m_Screen.GetHeight(), DEPTH);
    const char banner[] = "tiny386 / Circle infrastructure v1\n"
#ifdef TINY386_SMOKE
                          "AArch64 - x86 interpreter smoke - disk probe read-only\n";
#else
                          "AArch64 - guest CPU not started - disk probe read-only\n";
#endif
    m_Screen.Write(banner, sizeof banner - 1);
    return TRUE;
}

bool CKernel::ProbeRuntime(
#ifdef TINY386_SMOKE
    Tiny386BiosProbe *probe
#endif
)
{
    if (!m_SD.Initialize()) {
        m_Log.Write("SD", LogError, "On-board SD initialization failed");
        return false;
    }
    FRESULT result = f_mount(&m_FS, "SD:", 1);
    if (result != FR_OK) {
        m_Log.Write("FAT", LogError, "Mount SD: failed (%u)", unsigned(result));
        return false;
    }
#ifdef TINY386_SMOKE
    /* bios_probe owns only read operations after the volume is mounted. */
    if (!probe) {
        f_mount(nullptr, "SD:", 0);
        return false;
    }
    const int probeResult = tiny386_bios_probe(probe);
    const FRESULT unmountResult = f_mount(nullptr, "SD:", 0);
    if (unmountResult != FR_OK) {
        m_Log.Write("FAT", LogError, "Unmount SD: failed (%u)",
                    unsigned(unmountResult));
        return false;
    }
    return probeResult == 0;
#else
    /* Infrastructure target keeps the original bounded, read-only config
     * probe; BIOS loading is deliberately smoke-only at this milestone. */
    FIL file;
    result = f_open(&file, "SD:/386/config.ini", FA_READ);
    if (result != FR_OK) {
        m_Log.Write("FAT", LogError, "Open /386/config.ini failed (%u)", unsigned(result));
        f_mount(nullptr, "SD:", 0);
        return false;
    }
    const auto size = f_size(&file);
    unsigned char buffer[256];
    UINT count = 0;
    result = f_read(&file, buffer, sizeof buffer, &count);
    const FRESULT closeResult = f_close(&file);
    const FRESULT unmountResult = f_mount(nullptr, "SD:", 0);
    if (result != FR_OK || closeResult != FR_OK || unmountResult != FR_OK) {
        m_Log.Write("FAT", LogError, "Read/close/unmount failed (%u/%u/%u)",
                    unsigned(result), unsigned(closeResult), unsigned(unmountResult));
        return false;
    }
    m_Log.Write("FAT", LogNotice, "/386/config.ini size=%llu, read=%u (not parsed yet)",
                (unsigned long long)size, unsigned(count));
    return count != 0;
#endif
}

void CKernel::Run()
{
#ifdef TINY386_SMOKE
    Tiny386BiosProbe biosProbe{};
    const bool ok = ProbeRuntime(&biosProbe);
#else
    const bool ok = ProbeRuntime();
#endif
    m_Log.Write("BOOT", ok ? LogNotice : LogError,
                "Infrastructure probe %s; no BIOS instructions beyond reset probe/audio started",
                ok ? "PASS" : "FAIL");
#ifdef TINY386_SMOKE
    m_Log.Write("BIOS", biosProbe.config_complete ? LogNotice : LogError,
                "config size=%u bios=%s value=%s status=%d",
                unsigned(biosProbe.config_size), biosProbe.bios_path,
                biosProbe.bios_value, biosProbe.config_status);
    m_Log.Write("BIOS", biosProbe.load_complete ? LogNotice : LogError,
                "load %s size=%u base=%08x end=%08x crc32=%08x last16="
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x status=%d",
                biosProbe.bios_path, unsigned(biosProbe.bios_size),
                unsigned(biosProbe.bios_base), unsigned(biosProbe.bios_end),
                unsigned(biosProbe.bios_crc32),
                biosProbe.bios_last16[0], biosProbe.bios_last16[1],
                biosProbe.bios_last16[2], biosProbe.bios_last16[3],
                biosProbe.bios_last16[4], biosProbe.bios_last16[5],
                biosProbe.bios_last16[6], biosProbe.bios_last16[7],
                biosProbe.bios_last16[8], biosProbe.bios_last16[9],
                biosProbe.bios_last16[10], biosProbe.bios_last16[11],
                biosProbe.bios_last16[12], biosProbe.bios_last16[13],
                biosProbe.bios_last16[14], biosProbe.bios_last16[15],
                biosProbe.load_status);
    m_Log.Write("BIOS", biosProbe.probe_complete ? LogNotice : LogError,
                "reset status=%d linear=%08x vector=%s %04x:%04x target=%08x "
                "target=%s step=%s skipped=%s",
                biosProbe.probe_complete, unsigned(biosProbe.reset.reset_linear),
                biosProbe.reset.vector_ok ? "EA" : "not-EA",
                biosProbe.reset.vector_cs, biosProbe.reset.vector_ip,
                unsigned(biosProbe.reset.target_linear),
                biosProbe.reset.target_ok ? "OK" : "FAIL",
                biosProbe.reset.step_done ? "DONE" : "NO",
                biosProbe.reset.step_done ? "NO" : "YES");
#endif
#ifdef TINY386_SMOKE
    const int pmResult = tiny386_pm_smoke();
    m_Log.Write("CPU", pmResult == 0 ? LogNotice : LogError,
                "Protected-mode arithmetic/loop/RAM/HLT smoke %s (%d); no BIOS/x87/JIT",
                pmResult == 0 ? "PASS" : "FAIL", pmResult);
    const int realResult = tiny386_real_smoke();
    m_Log.Write("CPU", realResult == 0 ? LogNotice : LogError,
                "Real-mode reset-vector/segments/stack/loop/HLT smoke %s (%d); no BIOS/x87/JIT",
                realResult == 0 ? "PASS" : "FAIL", realResult);
#endif
    // Bounded initialization, then a live heartbeat suitable for halt/resume.
    for (unsigned tick = 0; ; ++tick) {
        m_LED.On();
        m_Timer.MsDelay(50);
        m_LED.Off();
        m_Timer.MsDelay(950);
        if (tick % 5 == 0) m_Log.Write("BOOT", LogNotice, "alive %u", tick);
    }
}
