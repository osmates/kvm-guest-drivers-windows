/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 * Copyright (C) 2023 Max R (viogpu3d, virtio-win pull request 943)
 * Copyright (C) 2026 OSMates contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include "helper.h"

// The DDI types make the compiler check each entry point against the WDK.
extern "C" DRIVER_INITIALIZE DriverEntry;

DXGKDDI_UNLOAD VioGpu3dUnload;
DXGKDDI_ADD_DEVICE VioGpu3dAddDevice;
DXGKDDI_REMOVE_DEVICE VioGpu3dRemoveDevice;
DXGKDDI_START_DEVICE VioGpu3dStartDevice;
DXGKDDI_STOP_DEVICE VioGpu3dStopDevice;
DXGKDDI_RESET_DEVICE VioGpu3dResetDevice;
DXGKDDI_DISPATCH_IO_REQUEST VioGpu3dDispatchIoRequest;
DXGKDDI_SET_POWER_STATE VioGpu3dSetPowerState;
DXGKDDI_QUERY_CHILD_RELATIONS VioGpu3dQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS VioGpu3dQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR VioGpu3dQueryDeviceDescriptor;
DXGKDDI_INTERRUPT_ROUTINE VioGpu3dInterruptRoutine;
DXGKDDI_DPC_ROUTINE VioGpu3dDpcRoutine;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP VioGpu3dStopDeviceAndReleasePostDisplayOwnership;
DXGKDDI_SYSTEM_DISPLAY_ENABLE VioGpu3dSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE VioGpu3dSystemDisplayWrite;

DXGKDDI_QUERYADAPTERINFO VioGpu3dQueryAdapterInfo;
DXGKDDI_CREATEDEVICE VioGpu3dCreateDevice;
DXGKDDI_DESTROYDEVICE VioGpu3dDestroyDevice;
DXGKDDI_CREATEALLOCATION VioGpu3dCreateAllocation;
DXGKDDI_DESTROYALLOCATION VioGpu3dDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION VioGpu3dDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA VioGpu3dGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO VioGpu3dOpenAllocation;
DXGKDDI_CLOSEALLOCATION VioGpu3dCloseAllocation;
DXGKDDI_CREATECONTEXT VioGpu3dCreateContext;
DXGKDDI_DESTROYCONTEXT VioGpu3dDestroyContext;
DXGKDDI_SUBMITCOMMANDVIRTUAL VioGpu3dSubmitCommandVirtual;
DXGKDDI_SUBMITCOMMAND VioGpu3dSubmitCommand;
DXGKDDI_PREEMPTCOMMAND VioGpu3dPreemptCommand;
DXGKDDI_PATCH VioGpu3dPatch;
DXGKDDI_RENDER VioGpu3dRender;
DXGKDDI_RENDERGDI VioGpu3dRenderGdi;
DXGKDDI_PRESENT VioGpu3dPresent;
DXGKDDI_BUILDPAGINGBUFFER VioGpu3dBuildPagingBuffer;
DXGKDDI_SETPOINTERPOSITION VioGpu3dSetPointerPosition;
DXGKDDI_SETPOINTERSHAPE VioGpu3dSetPointerShape;
DXGKDDI_RESETFROMTIMEOUT VioGpu3dResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT VioGpu3dRestartFromTimeout;
DXGKDDI_ESCAPE VioGpu3dEscape;
DXGKDDI_COLLECTDBGINFO VioGpu3dCollectDbgInfo;
DXGKDDI_QUERYCURRENTFENCE VioGpu3dQueryCurrentFence;
DXGKDDI_QUERYENGINESTATUS VioGpu3dQueryEngineStatus;
DXGKDDI_RESETENGINE VioGpu3dResetEngine;
DXGKDDI_CANCELCOMMAND VioGpu3dCancelCommand;
DXGKDDI_GETNODEMETADATA VioGpu3dGetNodeMetadata;
DXGKDDI_CALIBRATEGPUCLOCK VioGpu3dCalibrateGpuClock;
DXGKDDI_SETSTABLEPOWERSTATE VioGpu3dSetStablePowerState;
DXGKDDI_CREATEPROCESS VioGpu3dCreateProcess;
DXGKDDI_DESTROYPROCESS VioGpu3dDestroyProcess;
DXGKDDI_GETROOTPAGETABLESIZE VioGpu3dGetRootPageTableSize;
DXGKDDI_SETROOTPAGETABLE VioGpu3dSetRootPageTable;
DXGKDDI_MAPCPUHOSTAPERTURE VioGpu3dMapCpuHostAperture;
DXGKDDI_UNMAPCPUHOSTAPERTURE VioGpu3dUnmapCpuHostAperture;

DXGKDDI_ISSUPPORTEDVIDPN VioGpu3dIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN VioGpu3dRecommendFunctionalVidPn;
DXGKDDI_RECOMMENDVIDPNTOPOLOGY VioGpu3dRecommendVidPnTopology;
DXGKDDI_RECOMMENDMONITORMODES VioGpu3dRecommendMonitorModes;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY VioGpu3dEnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY VioGpu3dSetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN VioGpu3dCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH VioGpu3dUpdateActiveVidPnPresentPath;
DXGKDDI_QUERYVIDPNHWCAPABILITY VioGpu3dQueryVidPnHWCapability;
DXGKDDI_SETVIDPNSOURCEADDRESS VioGpu3dSetVidPnSourceAddress;
DXGKDDI_CONTROLINTERRUPT VioGpu3dControlInterrupt;
DXGKDDI_GETSCANLINE VioGpu3dGetScanLine;

#if DBG
void DebugPrintFuncSerial(CONST char *format, ...);
void DebugPrintFuncKdPrint(CONST char *format, ...);
#endif
