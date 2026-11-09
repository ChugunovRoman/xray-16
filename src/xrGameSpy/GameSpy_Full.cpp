#include "stdafx.h"
#include "GameSpy_Full.h"
#include "GameSpy_Available.h"
#include "GameSpy_HTTP.h"
#include "GameSpy_BrowsersWrapper.h"
#include "GameSpy_GP.h"
#include "GameSpy_ATLAS.h"
#include "Common/object_broker.h"

CGameSpy_Full::CGameSpy_Full(bool onlineServices)
{
    m_pGSA = NULL;
    m_pGS_HTTP = NULL;
    m_pGS_SB = NULL;
    m_pGS_GP = NULL;
    m_bOnlineServices = onlineServices;
    //---------------------------------------
    m_pGSA = xr_new<CGameSpy_Available>();
    //-----------------------------------------------------
    if (m_bOnlineServices)
    {
        shared_str resultstr;
        m_bServicesAlreadyChecked = m_pGSA->CheckAvailableServices(resultstr);
    }
    else
    {
        // The availability check is a blocking gethostbyname and an UDP query (up to 2 x 2000 ms) - skip it.
        // GSIACUnavailable: ServerBrowserNew (internet), gpInitialize, scInitialize, wsLoginProfile, sake and
        // the others refuse to work, so nothing of the SDK goes to the GameSpy servers afterwards.
        __GSIACResult = GSIACUnavailable;
        // true: Update does not report OutOfService (the main menu would show the "service is not available" box)
        m_bServicesAlreadyChecked = true;
        Msg("* GameSpy: online services are disabled (start with -gamespy to enable them)");
    }
    //-----------------------------------------------------
    gsCoreInitialize();
    m_pGS_HTTP = xr_new<CGameSpy_HTTP>();
    m_pGS_SB = xr_new<CGameSpy_BrowsersWrapper>();

    m_pGS_GP = xr_new<CGameSpy_GP>();
    m_pGS_ATLAS = xr_new<CGameSpy_ATLAS>();
}

CGameSpy_Full::~CGameSpy_Full()
{
    delete_data(m_pGSA);
    delete_data(m_pGS_HTTP);
    delete_data(m_pGS_SB);
    delete_data(m_pGS_GP);
    delete_data(m_pGS_ATLAS);
    gsCoreShutdown();
}

GSUpdateStatus CGameSpy_Full::Update()
{
    // GameSpy is off: the browsers have no ServerBrowser (ServerBrowserThink(NULL) would crash), GP and ATLAS
    // are not initialized - nothing to think about; Success hides the "connecting to master server" box
    if (!m_bOnlineServices)
        return GSUpdateStatus::Success;
    if (!m_bServicesAlreadyChecked)
    {
        m_bServicesAlreadyChecked = true;
        return GSUpdateStatus::OutOfService;
    }
    m_pGS_HTTP->Think();
    GSUpdateStatus status = m_pGS_SB->Update();
    m_pGS_GP->Think();
    CoreThink(15);
    m_pGS_ATLAS->Think();
    return status;
}

void CGameSpy_Full::CoreThink(u32 millisecondsTimeout) { gsCoreThink(millisecondsTimeout); }
