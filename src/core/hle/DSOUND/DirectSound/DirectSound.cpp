// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *  (c) 2017 blueshogun96
// *  (c) 2017-2020 RadWolfie
// *
// *  All rights reserved
// *
// ******************************************************************
#define LOG_PREFIX CXBXR_MODULE::DSOUND

// prevent name collisions
namespace xboxkrnl {
    #include <xboxkrnl/xboxkrnl.h>
};

#include <dsound.h>
#include "DirectSoundGlobal.hpp" // Global variables

#include "Logging.h"
#include "DirectSoundLogging.hpp"
#include "..\XbDSoundLogging.hpp"

#include "DSStream_PacketManager.hpp"

#include <mmreg.h>
#include <msacm.h>
#include <process.h>
#include <clocale>

// Temporary APU Timer Functions
// TODO: Move these to LLE APUDevice once we have one!

#define APU_TIMER_FREQUENCY	48000
LARGE_INTEGER APUInitialPerformanceCounter;
double NativeToXboxAPU_FactorForPerformanceFrequency = 0;

void ResetApuTimer()
{
	// Measure current host performance counter and frequency
	QueryPerformanceCounter(&APUInitialPerformanceCounter);
	NativeToXboxAPU_FactorForPerformanceFrequency = (double)APU_TIMER_FREQUENCY / APUInitialPerformanceCounter.QuadPart;
}

uint32_t GetAPUTime()
{
	::LARGE_INTEGER PerformanceCounter;
	QueryPerformanceCounter(&PerformanceCounter);

	// Re-Base on the time DirectSoundCreate was called
	PerformanceCounter.QuadPart -= APUInitialPerformanceCounter.QuadPart;
	// Apply a delta to make it appear to tick at 48khz
	PerformanceCounter.QuadPart = (ULONGLONG)(NativeToXboxAPU_FactorForPerformanceFrequency * PerformanceCounter.QuadPart);
	return (DWORD)PerformanceCounter.QuadPart;
}


// TODO: Tasks need to do for DirectSound HLE
// * Need create patches
//   * Ac97CreateMediaObject (Need OOVPA)
//   - WmaCreateDecoder (Need OOVPA, not require) Test case: WMAStream sample
//   - WmaCreateDecoderEx (Is just a forward to WmaCreateDecoder, nothing else)
//   - WmaCreateInMemoryDecoder (Need OOVPA, not require) Test case: WMAInMemory sample
//   - WmaCreateInMemoryDecoderEx (Is just a forward to WmaCreateInMemoryDecoder, nothing else)
//   * XWmaDecoderCreateMediaObject (Need OOVPA)
// * Missing IDirectSound patch
//   * IDirectSound_Compact (xbox doesn't use this method and cannot make oovpa for it.)
//   * IDirectSound_GetTime (not require)
//   * IDirectSound_MapBufferData (undocument)
//   * IDirectSound_SetCooperativeLevel (xbox doesn't use this method and cannot make oovpa for it.)
//   * IDirectSound_UnmapBufferData (undocument)
//   * IDirectSound_QueryInterfaceC (not require)
//   * IDirectSound_QueryInterface (not require)

// TODO: Tasks need to do for DirectSound LLE
// * Need to map range of 0x0fe800000 to 0xfe87ffff (?) and 0x0fec00000 to 0xfec00fff (?)

/* NOTE: SUCCEEDED define is only checking for is equal or greater than zero value.
    And FAILED check for less than zero value. Since DS_OK is only 0 base on DirectSound documentation,
    there is chance of failure which contain value greater than 0.
 */

// Managed memory xbox audio variables
static std::thread dsound_thread;
static void dsound_thread_worker(LPVOID);

#include "DirectSoundInline.hpp"

#ifdef __cplusplus
extern "C" {
#endif

void CxbxInitAudio()
{
    g_EmuShared->GetAudioSettings(&g_XBAudio);
}

#ifdef __cplusplus
}
#endif

