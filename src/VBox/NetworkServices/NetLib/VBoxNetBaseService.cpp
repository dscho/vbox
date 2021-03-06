/* $Id$ */
/** @file
 * VBoxNetDHCP - DHCP Service for connecting to IntNet.
 */
/** @todo r=bird: Cut&Past rules... Please fix DHCP refs! */

/*
 * Copyright (C) 2009-2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_NET_SERVICE

#include <VBox/com/com.h>
#include <VBox/com/listeners.h>
#include <VBox/com/string.h>
#include <VBox/com/Guid.h>
#include <VBox/com/array.h>
#include <VBox/com/ErrorInfo.h>
#include <VBox/com/errorprint.h>
#include <VBox/com/VirtualBox.h>
#include <VBox/com/NativeEventQueue.h>

#include <iprt/alloca.h>
#include <iprt/buildconfig.h>
#include <iprt/err.h>
#include <iprt/net.h>                   /* must come before getopt.h. */
#include <iprt/getopt.h>
#include <iprt/initterm.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <iprt/process.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/time.h>
#include <iprt/thread.h>
#include <iprt/mem.h>
#include <iprt/message.h>

#include <VBox/sup.h>
#include <VBox/intnet.h>
#include <VBox/intnetinline.h>
#include <VBox/vmm/vmm.h>
#include <VBox/version.h>

#include <vector>
#include <string>

#include <VBox/err.h>
#include <VBox/log.h>

#include "VBoxNetLib.h"
#include "VBoxNetBaseService.h"

#ifdef RT_OS_WINDOWS /* WinMain */
# include <Windows.h>
# include <stdlib.h>
#endif


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
struct VBoxNetBaseService::Data
{
    Data(const std::string& aName, const std::string& aNetworkName):
      m_Name(aName),
      m_Network(aNetworkName),
      m_enmTrunkType(kIntNetTrunkType_WhateverNone),
      m_pSession(NIL_RTR0PTR),
      m_cbSendBuf(128 * _1K),
      m_cbRecvBuf(256 * _1K),
      m_hIf(INTNET_HANDLE_INVALID),
      m_pIfBuf(NULL),
      m_cVerbosity(0),
      m_fNeedMain(false),
      m_EventQ(NULL),
      m_hThrRecv(NIL_RTTHREAD),
      fShutdown(false)
    {
        int rc = RTCritSectInit(&m_csThis);
        AssertRC(rc);
    };

    std::string         m_Name;
    std::string         m_Network;
    std::string         m_TrunkName;
    INTNETTRUNKTYPE     m_enmTrunkType;

    RTMAC               m_MacAddress;
    RTNETADDRIPV4       m_Ipv4Address;
    RTNETADDRIPV4       m_Ipv4Netmask;

    PSUPDRVSESSION      m_pSession;
    uint32_t            m_cbSendBuf;
    uint32_t            m_cbRecvBuf;
    INTNETIFHANDLE      m_hIf;          /**< The handle to the network interface. */
    PINTNETBUF          m_pIfBuf;       /**< Interface buffer. */

    std::vector<PRTGETOPTDEF> m_vecOptionDefs;

    int32_t             m_cVerbosity;

    /* cs for syncing */
    RTCRITSECT          m_csThis;

    /* Controls whether service will connect SVC for runtime needs */
    bool                m_fNeedMain;
    /* Event Queue */
    com::NativeEventQueue  *m_EventQ;

    /** receiving thread, used only if main is used */
    RTTHREAD m_hThrRecv;

    bool fShutdown;
    static int recvLoop(RTTHREAD, void *);
};

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/* Commonly used options for network configuration */
static RTGETOPTDEF g_aGetOptDef[] =
{
    { "--name",           'N',   RTGETOPT_REQ_STRING },
    { "--network",        'n',   RTGETOPT_REQ_STRING },
    { "--trunk-name",     't',   RTGETOPT_REQ_STRING },
    { "--trunk-type",     'T',   RTGETOPT_REQ_STRING },
    { "--mac-address",    'a',   RTGETOPT_REQ_MACADDR },
    { "--ip-address",     'i',   RTGETOPT_REQ_IPV4ADDR },
    { "--netmask",        'm',   RTGETOPT_REQ_IPV4ADDR },
    { "--verbose",        'v',   RTGETOPT_REQ_NOTHING },
    { "--need-main",      'M',   RTGETOPT_REQ_BOOL },
};


