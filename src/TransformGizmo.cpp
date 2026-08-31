#include "TransformGizmo.h"
#include <cmath>
#include <limits>

namespace gizmo
{
using namespace DirectX;
using V=zengine::Vec3;
namespace
{
    XMVECTOR Load(V v) { return XMVectorSet(v.x,v.y,v.z,0); }
    V Store(FXMVECTOR v) { XMFLOAT3 f; XMStoreFloat3(&f,v); return {f.x,f.y,f.z}; }
    V Add(V a,V b,float scale=1) { return {a.x+b.x*scale,a.y+b.y*scale,a.z+b.z*scale}; }
    V Unit(int axis) { return axis==0?V{1,0,0}:axis==1?V{0,1,0}:V{0,0,1}; }
    float Dot(FXMVECTOR a,FXMVECTOR b) { return XMVectorGetX(XMVector3Dot(a,b)); }
    void Basis(V axis,V& u,V& v)
    {
        const auto n=Load(axis);
        const auto seed=std::abs(axis.y)<0.9f?XMVectorSet(0,1,0,0):XMVectorSet(1,0,0,0);
        u=Store(XMVector3Normalize(XMVector3Cross(n,seed))); v=Store(XMVector3Cross(n,Load(u)));
    }
    void Ray(const ViewportCamera& c,Point p,XMVECTOR& origin,XMVECTOR& direction)
    {
        origin=XMVector3Unproject(XMVectorSet(p.x,p.y,0,0),0,0,c.width,c.height,0,1,c.projection,c.view,XMMatrixIdentity());
        const auto far=XMVector3Unproject(XMVectorSet(p.x,p.y,1,0),0,0,c.width,c.height,0,1,c.projection,c.view,XMMatrixIdentity());
        direction=XMVector3Normalize(far-origin);
    }
    void SetAxis(V& v,int axis,float value) { if (axis==0) v.x=value; else if (axis==1) v.y=value; else v.z=value; }
    float GetAxis(V v,int axis) { return axis==0?v.x:axis==1?v.y:v.z; }
}
std::optional<Point> Project(const ViewportCamera& c,V point)
{
    const auto clip=XMVector4Transform(XMVectorSet(point.x,point.y,point.z,1),c.view*c.projection);
    const float w=XMVectorGetW(clip),z=XMVectorGetZ(clip);
    if (!std::isfinite(w) || w<=0 || z<0 || z>w) return {};
    return Point{(XMVectorGetX(clip)/w+1)*c.width/2,(1-XMVectorGetY(clip)/w)*c.height/2};
}
Shape Build(const ViewportCamera& camera,const zengine::Transform& transform,Mode mode)
{
    Shape shape; shape.center=transform.Position();
    const float depth=XMVectorGetZ(XMVector3TransformCoord(Load(shape.center),camera.view));
    if (depth<=0.1f || depth>=99) return shape;
    shape.length=depth*std::tan(XM_PI/6)*180/camera.height;
    const auto r=transform.Rotation();
    const auto rx=XMMatrixRotationX(XMConvertToRadians(r.x)),ry=XMMatrixRotationY(XMConvertToRadians(r.y)),rz=XMMatrixRotationZ(XMConvertToRadians(r.z));
    for (int axis=0;axis<3;++axis)
    {
        // Scale follows local axes. Rotation rings match the Inspector's X-then-Y-then-Z Euler order.
        auto orientation=XMMatrixIdentity();
        if (mode==Mode::Scale) orientation=rx*ry*rz;
        else if (mode==Mode::Rotate) orientation=axis==0?ry*rz:axis==1?rz:XMMatrixIdentity();
        shape.axes[axis]=Store(XMVector3TransformNormal(Load(Unit(axis)),orientation));
        V u,v; Basis(shape.axes[axis],u,v);
        const auto line=[&](V a,V b) { shape.lines.push_back({a,b,axis}); };
        if (mode==Mode::Rotate)
        {
            for (int i=0;i<96;++i)
            {
                const auto at=[&](float angle) { return Add(Add(shape.center,u,std::cos(angle)*shape.length),v,std::sin(angle)*shape.length); };
                line(at(i*XM_2PI/96),at((i+1)*XM_2PI/96));
            }
        }
        else
        {
            const auto tip=Add(shape.center,shape.axes[axis],shape.length);
            line(Add(shape.center,shape.axes[axis],shape.length*0.15f),tip);
            if (mode==Mode::Move)
            {
                const auto base=Add(tip,shape.axes[axis],-shape.length*0.18f);
                for (float sign:{-1.f,1.f}) { line(tip,Add(base,u,sign*shape.length*.08f)); line(tip,Add(base,v,sign*shape.length*.08f)); }
            }
            else
            {
                V corners[8];
                for (int i=0;i<8;++i) corners[i]=Add(Add(Add(tip,u,(i&1?1.f:-1.f)*shape.length*.07f),v,(i&2?1.f:-1.f)*shape.length*.07f),shape.axes[axis],(i&4?1.f:-1.f)*shape.length*.07f);
                for (int i=0;i<8;++i) for (int bit:{1,2,4}) if (!(i&bit)) line(corners[i],corners[i|bit]);
            }
        }
    }
    return shape;
}
std::optional<Hit> Pick(const ViewportCamera& camera,const Shape& shape,Point point,float tolerance)
{
    std::optional<Hit> hit; float best=tolerance*tolerance;
    for (std::size_t i=0;i<shape.lines.size();++i)
    {
        const auto a=Project(camera,shape.lines[i].a),b=Project(camera,shape.lines[i].b); if (!a || !b) continue;
        const float dx=b->x-a->x,dy=b->y-a->y,denom=dx*dx+dy*dy;
        if (denom<0.01f) continue;
        const float t=std::clamp(((point.x-a->x)*dx+(point.y-a->y)*dy)/denom,0.f,1.f);
        const float x=point.x-a->x-t*dx,y=point.y-a->y-t*dy,d=x*x+y*y;
        if (d<best) { best=d; hit=Hit{shape.lines[i].axis,i,t}; }
    }
    return hit;
}
std::optional<float> Drag::AxisParameter(Point p) const
{
    XMVECTOR o,d; Ray(camera_,p,o,d); const auto a=Load(shape_.axes[axis_]),offset=o-Load(shape_.center);
    const float parallel=Dot(a,d),denom=1-parallel*parallel;
    if (denom<0.002f) return {};
    return (Dot(offset,a)-Dot(offset,d)*parallel)/denom;
}
std::optional<float> Drag::RingAngle(Point p) const
{
    XMVECTOR o,d; Ray(camera_,p,o,d); const auto a=Load(shape_.axes[axis_]);
    const float denom=Dot(a,d); if (std::abs(denom)<0.08f) return {};
    const float distance=Dot(Load(shape_.center)-o,a)/denom; if (distance<=0) return {};
    const auto radial=o+d*distance-Load(shape_.center);
    if (Dot(radial,radial)<shape_.length*shape_.length*.01f) return {};
    V u,v; Basis(shape_.axes[axis_],u,v);
    return std::atan2(Dot(radial,Load(v)),Dot(radial,Load(u)));
}
Drag::Drag(const ViewportCamera& camera,const zengine::Transform& transform,Mode mode,const Shape& shape,Hit hit,Point point)
    :camera_(camera),original_(transform),mode_(mode),shape_(shape),axis_(hit.axis),start_(point)
{
    auto a=Project(camera,shape.center),b=Project(camera,Add(shape.center,shape.axes[axis_],shape.length));
    if (mode==Mode::Rotate) { a=Project(camera,shape.lines.at(hit.segment).a); b=Project(camera,shape.lines.at(hit.segment).b); }
    if (a && b)
    {
        const float x=b->x-a->x,y=b->y-a->y,len=std::sqrt(x*x+y*y);
        if (len>0.01f) tangent_={x/len,y/len};
    }
    // A local scale axis aimed exactly at the camera has no projected shaft direction.
    // Its end box is still pickable; vertical dragging provides a stable escape from that pose.
    if (tangent_.x==0 && tangent_.y==0) tangent_={0,-1};
    if (mode==Mode::Rotate) { const auto angle=RingAngle(point); planar_=angle.has_value(); previousAngle_=angle.value_or(0); }
    else { const auto t=AxisParameter(point); planar_=t.has_value(); startParameter_=t.value_or(0); }
}
zengine::Transform Drag::Update(Point point)
{
    auto result=original_;
    const float pixels=(point.x-start_.x)*tangent_.x+(point.y-start_.y)*tangent_.y;
    if (mode_==Mode::Rotate)
    {
        if (planar_)
        {
            if (const auto angle=RingAngle(point)) { angle_+=std::remainder(*angle-previousAngle_,XM_2PI); previousAngle_=*angle; }
        }
        else angle_=pixels/90; // Near edge-on rings: drag their visible tangent instead of an unstable plane.
        auto r=original_.Rotation(); SetAxis(r,axis_,std::clamp(GetAxis(r,axis_)+XMConvertToDegrees(angle_),-1000000.f,1000000.f)); result.SetRotation(r);
    }
    else
    {
        const float amount=planar_?AxisParameter(point).value_or(startParameter_)-startParameter_:pixels*shape_.length/90;
        if (!std::isfinite(amount)) return result;
        if (mode_==Mode::Move)
        { auto p=Add(original_.Position(),shape_.axes[axis_],amount); p.x=std::clamp(p.x,-1000000.f,1000000.f); p.y=std::clamp(p.y,-1000000.f,1000000.f); p.z=std::clamp(p.z,-1000000.f,1000000.f); result.SetPosition(p); }
        else { auto s=original_.Scale(); SetAxis(s,axis_,std::clamp(GetAxis(s,axis_)+amount/shape_.length,-1000000.f,1000000.f)); result.SetScale(s); }
    }
    return result;
}
}