// ******************************************************************
// * patch: DirectSoundCreate
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(DirectSoundCreate)
(
    LPVOID          pguidDeviceId,
    OUT LPDIRECTSOUND8* ppDirectSound,
    LPUNKNOWN       pUnknown)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pguidDeviceId)
		LOG_FUNC_ARG_OUT(ppDirectSound)
		LOG_FUNC_ARG(pUnknown)
		LOG_FUNC_END;

    static bool initialized = false;

    HRESULT hRet = DS_OK;

    if (!initialized) {
        dsound_thread = std::thread(dsound_thread_worker, nullptr);
    }

	ResetApuTimer();

    // Set this flag when this function is called
    g_bDSoundCreateCalled = TRUE;

    if (!initialized || g_pDSound8 == nullptr) {
        hRet = DirectSoundCreate8(&g_XBAudio.adapterGUID, &g_pDSound8, nullptr);

        LPCSTR dsErrorMsg = nullptr;

        switch (hRet) {
            case DS_OK:
                // Is not a fatal error.
                break;
            case DSERR_ALLOCATED:
                dsErrorMsg = "Audio adapter is already allocated. Possible fault within Cxbx-Reloaded's emulator."
                            "\n\nPlease report to respective game compatibility issue.";
                break;
            case DSERR_INVALIDPARAM:
                dsErrorMsg = "DirectSoundCreate8 return invalid parameter."
                            "\n\nPlease report to respective game compatibility issue.";
                break;
            case DSERR_NOAGGREGATION:
                dsErrorMsg = "Audio adapter does not support aggregation."
                            "\n\nPlease use different audio adapter.";
                break;
            case DSERR_NODRIVER:
                dsErrorMsg = "Please select a valid audio adapter from Cxbx-Reloaded's config audio dialog."
                            "\n\nThen try again.";
                break;
            case DSERR_OUTOFMEMORY:
                dsErrorMsg = "Unable to allocate DirectSound subsystem class."
                            "\n\nPlease close any opened application(s) or restart computer before trying again.";
                break;
            default:
                dsErrorMsg = "DirectSoundCreate8 unknown failed: 0x%08X";
        }

        if (dsErrorMsg != nullptr) {
            CxbxKrnlCleanup(dsErrorMsg, hRet);
        }

        hRet = g_pDSound8->SetCooperativeLevel(GET_FRONT_WINDOW_HANDLE, DSSCL_PRIORITY);

        if (hRet != DS_OK) {
            CxbxKrnlCleanup("g_pDSound8->SetCooperativeLevel Failed!");
        }

        // clear sound buffer cache
        vector_ds_buffer::iterator ppDSBuffer = g_pDSoundBufferCache.begin();
        for (; ppDSBuffer != g_pDSoundBufferCache.end();) {
            while (XTL::EMUPATCH(IDirectSoundBuffer_Release)((*ppDSBuffer)) != 0) {};
            ppDSBuffer = g_pDSoundBufferCache.begin();
        }
        g_pDSoundBufferCache.reserve(X_DIRECTSOUND_CACHE_MAX);

        // clear sound stream cache
        vector_ds_stream::iterator ppDSStream = g_pDSoundStreamCache.begin();
        for (; ppDSStream != g_pDSoundStreamCache.end();) {
            while (XTL::EMUPATCH(CDirectSoundStream_Release)((*ppDSStream)) != 0);
            ppDSStream = g_pDSoundStreamCache.begin();
        }
        g_pDSoundStreamCache.reserve(X_DIRECTSOUND_CACHE_MAX);

        //Create Primary Buffer in order for Xbox's DirectSound to manage complete control of it.
        DSBUFFERDESC bufferDesc = { 0 };
        bufferDesc.dwSize = sizeof(DSBUFFERDESC);
        bufferDesc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D; //DSBCAPS_CTRLFX is not supported on primary buffer.
        bufferDesc.guid3DAlgorithm = GUID_NULL;

        hRet = g_pDSound8->CreateSoundBuffer(&bufferDesc, &g_pDSoundPrimaryBuffer, nullptr);

        if (hRet != DS_OK) {
            CxbxKrnlCleanup("Creating primary buffer for DirectSound Failed!");
        }

        /* Quote from MDSN "For the primary buffer, you must use the
         * IDirectSoundBuffer interface; IDirectSoundBuffer8 is not available."
         */
        // Return E_NOINTERFACE from QueryInterface method, make sense for g_pDSoundPrimaryBuffer
        // But how to set DSBCAPS_CTRLFX on primary buffer or should it be set for all current and future cache buffers?
        // We need LPDIRECTSOUNDFXI3DL2REVERB8 / IID_IDirectSoundFXI3DL2Reverb8 or use LPDIRECTSOUNDBUFFER8 / IID_IDirectSoundBuffer8

        hRet = g_pDSoundPrimaryBuffer->QueryInterface(IID_IDirectSound3DListener8, (LPVOID*)&g_pDSoundPrimary3DListener8);

        if (hRet != DS_OK) {
            CxbxKrnlCleanup("Creating primary 3D Listener for DirectSound Failed!");
        }

        initialized = true;
    }

    // This way we can be sure that this function returns a valid
    // DirectSound8 pointer even if we initialized it elsewhere!
    if (ppDirectSound != nullptr) {
        *ppDirectSound = g_pDSound8;
    }

    RETURN_RESULT_CHECK(hRet);
}