int VBoxNetBaseService::Data::recvLoop(RTTHREAD, void *pvUser)
{
    VBoxNetBaseService *pThis = static_cast<VBoxNetBaseService *>(pvUser);

    HRESULT hrc = com::Initialize();
    AssertComRCReturn(hrc, VERR_INTERNAL_ERROR);

    pThis->doReceiveLoop();

    return VINF_SUCCESS;
}


VBoxNetBaseService::VBoxNetBaseService(const std::string& aName, const std::string& aNetworkName):m(NULL)
{
    m = new VBoxNetBaseService::Data(aName, aNetworkName);

    for(unsigned int i = 0; i < RT_ELEMENTS(g_aGetOptDef); ++i)
        m->m_vecOptionDefs.push_back(&g_aGetOptDef[i]);
}


VBoxNetBaseService::~VBoxNetBaseService()
{
    /*
     * Close the interface connection.
     */
    if (m != NULL)
    {
        shutdown();
        if (m->m_hIf != INTNET_HANDLE_INVALID)
        {
            INTNETIFCLOSEREQ CloseReq;
            CloseReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
            CloseReq.Hdr.cbReq = sizeof(CloseReq);
            CloseReq.pSession = m->m_pSession;
            CloseReq.hIf = m->m_hIf;
            m->m_hIf = INTNET_HANDLE_INVALID;
            int rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_RTCPUID, VMMR0_DO_INTNET_IF_CLOSE, 0, &CloseReq.Hdr);
            AssertRC(rc);
        }

        if (m->m_pSession != NIL_RTR0PTR)
        {
            SUPR3Term(false /*fForced*/);
            m->m_pSession = NIL_RTR0PTR;
        }

        RTCritSectDelete(&m->m_csThis);

        delete m;
        m = NULL;
    }
}


int VBoxNetBaseService::init()
{
    if (isMainNeeded())
    {
        HRESULT hrc = com::Initialize();
        AssertComRCReturn(hrc, VERR_INTERNAL_ERROR);

        hrc = virtualbox.createLocalObject(CLSID_VirtualBox);
        AssertComRCReturn(hrc, VERR_INTERNAL_ERROR);
    }

    return VINF_SUCCESS;
}


bool VBoxNetBaseService::isMainNeeded() const
{
    return m->m_fNeedMain;
}


int VBoxNetBaseService::run()
{
    /**
     * If child class need Main we start receving thread which calls doReceiveLoop and enter to event polling loop
     * and for the rest clients we do receiving on the current (main) thread.
     */
    if (isMainNeeded())
        return startReceiveThreadAndEnterEventLoop();
    else
    {
        doReceiveLoop();
        return VINF_SUCCESS;
    }
}

/**
 * Parse the arguments.
 *
 * @returns 0 on success, fully bitched exit code on failure.
 *
 * @param   argc    Argument count.
 * @param   argv    Argument vector.
 */
