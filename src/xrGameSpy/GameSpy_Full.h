#pragma once
#include "xrGameSpy/xrGameSpy.h"

class CGameSpy_Available;
class CGameSpy_HTTP;
enum class GSUpdateStatus;
class CGameSpy_BrowsersWrapper;
class CGameSpy_GP;
class CGameSpy_ATLAS;

class XRGAMESPY_API CGameSpy_Full
{
    bool m_bServicesAlreadyChecked;
    bool m_bOnlineServices; // false: no availability check, the SDK is marked as unavailable, Update does nothing

public:
    // onlineServices == false: GameSpy is off - nothing goes to the network (no DNS lookup and no UDP query of
    // the availability check, the SDK refuses the master server, GP and ATLAS), the objects are still created
    // so the code that takes GetGameSpyGP() / GetGameSpyBrowser() and so on does not get null pointers
    explicit CGameSpy_Full(bool onlineServices = true);
    ~CGameSpy_Full();

    bool IsOnlineServicesEnabled() const { return m_bOnlineServices; }

    CGameSpy_Available* GetGameSpyAvailable() const { return m_pGSA; }
    CGameSpy_HTTP* GetGameSpyHTTP() const { return m_pGS_HTTP; }
    CGameSpy_BrowsersWrapper* GetGameSpyBrowser() const { return m_pGS_SB; }
    CGameSpy_GP* GetGameSpyGP() const { return m_pGS_GP; }
    CGameSpy_ATLAS* GetGameSpyATLAS() const { return m_pGS_ATLAS; }
    GSUpdateStatus Update();
    void CoreThink(u32 millisecondsTimeout);

private:
    CGameSpy_Available* m_pGSA;
    CGameSpy_HTTP* m_pGS_HTTP;
    CGameSpy_BrowsersWrapper* m_pGS_SB;
    CGameSpy_GP* m_pGS_GP;
    CGameSpy_ATLAS* m_pGS_ATLAS;
};