// ******************************************************************
// * patch: IDirectSound_AddRef
// ******************************************************************
ULONG WINAPI XTL::EMUPATCH(IDirectSound_AddRef)
(
    LPDIRECTSOUND8          pThis)
{
    return 1; // TODO: Fix me!

    DSoundMutexGuardLock;

	LOG_FUNC_ONE_ARG(pThis);

    ULONG uRet = g_pDSound8->AddRef();

    return uRet;
}

// ******************************************************************
// * patch: IDirectSound_Release
// ******************************************************************
ULONG WINAPI XTL::EMUPATCH(IDirectSound_Release)
(
    LPDIRECTSOUND8          pThis)
{
    return 0; // TODO: Fix me!

    DSoundMutexGuardLock;

	LOG_FUNC_ONE_ARG(pThis);

    ULONG uRet = g_pDSound8->Release();
    if (uRet == 0) {
        g_bDSoundCreateCalled = false;
        g_pDSound8 = nullptr;
    }

    return uRet;
}

// ******************************************************************
// * patch: DirectSoundGetSampleTime
// ******************************************************************
DWORD WINAPI XTL::EMUPATCH(DirectSoundGetSampleTime)()
{
    DSoundMutexGuardLock;

	LOG_FUNC();

	DWORD dwRet = GetAPUTime();

    return dwRet;
}

// ******************************************************************
// * patch: DirectSoundUseFullHRTF
// ******************************************************************
void WINAPI XTL::EMUPATCH(DirectSoundUseFullHRTF)
(
    void)
{
    //NOTE: enter/leave criticalsection is not required! Titles are calling it before DirectSoundCreate.

	LOG_FUNC();

    LOG_IGNORED();
}

// ******************************************************************
// * patch: DirectSoundUseLightHRTF
// ******************************************************************
void WINAPI XTL::EMUPATCH(DirectSoundUseLightHRTF)
(
    void)
{
    //NOTE: enter/leave criticalsection is not required! Titles are calling it before DirectSoundCreate.

	LOG_FUNC();

    LOG_IGNORED();
}

// ******************************************************************
// * patch: DirectSoundUseFullHRTF4Channel
// ******************************************************************
void WINAPI XTL::EMUPATCH(DirectSoundUseFullHRTF4Channel)
(
    void)
{
    //NOTE: enter/leave criticalsection is not required! Titles are calling it before DirectSoundCreate.

	LOG_FUNC();

    LOG_IGNORED();
}

// ******************************************************************
// * patch: DirectSoundUseLightHRTF4Channel
// ******************************************************************
void WINAPI XTL::EMUPATCH(DirectSoundUseLightHRTF4Channel)
(
    void)
{
    //NOTE: enter/leave criticalsection is not required! Titles are calling it before DirectSoundCreate.

	LOG_FUNC();

    LOG_IGNORED();
}