int VBoxNetBaseService::parseArgs(int argc, char **argv)
{

    RTGETOPTSTATE State;
    PRTGETOPTDEF paOptionArray = getOptionsPtr();
    int rc = RTGetOptInit(&State, argc, argv, paOptionArray, m->m_vecOptionDefs.size(), 0, 0 /*fFlags*/);
    AssertRCReturn(rc, 49);
#if 0
    /* default initialization */
    m_enmTrunkType = kIntNetTrunkType_WhateverNone;
#endif
    Log2(("BaseService: parseArgs enter\n"));

    for (;;)
    {
        RTGETOPTUNION Val;
        rc = RTGetOpt(&State, &Val);
        if (!rc)
            break;
        switch (rc)
        {
            case 'N': // --name
                m->m_Name = Val.psz;
                break;

            case 'n': // --network
                m->m_Network = Val.psz;
                break;

            case 't': //--trunk-name
                m->m_TrunkName = Val.psz;
                break;

            case 'T': //--trunk-type
                if (!strcmp(Val.psz, "none"))
                    m->m_enmTrunkType = kIntNetTrunkType_None;
                else if (!strcmp(Val.psz, "whatever"))
                    m->m_enmTrunkType = kIntNetTrunkType_WhateverNone;
                else if (!strcmp(Val.psz, "netflt"))
                    m->m_enmTrunkType = kIntNetTrunkType_NetFlt;
                else if (!strcmp(Val.psz, "netadp"))
                    m->m_enmTrunkType = kIntNetTrunkType_NetAdp;
                else if (!strcmp(Val.psz, "srvnat"))
                    m->m_enmTrunkType = kIntNetTrunkType_SrvNat;
                else
                {
                    RTStrmPrintf(g_pStdErr, "Invalid trunk type '%s'\n", Val.psz);
                    return 1;
                }
                break;

            case 'a': // --mac-address
                m->m_MacAddress = Val.MacAddr;
                break;

            case 'i': // --ip-address
                m->m_Ipv4Address = Val.IPv4Addr;
                break;

            case 'm': // --netmask
                m->m_Ipv4Netmask = Val.IPv4Addr;
                break;

            case 'v': // --verbose
                m->m_cVerbosity++;
                break;

            case 'V': // --version (missed)
                RTPrintf("%sr%u\n", RTBldCfgVersion(), RTBldCfgRevision());
                return 1;

            case 'M': // --need-main
                m->m_fNeedMain = true;
                break;

            case 'h': // --help (missed)
                RTPrintf("%s Version %sr%u\n"
                         "(C) 2009-" VBOX_C_YEAR " " VBOX_VENDOR "\n"
                         "All rights reserved.\n"
                         "\n"
                         "Usage: %s <options>\n"
                         "\n"
                         "Options:\n",
                         RTProcShortName(),
                         RTBldCfgVersion(),
                         RTBldCfgRevision(),
                         RTProcShortName());
                for (unsigned int i = 0; i < m->m_vecOptionDefs.size(); i++)
                    RTPrintf("    -%c, %s\n", m->m_vecOptionDefs[i]->iShort, m->m_vecOptionDefs[i]->pszLong);
                usage(); /* to print Service Specific usage */
                return 1;

            default:
                int rc1 = parseOpt(rc, Val);
                if (RT_FAILURE(rc1))
                {
                    rc = RTGetOptPrintError(rc, &Val);
                    RTPrintf("Use --help for more information.\n");
                    return rc;
                }
        }
    }

    RTMemFree(paOptionArray);
    return rc;
}


int VBoxNetBaseService::tryGoOnline(void)
{
    /*
     * Open the session, load ring-0 and issue the request.
     */
    int rc = SUPR3Init(&m->m_pSession);
    if (RT_FAILURE(rc))
    {
        m->m_pSession = NIL_RTR0PTR;
        LogRel(("VBoxNetBaseService: SUPR3Init -> %Rrc\n", rc));
        return rc;
    }

    char szPath[RTPATH_MAX];
    rc = RTPathExecDir(szPath, sizeof(szPath) - sizeof("/VMMR0.r0"));
    if (RT_FAILURE(rc))
    {
        LogRel(("VBoxNetBaseService: RTPathExecDir -> %Rrc\n", rc));
        return rc;
    }

    rc = SUPR3LoadVMM(strcat(szPath, "/VMMR0.r0"));
    if (RT_FAILURE(rc))
    {
        LogRel(("VBoxNetBaseService: SUPR3LoadVMM(\"%s\") -> %Rrc\n", szPath, rc));
        return rc;
    }

    /*
     * Create the open request.
     */
    PINTNETBUF pBuf;
    INTNETOPENREQ OpenReq;
    OpenReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
    OpenReq.Hdr.cbReq = sizeof(OpenReq);
    OpenReq.pSession = m->m_pSession;
    strncpy(OpenReq.szNetwork, m->m_Network.c_str(), sizeof(OpenReq.szNetwork));
    OpenReq.szNetwork[sizeof(OpenReq.szNetwork) - 1] = '\0';
    strncpy(OpenReq.szTrunk, m->m_TrunkName.c_str(), sizeof(OpenReq.szTrunk));
    OpenReq.szTrunk[sizeof(OpenReq.szTrunk) - 1] = '\0';
    OpenReq.enmTrunkType = m->m_enmTrunkType;
    OpenReq.fFlags = 0; /** @todo check this */
    OpenReq.cbSend = m->m_cbSendBuf;
    OpenReq.cbRecv = m->m_cbRecvBuf;
    OpenReq.hIf = INTNET_HANDLE_INVALID;

    /*
     * Issue the request.
     */
    Log2(("attempting to open/create network \"%s\"...\n", OpenReq.szNetwork));
    rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_VMCPUID, VMMR0_DO_INTNET_OPEN, 0, &OpenReq.Hdr);
    if (RT_FAILURE(rc))
    {
        Log2(("VBoxNetBaseService: SUPR3CallVMMR0Ex(,VMMR0_DO_INTNET_OPEN,) failed, rc=%Rrc\n", rc));
        return rc;
    }
    m->m_hIf = OpenReq.hIf;
    Log2(("successfully opened/created \"%s\" - hIf=%#x\n", OpenReq.szNetwork, m->m_hIf));

    /*
     * Get the ring-3 address of the shared interface buffer.
     */
    INTNETIFGETBUFFERPTRSREQ GetBufferPtrsReq;
    GetBufferPtrsReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
    GetBufferPtrsReq.Hdr.cbReq = sizeof(GetBufferPtrsReq);
    GetBufferPtrsReq.pSession = m->m_pSession;
    GetBufferPtrsReq.hIf = m->m_hIf;
    GetBufferPtrsReq.pRing3Buf = NULL;
    GetBufferPtrsReq.pRing0Buf = NIL_RTR0PTR;
    rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_VMCPUID, VMMR0_DO_INTNET_IF_GET_BUFFER_PTRS, 0, &GetBufferPtrsReq.Hdr);
    if (RT_FAILURE(rc))
    {
        Log2(("VBoxNetBaseService: SUPR3CallVMMR0Ex(,VMMR0_DO_INTNET_IF_GET_BUFFER_PTRS,) failed, rc=%Rrc\n", rc));
        return rc;
    }
    pBuf = GetBufferPtrsReq.pRing3Buf;
    Log2(("pBuf=%p cbBuf=%d cbSend=%d cbRecv=%d\n",
               pBuf, pBuf->cbBuf, pBuf->cbSend, pBuf->cbRecv));
    m->m_pIfBuf = pBuf;

    /*
     * Activate the interface.
     */
    INTNETIFSETACTIVEREQ ActiveReq;
    ActiveReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
    ActiveReq.Hdr.cbReq = sizeof(ActiveReq);
    ActiveReq.pSession = m->m_pSession;
    ActiveReq.hIf = m->m_hIf;
    ActiveReq.fActive = true;
    rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_VMCPUID, VMMR0_DO_INTNET_IF_SET_ACTIVE, 0, &ActiveReq.Hdr);
    if (RT_SUCCESS(rc))
        return 0;

    /* bail out */
    Log2(("VBoxNetBaseService: SUPR3CallVMMR0Ex(,VMMR0_DO_INTNET_IF_SET_PROMISCUOUS_MODE,) failed, rc=%Rrc\n", rc));

    /* ignore this error */
    return VINF_SUCCESS;
}


