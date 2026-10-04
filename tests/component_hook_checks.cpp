// Attaches a component whose hooks ANIMA_HOOK_CASE selects. The matching case compiles, and each
// other case declares a hook the scene would never call, which add_component rejects at compile
// time; CTest builds each case and expects its static_assert message.
#include <anima/scene.hpp>

namespace {
struct Hooked {
#if ANIMA_HOOK_CASE == 0
    void on_update(double) {}
    void on_late_update(double) const {}
    void on_fixed_update(double) {}
    void on_enable() noexcept {}
    void on_disable() noexcept {}
    bool update() { return true; }
#elif ANIMA_HOOK_CASE == 1
    bool on_update(double) { return true; }
#elif ANIMA_HOOK_CASE == 2
    void on_update() {}
#elif ANIMA_HOOK_CASE == 3
    void on_late_update(double, int) {}
#elif ANIMA_HOOK_CASE == 4
    void on_fixed_update() {}
    void on_fixed_update(int, int) {}
#elif ANIMA_HOOK_CASE == 5
    bool on_enable() noexcept { return true; }
#elif ANIMA_HOOK_CASE == 6
    void on_disable(double) noexcept {}
#endif
};
} // namespace

void attach_hooked(anima::GameObject object);
void attach_hooked(anima::GameObject object) { (void)object.add_component<Hooked>(); }