// ******************************************************************
// * patch: DirectSoundDoWork
// ******************************************************************
VOID WINAPI XTL::EMUPATCH(DirectSoundDoWork)()
{
    DSoundMutexGuardLock;

    if (!g_bDSoundCreateCalled) {
        return;
    }

	LOG_FUNC();

    xboxkrnl::LARGE_INTEGER getTime;
    xboxkrnl::KeQuerySystemTime(&getTime);

    DirectSoundDoWork_Buffer(getTime);

    // Actually, DirectSoundStream need to process buffer packets here.
    DirectSoundDoWork_Stream(getTime);

    return;
}
// For Async process purpose only
static void dsound_thread_worker(LPVOID nullPtr)
{
	SetThreadAffinityMask(GetCurrentThread(), g_CPUOthers);

    while (true) {
        // Testcase: Gauntlet Dark Legacy, if Sleep(1) then intro videos start to starved often
        // unless console is open with logging enabled. This is the cause of stopping intro videos often.
        Sleep(300);
        // Enforce mutex guard lock only occur inside below bracket for proper compile build.
        {
            DSoundMutexGuardLock;

            vector_ds_stream::iterator ppDSStream = g_pDSoundStreamCache.begin();
            for (; ppDSStream != g_pDSoundStreamCache.end(); ppDSStream++) {
                if ((*ppDSStream)->Host_BufferPacketArray.size() == 0) {
                    continue;
                }
                if (((*ppDSStream)->EmuFlags & DSE_FLAG_FLUSH_ASYNC) > 0 && (*ppDSStream)->Xb_rtFlushEx == 0) {
                    DSStream_Packet_Process((*ppDSStream));
                }
            }
        }
    }
}

// Kismet given name for RadWolfie's experiment major issue in the mutt.
#define DirectSuicideWork XTL::EMUPATCH(DirectSoundDoWork)

/* ------------- Sorted relative functions begin ------------------*/

// ******************************************************************
// * patch: CDirectSound_CommitDeferredSettings
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(CDirectSound_CommitDeferredSettings)
(
    X_CDirectSound*         pThis)
{
    DSoundMutexGuardLock;

	LOG_FUNC_ONE_ARG(pThis);

    HRESULT hRet = DS_OK;
    if (g_pDSoundPrimary3DListener8 != nullptr) {
        hRet = g_pDSoundPrimary3DListener8->CommitDeferredSettings();
    }

    RETURN_RESULT_CHECK(hRet);
}

// ******************************************************************
// * patch: IDirectSound_CommitDeferredSettings
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_CommitDeferredSettings)
(
    X_CDirectSound*     pThis)
{
    DSoundMutexGuardLock;

    LOG_FORWARD("CDirectSound_CommitDeferredSettings");

    return XTL::EMUPATCH(CDirectSound_CommitDeferredSettings)(pThis);
}

// ******************************************************************
// * patch: IDirectSound_CommitEffectData
// ******************************************************************
// This API is used relative with DSP effect.
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_CommitEffectData)
(
    X_CDirectSound*     pThis)
{
    DSoundMutexGuardLock;

    LOG_FUNC_ONE_ARG(pThis);

    // NOTE: DSP relative function.

    LOG_NOT_SUPPORTED();

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_GetEffectData
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_GetEffectData)
(
    X_CDirectSound* pThis,
    DWORD           dwEffectIndex,
    DWORD           dwOffset,
    OUT LPVOID          pvData,
    DWORD           dwDataSize)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(dwEffectIndex)
		LOG_FUNC_ARG(dwOffset)
		LOG_FUNC_ARG_OUT(pvData)
		LOG_FUNC_ARG(dwDataSize)
		LOG_FUNC_END;

    // NOTE: DSP relative function

    LOG_NOT_SUPPORTED();

    /* RadWolfie: Should not allocate memory, xbox xbe is just asking for input data, not allocate then input data...
    if (!pvData) {
        pvData = malloc(dwDataSize); // TODO : FIXME : Shouldn't this be : *pvData = ...  ?!?
    }*/

    return S_OK;
}

