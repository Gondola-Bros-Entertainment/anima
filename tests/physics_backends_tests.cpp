#include <anima/physics.hpp>
#include <anima/physics2d.hpp>
#include <doctest/doctest.h>
#ifdef B2_IS_NULL
#error Box2D implementation headers leaked
#endif
#ifdef JPH_VERSION_MAJOR
#error Jolt implementation headers leaked
#endif

TEST_CASE("Jolt and Box2D worlds coexist and can be recreated") {
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        anima::physics::World volume;
        anima::physics2d::World plane;
        anima::physics::BodySettings a;
        a.motion = anima::physics::Motion::dynamic;
        anima::physics2d::BodySettings b;
        b.motion = anima::physics2d::Motion::dynamic;
        auto solid = volume.create(a);
        auto flat = plane.create(b);
        volume.step(1. / 60);
        plane.step(1. / 60);
        CHECK(solid.valid());
        CHECK(flat.valid());
        CHECK(solid.pose().position.y < 0);
        CHECK(flat.pose().position.y < 0);
        flat.remove();
        CHECK_MESSAGE(solid.valid(), "2D removal invalidated 3D body");
    }
}
