// External UI animation only: requests and draws verified live routes; never queries game memory.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include "features.h"

namespace arkhelp::features {

class PathAnimation {
public:
    struct Playback {
        std::uint64_t id=0, started=0, updated=0, absent_since=0;
        ipc::PathEntry path{};
        float progress=0, length=0, consumed=0, last_x=0, last_y=0;
        bool ready=false, complete=false, have_position=false, located=true;
    };
    struct Hovered {
        std::array<std::uint64_t,ipc::kMaxUnits> ids{};
        std::size_t count=0;
        bool contains(std::uint64_t id) const {
            return std::find(ids.begin(),ids.begin()+count,id)!=ids.begin()+count;
        }
    };
    void reset() { *this=PathAnimation{}; }
    void invalidate_projection(std::uint64_t now) { reset(); projection_after_ms_=now; }
    std::span<const std::uint64_t> requests() const { return {requests_.data(),request_count_}; }
    const auto& playbacks() const { return playing_; }

    void update(const ipc::BattleSnapshot& snapshot,const PointerState& pointer,float scale,
                std::uint64_t now,int width,int height,const OverlayOptions& options) {
        if(serial_!=snapshot.battle_serial || width_!=width || height_!=height ||
            (last_tick_ && now<last_tick_)) {
            const auto cutoff=width_>0 && (width_!=width || height_!=height)?now:projection_after_ms_;
            reset(); projection_after_ms_=cutoff;
            serial_=snapshot.battle_serial; width_=width; height_=height;
        }
        last_tick_=now;
        request_count_=0;
        const auto hovered=hovered_enemies(snapshot,pointer,scale);
        for(auto& item:playing_) if(item.id) {
            bool alive=false;
            for(std::uint32_t i=0;i<std::min(snapshot.unit_count,ipc::kMaxUnits);++i)
                if(snapshot.units[i].tracking_id==item.id && (snapshot.units[i].flags&ipc::kUnitValid)) alive=true;
            if(!alive) item=Playback{};
        }
        for(std::size_t h=0;h<hovered.count;++h) {
            const auto id=hovered.ids[h];
            if(options.path_on_click ? !pointer.clicked : previous_hover_.contains(id)) continue;
            Playback* slot=nullptr;
            for(auto& item:playing_) if(item.id==id) { slot=&item; break; }
            if(!slot) for(auto& item:playing_) if(!item.id) { slot=&item; break; }
            // Keep already running animations intact when the bounded pool is full.
            if(slot && (!slot->id || slot->complete)) {
                *slot=Playback{}; slot->id=id; slot->started=slot->updated=now;
            }
        }
        previous_hover_=hovered;
        for(auto& item:playing_) {
            if(!item.id) continue;
            const ipc::UnitEntry* unit=nullptr;
            for(std::uint32_t i=0;i<std::min(snapshot.unit_count,ipc::kMaxUnits);++i)
                if(snapshot.units[i].tracking_id==item.id && (snapshot.units[i].flags&ipc::kUnitValid)) unit=&snapshot.units[i];
            if(!unit) { item=Playback{}; continue; }
            {
                for(std::uint32_t i=0;i<std::min(snapshot.path_count,ipc::kMaxPaths);++i) {
                    const auto& path=snapshot.paths[i];
                    if(path.tracking_id!=item.id || path.battle_serial!=serial_ || !(path.flags&ipc::kPathValid) ||
                        path.point_count<2 || path.point_count>ipc::kMaxPathPoints ||
                        !path.sampled_ms || path.sampled_ms<=projection_after_ms_ ||
                        path.sampled_ms>now || now-path.sampled_ms>1000 ||
                        (item.ready && path.sampled_ms<=item.path.sampled_ms)) continue;
                    bool valid=true;
                    for(std::uint32_t j=0;j<path.point_count;++j)
                        valid=valid && std::isfinite(path.points[j].x) && std::isfinite(path.points[j].y);
                    if(!valid) continue;
                    const float revealed=std::max(0.f,item.progress-item.consumed);
                    item.path=path;
                    item.length=path_length(path);
                    if(item.length>0) {
                        item.ready=true; item.updated=now; item.consumed=0;
                        item.progress=item.complete?item.length:std::min(revealed,item.length);
                        item.have_position=false;
                    }
                    break;
                }
            }
            if(item.ready) {
                consume(item,*unit,scale);
                item.progress=std::max(item.progress,item.consumed);
            }
            if(item.ready && !item.complete) {
                const auto elapsed=std::min<std::uint64_t>(now-item.updated,250);
                item.progress=std::min(item.length,item.progress+static_cast<float>(elapsed)*.001f*
                    std::clamp(options.path_speed,50,2000)*scale);
                item.updated=now;
                if(item.progress>=item.length) item.complete=true;
            }
            // Keep the original trigger armed while this instance remains hovered:
            // a slow first capture must still be able to fulfill a real click.
            if(!item.ready && now-item.started>3000 && !hovered.contains(item.id)) { item=Playback{}; continue; }
            // Leaving during drawing cannot cancel it. The linger clock starts only
            // after drawing completes, and resets whenever the pointer returns.
            if(item.complete && !hovered.contains(item.id)) {
                if(!item.absent_since) item.absent_since=now;
                if(now-item.absent_since>=static_cast<std::uint64_t>(std::clamp(options.path_linger_ms,0,10000))) {
                    item=Playback{}; continue;
                }
            } else item.absent_since=0;
            // Keep the remaining route current while the animation or its residue is visible.
            request(item.id);
        }
        // Hovering a group requests every nearby instance. Click mode still needs a real click.
        for(std::size_t h=0;h<hovered.count;++h) request(hovered.ids[h]);
    }