void VBoxNetBaseService::shutdown(void)
{
    syncEnter();
    m->fShutdown = true;
    syncLeave();
}


int VBoxNetBaseService::syncEnter()
{
    return RTCritSectEnter(&m->m_csThis);
}


int VBoxNetBaseService::syncLeave()
{
    return RTCritSectLeave(&m->m_csThis);
}


int VBoxNetBaseService::waitForIntNetEvent(int cMillis)
{
    int rc = VINF_SUCCESS;
    INTNETIFWAITREQ WaitReq;
    LogFlowFunc(("ENTER:cMillis: %d\n", cMillis));
    WaitReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
    WaitReq.Hdr.cbReq = sizeof(WaitReq);
    WaitReq.pSession = m->m_pSession;
    WaitReq.hIf = m->m_hIf;
    WaitReq.cMillies = cMillis;

    rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_VMCPUID, VMMR0_DO_INTNET_IF_WAIT, 0, &WaitReq.Hdr);
    LogFlowFuncLeaveRC(rc);
    return rc;
}

/* S/G API */
int VBoxNetBaseService::sendBufferOnWire(PCINTNETSEG pcSg, int cSg, size_t cbFrame)
{
    PINTNETHDR pHdr = NULL;
    uint8_t *pu8Frame = NULL;

    /* Allocate frame */
    int rc = IntNetRingAllocateFrame(&m->m_pIfBuf->Send, cbFrame, &pHdr, (void **)&pu8Frame);
    AssertRCReturn(rc, rc);

    /* Now we fill pvFrame with S/G above */
    int offFrame = 0;
    for (int idxSg = 0; idxSg < cSg; ++idxSg)
    {
        memcpy(&pu8Frame[offFrame], pcSg[idxSg].pv, pcSg[idxSg].cb);
        offFrame+=pcSg[idxSg].cb;
    }

    /* Commit */
    IntNetRingCommitFrameEx(&m->m_pIfBuf->Send, pHdr, cbFrame);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * forcible ask for send packet on the "wire"
 */
void VBoxNetBaseService::flushWire()
{
    int rc = VINF_SUCCESS;
    INTNETIFSENDREQ SendReq;
    SendReq.Hdr.u32Magic = SUPVMMR0REQHDR_MAGIC;
    SendReq.Hdr.cbReq    = sizeof(SendReq);
    SendReq.pSession     = m->m_pSession;
    SendReq.hIf          = m->m_hIf;
    rc = SUPR3CallVMMR0Ex(NIL_RTR0PTR, NIL_VMCPUID, VMMR0_DO_INTNET_IF_SEND, 0, &SendReq.Hdr);
    AssertRCReturnVoid(rc);
    LogFlowFuncLeave();

}


int VBoxNetBaseService::hlpUDPBroadcast(unsigned uSrcPort, unsigned uDstPort,
                                         void const *pvData, size_t cbData) const
{
    return VBoxNetUDPBroadcast(m->m_pSession, m->m_hIf, m->m_pIfBuf,
                        m->m_Ipv4Address, &m->m_MacAddress, uSrcPort,
                        uDstPort, pvData, cbData);

}


const std::string VBoxNetBaseService::getName() const
{
    return m->m_Name;
}


void VBoxNetBaseService::setName(const std::string& aName)
{
    m->m_Name = aName;
}


const std::string VBoxNetBaseService::getNetwork() const
{
    return m->m_Network;
}


void VBoxNetBaseService::setNetwork(const std::string& aNetwork)
{
    m->m_Network = aNetwork;
}


const RTMAC VBoxNetBaseService::getMacAddress() const
{
    return m->m_MacAddress;
}


void VBoxNetBaseService::setMacAddress(const RTMAC& aMac)
{
    m->m_MacAddress = aMac;
}


const RTNETADDRIPV4 VBoxNetBaseService::getIpv4Address() const
{
    return m->m_Ipv4Address;
}


void VBoxNetBaseService::setIpv4Address(const RTNETADDRIPV4& aAddress)
{
    m->m_Ipv4Address = aAddress;
}


const RTNETADDRIPV4 VBoxNetBaseService::getIpv4Netmask() const
{
    return m->m_Ipv4Netmask;
}


void VBoxNetBaseService::setIpv4Netmask(const RTNETADDRIPV4& aNetmask)
{
    m->m_Ipv4Netmask = aNetmask;
}


uint32_t VBoxNetBaseService::getSendBufSize() const
{
    return m->m_cbSendBuf;
}


void VBoxNetBaseService::setSendBufSize(uint32_t cbBuf)
{
    m->m_cbSendBuf = cbBuf;
}


uint32_t VBoxNetBaseService::getRecvBufSize() const
{
    return m->m_cbRecvBuf;
}


void VBoxNetBaseService::setRecvBufSize(uint32_t cbBuf)
{
    m->m_cbRecvBuf = cbBuf;
}


int32_t VBoxNetBaseService::getVerbosityLevel() const
{
    return m->m_cVerbosity;
}


void VBoxNetBaseService::setVerbosityLevel(int32_t aVerbosity)
{
    m->m_cVerbosity = aVerbosity;
}


void VBoxNetBaseService::addCommandLineOption(const PRTGETOPTDEF optDef)
{
    m->m_vecOptionDefs.push_back(optDef);
}


void VBoxNetBaseService::doReceiveLoop()
{
    int rc;
    /* Well we're ready */
    PINTNETRINGBUF  pRingBuf = &m->m_pIfBuf->Recv;

    for (;;)
    {
        /*
         * Wait for a packet to become available.
         */
        /* 2. waiting for request for */
        rc = waitForIntNetEvent(2000);
        if (RT_FAILURE(rc))
        {
            if (rc == VERR_TIMEOUT || rc == VERR_INTERRUPTED)
            {
                /* do we want interrupt anyone ??? */
                continue;
            }
            LogRel(("VBoxNetNAT: waitForIntNetEvent returned %Rrc\n", rc));
            AssertRCReturnVoid(rc);
        }

        /*
         * Process the receive buffer.
         */
        PCINTNETHDR pHdr;

        while ((pHdr = IntNetRingGetNextFrameToRead(pRingBuf)) != NULL)
        {
            uint8_t const u8Type = pHdr->u8Type;
            size_t         cbFrame = pHdr->cbFrame;
            switch (u8Type)
            {

                case INTNETHDR_TYPE_FRAME:
                    {
                        void *pvFrame = IntNetHdrGetFramePtr(pHdr, m->m_pIfBuf);
                        rc = processFrame(pvFrame, cbFrame);
                        if (RT_FAILURE(rc) && rc == VERR_IGNORED)
                        {
                            /* XXX: UDP + ARP for DHCP */
                            VBOXNETUDPHDRS Hdrs;
                            size_t  cb;
                            void   *pv = VBoxNetUDPMatch(m->m_pIfBuf, RTNETIPV4_PORT_BOOTPS, &m->m_MacAddress,
                                                         VBOXNETUDP_MATCH_UNICAST | VBOXNETUDP_MATCH_BROADCAST
                                                         | VBOXNETUDP_MATCH_CHECKSUM
                                                         | (m->m_cVerbosity > 2 ? VBOXNETUDP_MATCH_PRINT_STDERR : 0),
                                                         &Hdrs, &cb);
                            if (pv && cb)
                                processUDP(pv, cb);
                            else
                                VBoxNetArpHandleIt(m->m_pSession, m->m_hIf, m->m_pIfBuf, &m->m_MacAddress, m->m_Ipv4Address);
                        }
                    }
                    break;
                case INTNETHDR_TYPE_GSO:
                  {
                      PCPDMNETWORKGSO pGso = IntNetHdrGetGsoContext(pHdr, m->m_pIfBuf);
                      rc = processGSO(pGso, cbFrame);
                      if (RT_FAILURE(rc) && rc == VERR_IGNORED)
                          break;
                  }
                  break;
                case INTNETHDR_TYPE_PADDING:
                    break;
                default:
                    break;
            }
            IntNetRingSkipFrame(&m->m_pIfBuf->Recv);

        } /* loop */
    }

}


int VBoxNetBaseService::startReceiveThreadAndEnterEventLoop()
{
    AssertMsgReturn(isMainNeeded(), ("It's expected that we need Main"), VERR_INTERNAL_ERROR);

    /* start receiving thread */
    int rc = RTThreadCreate(&m->m_hThrRecv, /* thread handle*/
                            &VBoxNetBaseService::Data::recvLoop,  /* routine */
                            this, /* user data */
                            128 * _1K, /* stack size */
                            RTTHREADTYPE_IO, /* type */
                            0, /* flags, @todo: waitable ?*/
                            "RECV");
    AssertRCReturn(rc,rc);

    m->m_EventQ = com::NativeEventQueue::getMainEventQueue();
    AssertPtrReturn(m->m_EventQ, VERR_INTERNAL_ERROR);

    while(true)
    {
        m->m_EventQ->processEventQueue(0);

        if (m->fShutdown)
            break;

        m->m_EventQ->processEventQueue(500);
    }

    return VINF_SUCCESS;
}


void VBoxNetBaseService::debugPrint(int32_t iMinLevel, bool fMsg, const char *pszFmt, ...) const
{
    if (iMinLevel <= m->m_cVerbosity)
    {
        va_list va;
        va_start(va, pszFmt);
        debugPrintV(iMinLevel, fMsg, pszFmt, va);
        va_end(va);
    }
}


/**
 * Print debug message depending on the m_cVerbosity level.
 *
 * @param   iMinLevel       The minimum m_cVerbosity level for this message.
 * @param   fMsg            Whether to dump parts for the current service message.
 * @param   pszFmt          The message format string.
 * @param   va              Optional arguments.
 */
void VBoxNetBaseService::debugPrintV(int iMinLevel, bool fMsg, const char *pszFmt, va_list va) const
{
    if (iMinLevel <= m->m_cVerbosity)
    {
        va_list vaCopy;                 /* This dude is *very* special, thus the copy. */
        va_copy(vaCopy, va);
        RTStrmPrintf(g_pStdErr, "%s: %s: %N\n",
                     RTProcShortName(),
                     iMinLevel >= 2 ? "debug" : "info",
                     pszFmt,
                     &vaCopy);
        va_end(vaCopy);
    }

}


PRTGETOPTDEF VBoxNetBaseService::getOptionsPtr()
{
    PRTGETOPTDEF pOptArray = NULL;
    pOptArray = (PRTGETOPTDEF)RTMemAlloc(sizeof(RTGETOPTDEF) * m->m_vecOptionDefs.size());
    if (!pOptArray)
        return NULL;
    for (unsigned int i = 0; i < m->m_vecOptionDefs.size(); ++i)
    {
        PRTGETOPTDEF pOpt = m->m_vecOptionDefs[i];
        memcpy(&pOptArray[i], m->m_vecOptionDefs[i], sizeof(RTGETOPTDEF));
    }
    return pOptArray;
}
