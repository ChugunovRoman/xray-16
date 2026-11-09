#pragma once
// Global War procedural layer. No Monolith source is copied.
#include "Actor.h"
#include "Actor_Flags.h"
#include "Weapon.h"
#include "Inventory.h"
#include "xrEngine/XR_IOConsole.h"

namespace gw_bodycam_motion
{
struct Spring
{
    float x=0, v=0;
    void step(float target,float frequency,float dt)
    {
        // Exact critically damped solution for a constant target; stable at all frame rates.
        const float w=frequency, e=expf(-w*dt), d=x-target, c=v+w*d;
        x=target+(d+c*dt)*e; v=(v-w*c*dt)*e;
    }
};
struct State
{
    CActor* actor=nullptr; CWeapon* weapon=nullptr;
    u32 frame=~0u; float yaw=0,pitch=0,phase=0,aim=0;
    Fvector position{}, hud{}, camera{}, angle{};
    Spring turnX,turnY,forward,side,move,kick;
};
inline State& state(){static State s;return s;}
inline int& sms_visible(){static int value=1;return value;}
inline float gps_top(){return sms_visible()?400.f:550.f;}
inline bool active(){int minimum=0,maximum=0;return Console && Console->GetInteger("r__bodycam",minimum,maximum)==1;}
inline int& hud_only(){static int value=0;return value;}
inline bool hud_active(){return hud_only()!=0 || active();}
inline bool god_vitals(){return hud_active() && IsGameTypeSingle() && psActorFlags.test(AF_GODMODE);}
extern std::atomic<u32> god_vitals_activation;
inline float param(pcstr section,pcstr key,float fallback,float minimum,float maximum)
{
    const float value=READ_IF_EXISTS(pSettings,r_float,section,key,fallback);
    return _valid(value)?clampr(value,minimum,maximum):fallback;
}
struct Profile{float frequency,lag,kick,bob;};
inline Profile profile(CWeapon* weapon)
{
    const int type=!weapon?1:(weapon->HandDependence()==hd1Hand?0:(weapon->Weight()>5.f?2:1));
    static const Profile profiles[3]= {
        {param("bodycam_motion_pistol","frequency",19,8,40),param("bodycam_motion_pistol","lag",0.010f,0,0.03f),param("bodycam_motion_pistol","kick",0.20f,0,0.5f),param("bodycam_motion_pistol","bob",0.003f,0,0.01f)},
        {param("bodycam_motion_rifle","frequency",13,8,40),param("bodycam_motion_rifle","lag",0.017f,0,0.03f),param("bodycam_motion_rifle","kick",0.27f,0,0.5f),param("bodycam_motion_rifle","bob",0.004f,0,0.01f)},
        {param("bodycam_motion_heavy","frequency",10,8,40),param("bodycam_motion_heavy","lag",0.023f,0,0.03f),param("bodycam_motion_heavy","kick",0.32f,0,0.5f),param("bodycam_motion_heavy","bob",0.005f,0,0.01f)}};
    return profiles[type];
}
inline void shot(CWeapon* weapon)
{
    State& s=state();
    if(active() && s.weapon==weapon && weapon && weapon->H_Parent()==s.actor)
        s.kick.v-=profile(weapon).kick; // axial visual impulse only; native ballistic/camera recoil stays unchanged
}
inline void camera(CActor* actor,Fvector& point,Fvector& angles,float yaw,float pitch,float dt)
{
    State& s=state();
    if(!active() || !actor->HUDview() || !actor->g_Alive() || Device.Paused()) {s=State{};return;}
    CWeapon* weapon=smart_cast<CWeapon*>(actor->inventory().ActiveItem());
    if(s.actor!=actor || dt<=0 || dt>0.1f || (s.frame!=Device.dwFrame && s.frame+1!=Device.dwFrame))
    {s=State{};s.actor=actor;s.weapon=weapon;s.yaw=yaw;s.pitch=pitch;s.position=actor->Position();}
    if(s.weapon!=weapon){s.weapon=weapon;s.kick=Spring{};}
    if(s.frame!=Device.dwFrame)
    {
        s.frame=Device.dwFrame;
        const Profile p=profile(weapon);
        const float speedYaw=clampr(angle_difference_signed(s.yaw,yaw)/_max(dt,0.001f),-8.f,8.f);
        const float speedPitch=clampr(angle_difference_signed(s.pitch,pitch)/_max(dt,0.001f),-8.f,8.f);
        s.yaw=yaw;s.pitch=pitch;
        Fvector velocity;velocity.sub(actor->Position(),s.position);velocity.div(_max(dt,0.001f));s.position=actor->Position();
        if(velocity.square_magnitude()>225.f){s=State{};return;} // teleport/load discontinuity
        Fvector forward;forward.set(-sinf(yaw),0,cosf(yaw));
        const float longitudinal=velocity.dotproduct(forward);
        const float lateral=velocity.x*cosf(yaw)+velocity.z*sinf(yaw);
        s.turnX.step(-speedYaw*p.lag,p.frequency,dt);s.turnY.step(speedPitch*p.lag,p.frequency,dt);
        s.forward.step(clampr(longitudinal,-7.f,7.f),p.frequency,dt);s.side.step(clampr(lateral,-7.f,7.f),p.frequency,dt);
        s.move.step(clampr(velocity.magnitude()/5.f,0.f,1.f),9.f,dt);
        s.kick.step(0,p.frequency,dt);
        s.phase+=dt*(7.f+3.f*s.move.x);
        const float targetAim=weapon?_max(weapon->GetZRotatingFactor(),weapon->IsSecondZoomed()?1.f:0.f):0.f;
        s.aim=clampr(targetAim,0.f,1.f);
        const float hip=(1.f-s.aim)*(1.f-s.aim);
        const bool busy=weapon && (weapon->GetState()==CWeapon::eReload || weapon->GetState()==CWeapon::eHiding);
        const float gain=busy?0.f:hip;
        const float bob=p.bob*s.move.x;
        s.hud.set(clampr(s.turnX.x-s.side.x*0.001f,-0.035f,0.035f)*gain,
                  (clampr(s.turnY.x,-0.025f,0.025f)+sinf(s.phase)*bob)*gain,
                  clampr(s.kick.x,-0.025f,0.005f)*(busy?0.f:0.15f+0.85f*hip)-s.forward.x*0.0015f*gain);
        // Translation/roll only in ADS: optical axes retain the native aim solution.
        s.camera.set(-s.side.x*0.0007f*hip*cosf(yaw),sinf(s.phase*2.f)*bob*0.25f*hip,-s.side.x*0.0007f*hip*sinf(yaw));
        s.angle.set(clampr(s.turnY.x*0.10f,-0.003f,0.003f)*hip,
                    clampr(s.turnX.x*0.10f,-0.003f,0.003f)*hip,
                    (s.turnX.x*0.08f+sinf(s.phase)*bob*0.3f)*hip);
    }
    point.add(s.camera);angles.add(s.angle);
}
inline void hud(CActor* actor,CWeapon* weapon,Fmatrix& transform)
{
    State& s=state();
    if(!active() || s.actor!=actor || s.weapon!=weapon || s.frame!=Device.dwFrame)return;
    transform.c.mad(transform.i,s.hud.x);transform.c.mad(transform.j,s.hud.y);transform.c.mad(transform.k,s.hud.z);
}
}
