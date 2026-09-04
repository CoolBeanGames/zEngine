#pragma once
#include "zscript/Script.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace zengine::script::world {
using Matrix=std::array<std::array<double,4>,4>;
inline Matrix Identity(){Matrix m{};for(int i=0;i<4;++i)m[i][i]=1;return m;}
inline Matrix Multiply(const Matrix& a,const Matrix& b){Matrix m{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)m[i][j]+=a[i][k]*b[k][j];return m;}
inline Matrix Rotation(Vector3 v){
    const auto radians=std::numbers::pi/180;const auto x=v.x*radians,y=v.y*radians,z=v.z*radians;
    auto rx=Identity(),ry=Identity(),rz=Identity();
    rx[1][1]=rx[2][2]=std::cos(x);rx[1][2]=std::sin(x);rx[2][1]=-std::sin(x);
    ry[0][0]=ry[2][2]=std::cos(y);ry[0][2]=-std::sin(y);ry[2][0]=std::sin(y);
    rz[0][0]=rz[1][1]=std::cos(z);rz[0][1]=std::sin(z);rz[1][0]=-std::sin(z);
    return Multiply(Multiply(rx,ry),rz);
}
inline Matrix Local(Vector3 p,Vector3 r,Vector3 s){auto m=Rotation(r);const double scales[]={s.x,s.y,s.z};for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i][j]*=scales[i];m[3][0]=p.x;m[3][1]=p.y;m[3][2]=p.z;return m;}
inline Vector3 Euler(const Matrix& m){
    const auto y=std::asin(std::clamp(-m[0][2],-1.0,1.0));const bool regular=std::abs(std::cos(y))>1e-8;
    const auto x=regular?std::atan2(m[1][2],m[2][2]):std::atan2(-m[2][1],m[1][1]);const auto z=regular?std::atan2(m[0][1],m[0][0]):0;
    const auto degrees=180/std::numbers::pi;return {x*degrees,y*degrees,z*degrees};
}
inline Vector3 Scale(const Matrix& m){
    auto length=[&](int row){return std::hypot(m[row][0],m[row][1],m[row][2]);};
    const double det=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    return {length(0)*(det<0?-1:1),length(1),length(2)};
}
// ZE-83: helpers to turn a desired GLOBAL transform component into the LOCAL one
// that produces it under a given parent world matrix (row-vector convention:
// world = Local(child) * parentWorld).
inline Matrix Transpose3(const Matrix& m){Matrix r=Identity();for(int i=0;i<3;++i)for(int j=0;j<3;++j)r[i][j]=m[j][i];return r;}
inline Matrix Inverse3(const Matrix& m){
    const double det=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    Matrix r=Identity();
    if(std::abs(det)<1e-12)return r; // singular parent scale -> fall back to identity
    const double inv=1.0/det;
    r[0][0]=(m[1][1]*m[2][2]-m[1][2]*m[2][1])*inv; r[0][1]=(m[0][2]*m[2][1]-m[0][1]*m[2][2])*inv; r[0][2]=(m[0][1]*m[1][2]-m[0][2]*m[1][1])*inv;
    r[1][0]=(m[1][2]*m[2][0]-m[1][0]*m[2][2])*inv; r[1][1]=(m[0][0]*m[2][2]-m[0][2]*m[2][0])*inv; r[1][2]=(m[0][2]*m[1][0]-m[0][0]*m[1][2])*inv;
    r[2][0]=(m[1][0]*m[2][1]-m[1][1]*m[2][0])*inv; r[2][1]=(m[0][1]*m[2][0]-m[0][0]*m[2][1])*inv; r[2][2]=(m[0][0]*m[1][1]-m[0][1]*m[1][0])*inv;
    return r;
}
inline Matrix AffineInverse(const Matrix& m){
    Matrix r=Inverse3(m); // fills the 3x3; translation row below
    r[3][0]=-(m[3][0]*r[0][0]+m[3][1]*r[1][0]+m[3][2]*r[2][0]);
    r[3][1]=-(m[3][0]*r[0][1]+m[3][1]*r[1][1]+m[3][2]*r[2][1]);
    r[3][2]=-(m[3][0]*r[0][2]+m[3][1]*r[1][2]+m[3][2]*r[2][2]);
    return r;
}
inline Vector3 TransformPoint(Vector3 v,const Matrix& m){
    return {v.x*m[0][0]+v.y*m[1][0]+v.z*m[2][0]+m[3][0],
            v.x*m[0][1]+v.y*m[1][1]+v.z*m[2][1]+m[3][1],
            v.x*m[0][2]+v.y*m[1][2]+v.z*m[2][2]+m[3][2]};
}
}