    static Hovered hovered_enemies(const ipc::BattleSnapshot& snapshot,const PointerState& pointer,float scale) {
        Hovered result;
        const float radius_squared=48.0f*scale*48.0f*scale;
        if(!pointer.valid) return result;
        for(std::uint32_t i=0;i<std::min(snapshot.unit_count,ipc::kMaxUnits);++i) {
            const auto& unit=snapshot.units[i];
            if(!(unit.flags&ipc::kUnitValid) || !unit.tracking_id) continue;
            const auto distance=[&](float x,float y) { const float dx=pointer.x-x,dy=pointer.y-y; return dx*dx+dy*dy; };
            float d=distance(unit.x,unit.y);
            if(unit.flags&ipc::kUnitGroundValid) d=std::min(d,distance(unit.ground_x,unit.ground_y));
            if(d<=radius_squared && !result.contains(unit.tracking_id)) result.ids[result.count++]=unit.tracking_id;
        }
        return result;
    }

    static float path_length(const ipc::PathEntry& path) {
        float length=0;
        for(std::uint32_t i=1;i<std::min(path.point_count,ipc::kMaxPathPoints);++i)
            if(!(path.points[i].flags&ipc::kPathMoveTo))
                length+=std::hypot(path.points[i].x-path.points[i-1].x,path.points[i].y-path.points[i-1].y);
        return length;
    }
private:
    static void consume(Playback& item,const ipc::UnitEntry& unit,float scale) {
        const float x=(unit.flags&ipc::kUnitGroundValid)?unit.ground_x:unit.x;
        const float y=(unit.flags&ipc::kUnitGroundValid)?unit.ground_y:unit.y;
        const float moved=item.have_position?std::hypot(x-item.last_x,y-item.last_y):item.length;
        const float limit=item.have_position?std::min(item.length,item.consumed+std::max(8.f*scale,moved*1.5f)):
                                              item.length;
        float best=std::numeric_limits<float>::max(),chosen=item.consumed,travel=0;
        for(std::uint32_t i=1;i<item.path.point_count;++i) {
            const auto& a=item.path.points[i-1]; const auto& b=item.path.points[i];
            if(b.flags&ipc::kPathMoveTo) continue;
            const float dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);
            if(length<.01f) continue;
            const float begin=std::clamp((item.consumed-travel)/length,0.f,1.f);
            const float end=std::clamp((limit-travel)/length,0.f,1.f);
            if(travel+length>=item.consumed && travel<=limit && end>=begin) {
                const float t=std::clamp(((x-a.x)*dx+(y-a.y)*dy)/(length*length),begin,end);
                const float distance=std::hypot(x-a.x-dx*t,y-a.y-dy*t);
                // Earliest equal-distance segment wins at crossings, preserving path order.
                if(distance+.01f<best) { best=distance; chosen=travel+length*t; }
            }
            travel+=length;
        }
        item.located=best<=80.f*scale;
        if(item.located) item.consumed=std::max(item.consumed,chosen);
        item.last_x=x; item.last_y=y; item.have_position=true;
    }
    void request(std::uint64_t id) {
        if(request_count_>=requests_.size()) return;
        if(std::find(requests_.begin(),requests_.begin()+request_count_,id)==requests_.begin()+request_count_)
            requests_[request_count_++]=id;
    }
    std::array<Playback,ipc::kMaxPaths> playing_{};
    std::array<std::uint64_t,ipc::kMaxPaths> requests_{};
    std::size_t request_count_=0;
    std::uint32_t serial_=0;
    int width_=0,height_=0;
    Hovered previous_hover_{};
    std::uint64_t last_tick_=0,projection_after_ms_=0;
};

inline void draw_paths(gfx::Surface& surface,const PathAnimation& animation,float scale,gfx::Color color,
                       const FeatureAppearance& appearance = {}) {
    const float width=std::max(1.0f,std::clamp(appearance.line_width,1,8)*scale);
    for(const auto& item:animation.playbacks()) {
        if(!item.id || !item.ready || !item.located) continue;
        float travel=0;
        for(std::uint32_t i=1;i<item.path.point_count && travel<item.progress;++i) {
            const auto& a=item.path.points[i-1];
            const auto& b=item.path.points[i];
            if(b.flags&ipc::kPathMoveTo) continue;
            const float length=std::hypot(b.x-a.x,b.y-a.y);
            if(length<.01f) continue;
            const float begin=std::clamp((item.consumed-travel)/length,0.f,1.f);
            const float amount=std::clamp((item.progress-travel)/length,0.f,1.f);
            travel+=length;
            if(amount<=begin) continue;
            const float x=a.x+(b.x-a.x)*amount,y=a.y+(b.y-a.y)*amount;
            surface.draw_line(a.x+(b.x-a.x)*begin,a.y+(b.y-a.y)*begin,x,y,color,width,appearance.line_style);
            if(amount<1 || i+1==item.path.point_count) {
                const float ux=(b.x-a.x)/length,uy=(b.y-a.y)/length;
                const float arrow=6*scale;
                surface.draw_line(x,y,x-arrow*ux+arrow*.5f*uy,y-arrow*uy-arrow*.5f*ux,color,width);
                surface.draw_line(x,y,x-arrow*ux-arrow*.5f*uy,y-arrow*uy+arrow*.5f*ux,color,width);
            }
        }
    }
}
} // namespace arkhelp::features
