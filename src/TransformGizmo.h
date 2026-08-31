#pragma once
#include "core/GameObject.h"
#include "ViewportCamera.h"
#include <optional>
#include <vector>

namespace gizmo
{
    enum class Mode { Move, Rotate, Scale };
    struct Point { float x=0,y=0; };
    struct Segment { zengine::Vec3 a,b; int axis; };
    struct Shape
    {
        std::vector<Segment> lines;
        zengine::Vec3 center;
        zengine::Vec3 axes[3];
        float length=1;
    };
    struct Hit { int axis; std::size_t segment; float fraction; };
    std::optional<Point> Project(const ViewportCamera&,zengine::Vec3);
    Shape Build(const ViewportCamera&,const zengine::Transform&,Mode);
    std::optional<Hit> Pick(const ViewportCamera&,const Shape&,Point,float tolerance=8);

    // A transaction snapshots the target transform/camera; callers own object identity and cancellation.
    class Drag
    {
    public:
        Drag(const ViewportCamera&,const zengine::Transform&,Mode,const Shape&,Hit,Point);
        zengine::Transform Update(Point);
        int Axis() const { return axis_; }
        const zengine::Transform& Original() const { return original_; }
    private:
        ViewportCamera camera_;
        zengine::Transform original_;
        Mode mode_;
        Shape shape_;
        int axis_;
        Point start_, tangent_;
        float startParameter_=0, angle_=0, previousAngle_=0;
        bool planar_=false;
        std::optional<float> AxisParameter(Point) const;
        std::optional<float> RingAngle(Point) const;
    };
}
