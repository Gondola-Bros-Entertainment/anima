#include <anima/physics.hpp>
#include <anima/physics2d.hpp>
#include <iostream>
#include <stdexcept>
#ifdef B2_IS_NULL
#error Box2D implementation headers leaked
#endif
#ifdef JPH_VERSION_MAJOR
#error Jolt implementation headers leaked
#endif
int main() {
    try {
        for (int i = 0; i < 3; ++i) {
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
            if (!solid.valid() || !flat.valid() || solid.pose().position.y >= 0 || flat.pose().position.y >= 0)
                throw std::runtime_error("Independent physics backends did not coexist");
            flat.remove();
            if (!solid.valid())
                throw std::runtime_error("2D removal invalidated 3D body");
        }
        std::cout << "PASS optional Jolt and Box2D coexistence and world recreation\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
