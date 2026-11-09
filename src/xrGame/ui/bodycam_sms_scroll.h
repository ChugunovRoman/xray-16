#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Presentation state only. Message contents live in the game's news registry/UI rows.
namespace bodycam_sms
{
struct Row { float top, bottom; };
class Scroll
{
public:
    enum class Phase { Enter, WaitStart, Read, HoldEnd, Idle, CatchUp };
    float offset=0;
    size_t cursor=0;
    Phase phase=Phase::Enter;
    double wait=0;
    bool wasVisible=true;
    float catchSpeed=180;

    void Latest(const std::vector<Row>& rows,float height)
    {
        cursor=rows.size();phase=Phase::CatchUp;wait=0;
        const float target=rows.empty()?0:std::max(0.f,rows.back().bottom-height);
        catchSpeed=std::max(180.f,std::abs(target-offset)/.5f);
    }
    void RemovePrefix(size_t count,float shift)
    { cursor=cursor>count?cursor-count:0;offset=std::max(0.f,offset-shift); }

    void Update(double dt,const std::vector<Row>& rows,float height,bool visible,bool animate=true)
    {
        if(!visible){wasVisible=false;return;}
        if(!wasVisible){Latest(rows,height);wasVisible=true;}
        if(rows.empty()){offset=0;cursor=0;phase=Phase::Enter;wait=0;return;}
        const float maximum=std::max(0.f,rows.back().bottom-height);
        if(phase==Phase::CatchUp)catchSpeed=std::max(catchSpeed,std::abs(maximum-offset)/.5f);
        offset=std::max(0.f,std::min(offset,maximum));
        if(!animate)return;
        double left=std::max(0.,dt);
        for(size_t transitions=0;transitions<rows.size()*5+8;++transitions)
        {
            if(phase==Phase::Idle)
            {
                if(cursor>=rows.size())break;
                phase=Phase::Enter;
            }
            float target=offset,speed=36;
            if(phase==Phase::CatchUp){target=maximum;speed=catchSpeed;}
            else if(cursor>=rows.size()){phase=Phase::Idle;break;}
            else if(phase==Phase::Enter)
                target=rows[cursor].bottom-rows[cursor].top>height?rows[cursor].top:std::max(0.f,rows[cursor].bottom-height);
            else if(phase==Phase::Read){target=std::max(0.f,rows[cursor].bottom-height);speed=4;}
            if(phase==Phase::WaitStart || phase==Phase::HoldEnd)
            {
                const double use=std::min(left,std::max(0.,3.-wait));wait+=use;left-=use;
                if(wait<3.-1e-8)break;
                wait=0;
                if(phase==Phase::WaitStart)phase=Phase::Read;
                else {++cursor;phase=Phase::Enter;}
                continue;
            }
            const double duration=std::abs(target-offset)/speed;
            if(duration>left+1e-8)
            {
                offset+=float((target>offset?1:-1)*speed*left);break;
            }
            offset=target;left=std::max(0.,left-duration);
            if(phase==Phase::CatchUp){cursor=rows.size();phase=Phase::Idle;break;}
            if(phase==Phase::Enter){phase=Phase::WaitStart;wait=0;}
            else if(duration>1e-8){phase=Phase::HoldEnd;wait=0;}
            else {++cursor;phase=Phase::Enter;}
        }
    }
};
}
