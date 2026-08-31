#pragma once
#include "core/GameObject.h"
#include <array>
inline std::array<float,9> TransformValues(const zengine::Transform& t){auto p=t.Position(),r=t.Rotation(),s=t.Scale();return {p.x,p.y,p.z,r.x,r.y,r.z,s.x,s.y,s.z};}
inline unsigned TransformDifference(const zengine::Transform& a,const zengine::Transform& b){auto x=TransformValues(a),y=TransformValues(b);unsigned mask=0;for(unsigned i=0;i<9;++i)if(x[i]!=y[i])mask|=1u<<i;return mask;}
inline zengine::Transform OverrideTransform(const zengine::Transform& source,const zengine::Transform& instance,unsigned mask){auto a=TransformValues(source),b=TransformValues(instance);for(unsigned i=0;i<9;++i)if(mask&(1u<<i))a[i]=b[i];zengine::Transform result;result.SetPosition({a[0],a[1],a[2]});result.SetRotation({a[3],a[4],a[5]});result.SetScale({a[6],a[7],a[8]});return result;}