// ******************************************************************
// * patch: IDirectSound_SetEffectData
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetEffectData)
(
    LPVOID  pThis,
    DWORD   dwEffectIndex,
    DWORD   dwOffset,
    LPCVOID pvData,
    DWORD   dwDataSize,
    DWORD   dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(dwEffectIndex)
		LOG_FUNC_ARG(dwOffset)
		LOG_FUNC_ARG(pvData)
		LOG_FUNC_ARG(dwDataSize)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    // NOTE: DSP relative function

    LOG_NOT_SUPPORTED();

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_DownloadEffectsImage
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_DownloadEffectsImage)
(
    LPDIRECTSOUND8  pThis,
    LPCVOID         pvImageBuffer,
    DWORD           dwImageSize,
    PVOID           pImageLoc,      // TODO: Use this param
    PVOID*          ppImageDesc)    // TODO: Use this param
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(pvImageBuffer)
		LOG_FUNC_ARG(dwImageSize)
		LOG_FUNC_ARG(pImageLoc)
		LOG_FUNC_ARG(ppImageDesc)
		LOG_FUNC_END;

    // This function is relative to DSP for Interactive 3-D Audio Level 2 (I3DL2)

    LOG_NOT_SUPPORTED();

    return S_OK;
}

// ******************************************************************
// * patch: IDirectSound_EnableHeadphones
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_EnableHeadphones)
(
    LPDIRECTSOUND8      pThis,
    BOOL                fEnabled)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(fEnabled)
		LOG_FUNC_END;

    //Windows Vista and later does not set speaker configuration from SetSpeakerConfig function.
    EmuLog(LOG_LEVEL::WARNING, "EmuIDirectSound_EnableHeadphones ignored");

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_GetCaps
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_GetCaps)
(
    X_CDirectSound*     pThis,
    OUT X_DSCAPS*           pDSCaps)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG_OUT(pDSCaps)
		LOG_FUNC_END;

    // Convert PC -> Xbox
    if (pDSCaps) {
        pDSCaps->dwFreeBufferSGEs = DSoundSGEFreeBuffer();
        // To prevent pass down overflow size.
        pDSCaps->dwMemoryAllocated = (XTL_DS_SGE_SIZE_MAX < g_dwXbMemAllocated ? XTL_DS_SGE_SIZE_MAX : g_dwXbMemAllocated);

        // TODO: What are the max values for 2D and 3D Buffers? Once discover, then perform real time update in global variable.
        pDSCaps->dwFree2DBuffers = (pDSCaps->dwFreeBufferSGEs == 0 ? 0 : 0x200 /* TODO: Replace me to g_dwFree2DBuffers*/ );
        pDSCaps->dwFree3DBuffers = (pDSCaps->dwFreeBufferSGEs == 0 ? 0 : 0x200 /* TODO: Replace me to g_dwFree3DBuffers*/ );

        EmuLog(LOG_LEVEL::DEBUG, "X_DSCAPS: dwFree2DBuffers = %8X | dwFree3DBuffers = %8X | dwFreeBufferSGEs = %08X | dwMemAlloc = %08X", pDSCaps->dwFree2DBuffers, pDSCaps->dwFree3DBuffers, pDSCaps->dwFreeBufferSGEs, pDSCaps->dwMemoryAllocated);
    }

    return S_OK;
}

// ******************************************************************
// * patch: IDirectSound_GetOutputLevels
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_GetOutputLevels)
(
    LPDIRECTSOUND8*         pThis,
    OUT X_DSOUTPUTLEVELS*       pOutputLevels,
    BOOL                    bResetPeakValues)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG_OUT(pOutputLevels)
		LOG_FUNC_ARG(bResetPeakValues)
		LOG_FUNC_END;

    // TODO: Anything?  Either way, I've never seen a game to date use this...

    // NOTE: It ask for access to real time output (require capture device I believe).

    LOG_NOT_SUPPORTED();

    return S_OK;
}

// ******************************************************************
// * patch: CDirectSound_GetSpeakerConfig
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(CDirectSound_GetSpeakerConfig)
(
    X_CDirectSound*         pThis,
    OUT PDWORD              pdwSpeakerConfig)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG_OUT(pdwSpeakerConfig)
		LOG_FUNC_END;

    // TODO: Fix me!
    *pdwSpeakerConfig = 0; // STEREO

    return S_OK;
}

// ******************************************************************
// * patch: IDirectSound_GetSpeakerConfig
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_GetSpeakerConfig)
(
    X_CDirectSound*         pThis,
    OUT LPDWORD*            pdwSpeakerConfig)
{
    DSoundMutexGuardLock;

    LOG_FUNC_BEGIN
        LOG_FUNC_ARG(pThis)
        LOG_FUNC_ARG_OUT(pdwSpeakerConfig)
    LOG_FUNC_END;

    //For now, let's set it to stereo.
    *pdwSpeakerConfig = X_DSSPEAKER_STEREO;

    return S_OK;
}

// ******************************************************************
// * patch: IDirectSound_SetAllParameters
// ******************************************************************
// NOTE: No conversion requirement for XB to PC.
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetAllParameters)
(
    LPDIRECTSOUND8          pThis,
    LPCDS3DLISTENER         pDS3DListenerParameters,
    DWORD                   dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(pDS3DListenerParameters)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = g_pDSoundPrimary3DListener8->SetAllParameters(pDS3DListenerParameters, dwApply);

    RETURN_RESULT_CHECK(hRet);
}

// ******************************************************************
// * patch: IDirectSound_SetDistanceFactor
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetDistanceFactor)
(
    LPDIRECTSOUND8  pThis,
    FLOAT           fDistanceFactor,
    DWORD           dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(fDistanceFactor)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = HybridDirectSound3DListener_SetDistanceFactor(g_pDSoundPrimary3DListener8, fDistanceFactor, dwApply);

    return hRet;
}

// ******************************************************************
// * patch: IDirectSound_SetDopplerFactor
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetDopplerFactor)
(
    LPDIRECTSOUND8  pThis,
    FLOAT           fDopplerFactor,
    DWORD           dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(fDopplerFactor)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = HybridDirectSound3DListener_SetDopplerFactor(g_pDSoundPrimary3DListener8, fDopplerFactor, dwApply);

    return hRet;
}

// ******************************************************************
// * patch: IDirectSound_SetI3DL2Listener
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetI3DL2Listener)
(
    LPDIRECTSOUND8          pThis,
    X_DSI3DL2LISTENER      *pds3dl,
    DWORD                   dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(pds3dl)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    // This function is relative to DSP, although it needs SetFX from LPDIRECTSOUNDBUFFER8 or LPDIRECTSOUNDFXI3DL2REVERB8 class.

    LOG_NOT_SUPPORTED();

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_SetMixBinHeadroom
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetMixBinHeadroom)
(
    LPDIRECTSOUND8          pThis,
    DWORD                   dwMixBinMask,
    DWORD                   dwHeadroom)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(dwMixBinMask)
		LOG_FUNC_ARG(dwHeadroom)
		LOG_FUNC_END;

    LOG_UNIMPLEMENTED();

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_SetOrientation
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetOrientation)
(
    LPDIRECTSOUND8  pThis,
    FLOAT           xFront,
    FLOAT           yFront,
    FLOAT           zFront,
    FLOAT           xTop,
    FLOAT           yTop,
    FLOAT           zTop,
    DWORD           dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(xFront)
		LOG_FUNC_ARG(yFront)
		LOG_FUNC_ARG(zFront)
		LOG_FUNC_ARG(xTop)
		LOG_FUNC_ARG(yTop)
		LOG_FUNC_ARG(zTop)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = DS_OK;
    // TODO: (DSound) Should we do restrictive or passive to return actual result back to titles?
    // Test case: Jet Set Radio Future, ?
    if (xFront == 0.0f && yFront == 0.0f && zFront == 0.0f) {
        printf("WARNING: SetOrientation was called with xFront = 0, yFront = 0, and zFront = 0. Current action is ignore call to PC.\n");
    }
    if (xTop == 0.0f && yTop == 0.0f && zTop == 0.0f) {
        printf("WARNING: SetOrientation was called with xTop = 0, yTop = 0, and zTop = 0. Current action is ignore call to PC.\n");
    } else {
        hRet = g_pDSoundPrimary3DListener8->SetOrientation(xFront, yFront, zFront, xTop, yTop, zTop, dwApply);
    }

    RETURN_RESULT_CHECK(hRet);
}

// ******************************************************************
// * patch: IDirectSound_SetPosition
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetPosition)
(
    LPDIRECTSOUND8          pThis,
    FLOAT                   x,
    FLOAT                   y,
    FLOAT                   z,
    DWORD                   dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(x)
		LOG_FUNC_ARG(y)
		LOG_FUNC_ARG(z)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = g_pDSoundPrimary3DListener8->SetPosition(x, y, z, dwApply);

    RETURN_RESULT_CHECK(hRet);
}

// ******************************************************************
// * patch: IDirectSound_SetRolloffFactor
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetRolloffFactor)
(
    LPDIRECTSOUND8  pThis,
    FLOAT           fRolloffFactor,
    DWORD           dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(fRolloffFactor)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = HybridDirectSound3DListener_SetRolloffFactor(g_pDSoundPrimary3DListener8, fRolloffFactor, dwApply);

    return hRet;
}

// ******************************************************************
// * patch: CDirectSound_SynchPlayback
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(CDirectSound_SynchPlayback)
(
    LPDIRECTSOUND8 pThis)
{
    DSoundMutexGuardLock;

	LOG_FUNC_ONE_ARG(pThis);

    //TODO: Test case Rayman 3 - Hoodlum Havoc, Battlestar Galactica, Miami Vice, and ...?

    vector_ds_buffer::iterator ppDSBuffer = g_pDSoundBufferCache.begin();
    for (; ppDSBuffer != g_pDSoundBufferCache.end(); ppDSBuffer++) {
        if ((*ppDSBuffer)->X_BufferCache == nullptr) {
            continue;
        }

        if (((*ppDSBuffer)->EmuFlags & DSE_FLAG_SYNCHPLAYBACK_CONTROL) > 0) {
            DSoundBufferSynchPlaybackFlagRemove((*ppDSBuffer)->EmuFlags);
            EmuLog(LOG_LEVEL::DEBUG, "SynchPlayback - EmuPlayFlags: %08X", (*ppDSBuffer)->EmuPlayFlags);
            (*ppDSBuffer)->EmuDirectSoundBuffer8->Play(0, 0, (*ppDSBuffer)->EmuPlayFlags);
        }
    }

    vector_ds_stream::iterator ppDSStream = g_pDSoundStreamCache.begin();
    for (; ppDSStream != g_pDSoundStreamCache.end(); ppDSStream++) {
        if ((*ppDSStream)->Host_BufferPacketArray.size() == 0) {
            continue;
        }
        if (((*ppDSStream)->EmuFlags & DSE_FLAG_SYNCHPLAYBACK_CONTROL) > 0) {
            DSoundBufferSynchPlaybackFlagRemove((*ppDSStream)->EmuFlags);
            DSStream_Packet_Process((*ppDSStream));
        }
    }

    //EmuLog(LOG_LEVEL::DEBUG, "Buffer started: %u; Stream started: %u", debugSynchBufferCount, debugSynchStreamCount);

    return DS_OK;
}

// ******************************************************************
// * patch: IDirectSound_SynchPlayback
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SynchPlayback)
(
    LPDIRECTSOUND8          pThis)
{
    DSoundMutexGuardLock;

	LOG_FORWARD("CDirectSound_SynchPlayback");

    return XTL::EMUPATCH(CDirectSound_SynchPlayback)(pThis);
}

// ******************************************************************
// * patch: IDirectSound_SetVelocity
// ******************************************************************
HRESULT WINAPI XTL::EMUPATCH(IDirectSound_SetVelocity)
(
    LPDIRECTSOUND8          pThis,
    FLOAT                   x,
    FLOAT                   y,
    FLOAT                   z,
    DWORD                   dwApply)
{
    DSoundMutexGuardLock;

	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(pThis)
		LOG_FUNC_ARG(x)
		LOG_FUNC_ARG(y)
		LOG_FUNC_ARG(z)
		LOG_FUNC_ARG(dwApply)
		LOG_FUNC_END;

    HRESULT hRet = g_pDSoundPrimary3DListener8->SetVelocity(x, y, z, dwApply);

    RETURN_RESULT_CHECK(hRet);
}
