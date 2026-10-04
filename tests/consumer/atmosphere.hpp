#pragma once
// The sky that an enabled anima::Atmosphere draws, against atmosphere_reference.hpp's independent evaluation of the
// documented integral: pixels toward the sun and away from it, near the horizon, high, near the zenith and on the
// ground below the horizon, from an eye 1.7 m up under suns from noon to 6 degrees below the horizon, and from 2 km up
// in a hazy atmosphere. Then the horizon reddening toward a low sun and the sky darkening through twilight; surfaces
// lit by atmosphere_sunlight(), which follows each of its inputs the frame it changes; tables rebuilt when the medium
// changes and the first frame back when it returns, left as they were when any other input changes, and the sky view
// table redrawn when one of its inputs changes; the same frame when the eye and the ground move together; the sky seen
// from 1 m below the top of an atmosphere thinned under a distant eye; the time and memory the tables take, with frames
// that change none of their inputs dispatching nothing, frames that only turn the camera among them, and frames that
// raise a distant eye by about twice the tolerance redrawing the sky view table; and the time set_environment() takes,
// which integrates the sun's transmittance again only when an input of the sunlight changes.
#include "atmosphere_reference.hpp"
#include "blending.hpp"
#include "gpu_checks.hpp"
#include <algorithm>
#include <anima/environment.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atmosphere_test {
using atmosphere_reference::Direction;
using atmosphere_reference::Rgb;
using blending_test::require;

// A sampled direction, in degrees above the horizontal and turned from the sun's azimuth.
struct Sample {
    double elevation, azimuth;
};
// A view from the eye, pitched and turned from the sun's azimuth in degrees, and the directions sampled in it.
struct View {
    std::string name;
    double pitch, azimuth;
    std::vector<Sample> samples;
};
// Toward the sun, 25 degrees aside so that its disc and the steepest part of its halo stay out, and away from it: on
// the ground below the horizon, near the horizon and above it, then high, then near the zenith.
inline std::vector<View> views(bool zenith) {
    std::vector<View> result{{"toward", 0, 25, {{-10, 25}, {3, 25}, {15, 25}}},
                             {"away", 0, 180, {{-10, 180}, {3, 180}, {15, 180}}},
                             {"toward-high", 45, 25, {{45, 25}}},
                             {"away-high", 45, 180, {{45, 180}}}};
    if (zenith)
        result.push_back({"zenith", 70, 180, {{88, 180}}});
    return result;
}
// Relative tolerance and display levels either way, which the tables' resolution and half-precision storage need.
constexpr double relative = .04, levels = 2;

inline double decoded(int level) {
    const auto value = level / 255.;
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
inline double luminance(const Rgb &c) { return .2126 * c[0] + .7152 * c[1] + .0722 * c[2]; }
inline double channel(anima::Vec3 v, std::size_t c) { return c == 0 ? v.x : c == 1 ? v.y : v.z; }
inline std::string text(const Rgb &c) {
    std::ostringstream out;
    out << c[0] << ' ' << c[1] << ' ' << c[2];
    return out.str();
}

class Rig {
  public:
    explicit Rig(const std::filesystem::path &output)
        : window_(gpu_check::window("Anima atmosphere verification", 800, 600)), renderer(window_.get(), options()),
          images(output) {}
    // Draws one frame of @p scenes through @p view_projection in @p environment, and reads it back as @p name unless
    // the name is empty.
    void draw(const std::string &name, const anima::Mat4 &view_projection, const anima::Environment &environment,
              std::vector<std::shared_ptr<const anima::Scene>> scenes = {}) {
        const auto before = std::chrono::steady_clock::now();
        renderer.set_environment(environment);
        environment_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
        renderer.set_scenes(std::move(scenes));
        renderer.set_view(view_projection);
        if (!name.empty())
            renderer.request_capture();
        for (;;) {
            require(std::chrono::steady_clock::now() - began_ < gpu_check::watchdog, "Atmosphere watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Atmosphere check interrupted");
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        if (!name.empty())
            images.add(name, gpu_check::take(renderer));
    }
    // The size of every capture, from a first frame.
    [[nodiscard]] std::pair<std::size_t, std::size_t> size() {
        if (!width_) {
            draw("size", anima::perspective(std::numbers::pi_v<float> / 4, aspect(), .1F, 1), anima::Environment{});
            width_ = images["size"].width;
            height_ = images["size"].height;
            images.discard({"size"});
        }
        return {width_, height_};
    }
    [[nodiscard]] float aspect() const {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Atmosphere window has no drawable size");
        return float(width) / float(height);
    }

  private:
    static anima::RendererOptions options() {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.profile = true;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    std::size_t width_{}, height_{};
    std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();

  public:
    anima::VulkanRenderer renderer;
    gpu_check::Captures images;
    // Microseconds that the latest draw()'s call to set_environment() took.
    double environment_us{};
};

inline anima::Vec3 vec(const Direction &d) { return {float(d.x), float(d.y), float(d.z)}; }
inline anima::Mat4 camera(float aspect, anima::Vec3 eye, double pitch, double azimuth) {
    return anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 1000) *
           anima::look_at(eye, eye + vec(atmosphere_reference::direction(pitch, azimuth)));
}
// The pixel of a @p width by @p height image that shows @p world through @p view_projection.
inline blending_test::Pixel pixel_at(const anima::Mat4 &view_projection, anima::Vec3 world, std::size_t width,
                                     std::size_t height) {
    gpu_check::Image frame;
    frame.width = static_cast<decltype(frame.width)>(width);
    frame.height = static_cast<decltype(frame.height)>(height);
    return blending_test::pixel_of(view_projection, world, frame);
}
// The unit direction from @p eye through the center of @p pixel of a @p width by @p height image drawn through the
// view-projection whose inverse is @p inverse, unprojected in double precision: the ray the sky shader forms from its
// matrix relative to the eye.
inline Direction ray(const anima::Mat4 &inverse, anima::Vec3 eye, blending_test::Pixel pixel, std::size_t width,
                     std::size_t height) {
    const auto x = (double(pixel[0]) + .5) / double(width) * 2 - 1,
               y = (double(pixel[1]) + .5) / double(height) * 2 - 1;
    std::array<double, 4> point{};
    for (unsigned row = 0; row < 4; ++row)
        point[row] = double(inverse[row]) * x + double(inverse[4 + row]) * y + double(inverse[8 + row]) * .5 +
                     double(inverse[12 + row]);
    const Direction d{point[0] / point[3] - eye.x, point[1] / point[3] - eye.y, point[2] / point[3] - eye.z};
    const auto length = std::hypot(d.x, d.y, d.z);
    return {d.x / length, d.y / length, d.z / length};
}

// One sampled pixel: where it is, the reference radiance of its ray per unit of sunlight, and what it showed.
struct Measured {
    blending_test::Pixel pixel;
    Rgb expected, shown;
};
// Draws every view of @p sky from @p eye, @p altitude above the ground, under a sun @p sun_elevation degrees up, each
// exposed so that its brightest sample shows about half of display white, and requires each sample within the
// tolerance of the reference. Returns the samples' radiance per unit of the sun's irradiance, by view and sample.
inline std::vector<std::vector<Measured>> check_views(Rig &rig, const atmosphere_reference::Sky &sky,
                                                      anima::Environment environment, anima::Vec3 eye, double altitude,
                                                      double sun_elevation, const std::vector<View> &list,
                                                      const std::string &label, bool compare) {
    const auto sun = atmosphere_reference::direction(sun_elevation, 0);
    environment.sun.direction = vec(sun);
    const auto aspect = rig.aspect();
    std::vector<std::vector<Measured>> result;
    for (const auto &view : list) {
        const auto view_projection = camera(aspect, eye, view.pitch, view.azimuth);
        const auto inverse = anima::inverse(view_projection);
        const auto name = label + "-" + view.name;
        const auto [width, height] = rig.size();
        std::vector<Measured> measured;
        double brightest = 0;
        for (const auto &sample : view.samples) {
            const auto toward = atmosphere_reference::direction(sample.elevation, sample.azimuth);
            const auto at = eye + anima::Vec3{float(toward.x * 100), float(toward.y * 100), float(toward.z * 100)};
            const auto pixel = pixel_at(view_projection, at, width, height);
            const auto expected = sky.radiance(altitude, ray(inverse, eye, pixel, width, height), sun);
            for (std::size_t c = 0; c < 3; ++c)
                brightest = std::max(brightest, expected[c] * channel(environment.sun.irradiance, c));
            measured.push_back({pixel, expected, {}});
        }
        environment.exposure = brightest > 0 ? float(.5 / brightest) : 1.F;
        rig.draw(name, view_projection, environment);
        const auto &image = rig.images[name];
        for (std::size_t s = 0; s < measured.size(); ++s) {
            auto &m = measured[s];
            const auto shown = gpu_check::pixel(image, m.pixel[0], m.pixel[1]);
            bool matched = true;
            std::ostringstream report;
            report << label << ' ' << view.name << " sample " << view.samples[s].elevation << ','
                   << view.samples[s].azimuth << ": shows " << gpu_check::text(shown) << ", expected";
            double worst = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double scale = double(environment.exposure) * channel(environment.sun.irradiance, c);
                const double linear = m.expected[c] * scale;
                const double low = blending_test::encoded(linear * (1 - relative)) - levels,
                             high = blending_test::encoded(linear * (1 + relative)) + levels;
                matched = matched && shown[c] >= low && shown[c] <= high;
                report << ' ' << blending_test::encoded(linear);
                m.shown[c] = decoded(shown[c]) / scale;
                if (linear > .02)
                    worst = std::max(worst, std::abs(decoded(shown[c]) - linear) / linear);
            }
            report << " (radiance per unit sun " << text(m.expected) << "; largest relative error " << worst << ")";
            std::cout << "ATMOSPHERE " << report.str() << '\n';
            if (compare)
                rig.images.require(matched, report.str(), {name});
        }
        rig.images.discard({std::string_view(name)});
        result.push_back(std::move(measured));
    }
    return result;
}

inline anima::Environment earth() {
    anima::Environment environment;
    environment.atmosphere.enabled = true;
    environment.sun.irradiance = {1.2F, 1, .8F};
    environment.fill.irradiance = {0, 0, 0};
    environment.ambient_sky = environment.ambient_ground = environment.ambient_specular = {0, 0, 0};
    return environment;
}

// The sky from the ground under suns from noon to 6 degrees below the horizon, then the horizon's reddening toward a
// low sun and the darkening of the zenith and the sky opposite the sun through twilight, down to 12 degrees below,
// where the sky falls below half precision's normal range and only its order is checked.
inline void check_earth(Rig &rig) {
    const auto environment = earth();
    const atmosphere_reference::Sky sky(environment.atmosphere);
    constexpr anima::Vec3 eye{0, 1.7F, 0};
    std::vector<std::pair<double, std::vector<std::vector<Measured>>>> by_sun;
    for (const double sun : {90.0, 45.0, 10.0, 2.0, -2.0, -6.0, -12.0})
        by_sun.emplace_back(sun, check_views(rig, sky, environment, eye, 1.7, sun, views(sun != 90),
                                             "earth-" + std::to_string(int(sun)), sun > -12));
    const auto find = [&](double sun) -> const std::vector<std::vector<Measured>> & {
        for (const auto &[elevation, measured] : by_sun)
            if (elevation == sun)
                return measured;
        throw std::logic_error("No such sun");
    };
    // Toward the sun 3 degrees up, the second sample of the first view.
    const auto reddening = [&](double sun) {
        const auto &c = find(sun)[0][1].shown;
        return c[0] / c[2];
    };
    std::cout << "ATMOSPHERE {\"horizon_red_to_blue_noon\":" << reddening(90)
              << ",\"horizon_red_to_blue_2\":" << reddening(2) << "}\n";
    require(reddening(2) >= 4 * reddening(90),
            "The horizon toward a low sun is not red enough against noon's: " + std::to_string(reddening(2)) +
                " against " + std::to_string(reddening(90)));
    double zenith = std::numeric_limits<double>::infinity(), opposite = zenith;
    for (const double sun : {2.0, -2.0, -6.0, -12.0}) {
        const auto &measured = find(sun);
        const auto now_zenith = luminance(measured[4][0].shown), now_opposite = luminance(measured[1][2].shown);
        std::cout << "ATMOSPHERE {\"sun\":" << sun << ",\"zenith_luminance\":" << now_zenith
                  << ",\"opposite_luminance\":" << now_opposite << "}\n";
        require(now_zenith < zenith && now_opposite < opposite,
                "The sky did not darken as the sun set to " + std::to_string(sun) + " degrees");
        zenith = now_zenith;
        opposite = now_opposite;
    }
}

// A hazy atmosphere with thicker, less forward Mie scattering over a darker ground, seen from 2 km up, so that the
// eye's altitude and every field reach the tables.
inline void check_haze(Rig &rig) {
    auto environment = earth();
    auto &a = environment.atmosphere;
    a.mie_scattering = {2e-5F, 2e-5F, 2e-5F};
    a.mie_absorption = {4e-6F, 4e-6F, 4e-6F};
    a.mie_anisotropy = .6F;
    a.ground_albedo = {.1F, .15F, .2F};
    a.rayleigh_scattering = {6e-6F, 1.4e-5F, 3e-5F};
    a.ground_height = 100;
    const atmosphere_reference::Sky sky(a);
    check_views(rig, sky, environment, {0, 2'100, 0}, 2'000, 20, views(false), "haze", true);
}

// A lit quad facing a sun behind the eye shows atmosphere_sunlight(): the same pixels as with the atmosphere disabled
// and the sun's irradiance set to it, which differ from the sun's irradiance itself.
inline void check_sunlight(Rig &rig) {
    const auto view_projection = camera(rig.aspect(), {0, 1.7F, 0}, 0, 0);
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, blending_test::facing(blending_test::opaque({.8, .8, .8}, true), {0, 1.7F, -5}, 1, 1));
    const anima::Vec3 center{0, 1.7F, -5};
    for (const double sun : {10.0, 2.0}) {
        auto enabled = earth();
        enabled.sun.irradiance = {3, 3, 3};
        enabled.sun.direction = vec(atmosphere_reference::direction(sun, 180));
        auto disabled = enabled;
        disabled.atmosphere.enabled = false;
        disabled.sun.irradiance = anima::atmosphere_sunlight(enabled);
        auto unattenuated = enabled;
        unattenuated.atmosphere.enabled = false;
        const auto name = "sunlit-" + std::to_string(int(sun));
        rig.draw(name, view_projection, enabled, {scene});
        rig.draw(name + "-disabled", view_projection, disabled, {scene});
        rig.draw(name + "-unattenuated", view_projection, unattenuated, {scene});
        const auto at = blending_test::pixel_of(view_projection, center, rig.images[name]);
        const auto lit = gpu_check::pixel(rig.images[name], at[0], at[1]);
        const auto same = gpu_check::pixel(rig.images[name + "-disabled"], at[0], at[1]);
        const auto brighter = gpu_check::pixel(rig.images[name + "-unattenuated"], at[0], at[1]);
        std::cout << "ATMOSPHERE sunlit quad at " << sun << " degrees: " << gpu_check::text(lit) << ", disabled with "
                  << "atmosphere_sunlight() " << gpu_check::text(same) << ", unattenuated " << gpu_check::text(brighter)
                  << '\n';
        rig.images.require(lit[0] > 20 && gpu_check::difference(lit, same) <= 1,
                           "A quad lit through the atmosphere does not show atmosphere_sunlight()",
                           {name, name + "-disabled"});
        rig.images.require(gpu_check::difference(lit, brighter) > 10,
                           "The atmosphere did not dim the sun on a lit quad, so the check is blind",
                           {name, name + "-unattenuated"});
        rig.images.discard({name, name + "-disabled", name + "-unattenuated"});
    }
}

// The sun at the ground follows each input of atmosphere_sunlight() from the frame it changes: a lit quad drawn right
// after a change shows the changed environment's sunlight, the same pixels as with the atmosphere disabled and the
// sun's irradiance set to it, and not the sunlight from before the change, which differs. The inputs are the sun's
// direction and irradiance, the sun's angular radius, which only a sun on the horizon shows, the medium, here its Mie
// scattering, and whether the atmosphere is enabled. The two frames that the changed one is compared with each follow
// a frame that differs from them in every one of those inputs, so that even a renderer that misses one of them lights
// them afresh. Setting the same environment again draws the same frame.
inline void check_sunlight_updates(Rig &rig) {
    const auto view_projection = camera(rig.aspect(), {0, 1.7F, 0}, 0, 0);
    auto scene = std::make_shared<anima::Scene>();
    const anima::Vec3 center{0, 1.7F, -5};
    (void)scene->create({}, blending_test::facing(blending_test::opaque({.8, .8, .8}, true), center, 1, 1));
    // Behind the eye, so that the quad faces it.
    const auto behind = [](double elevation) { return vec(atmosphere_reference::direction(elevation, 180)); };
    auto high = earth();
    high.sun.irradiance = {3, 3, 3};
    high.sun.direction = behind(10);
    // A tenth of a degree below the horizon, where about a third of the disc shows, bright enough to light the quad.
    auto low = high;
    low.sun.direction = behind(-.1);
    low.sun.irradiance = {30, 30, 30};
    // Differs from every environment below in each input of the sunlight.
    auto other = earth();
    other.sun.direction = behind(30);
    other.sun.irradiance = {1, 1, 1};
    other.atmosphere.sun_angular_radius = .01F;
    other.atmosphere.mie_scattering = {1e-5F, 1e-5F, 1e-5F};
    rig.draw("first", view_projection, high, {scene});
    rig.draw("again", view_projection, high, {scene});
    rig.images.require_same("first", "again", "Setting the same environment again changed the frame");
    rig.images.discard({"first", "again"});
    struct Change {
        const char *input;
        const anima::Environment &before;
        void (*apply)(anima::Environment &);
    };
    using anima::Environment;
    const Change changes[]{
        {"sun.direction", high, [](Environment &e) { e.sun.direction = vec(atmosphere_reference::direction(3, 180)); }},
        {"sun.irradiance", high, [](Environment &e) { e.sun.irradiance = {2, 2, 2}; }},
        {"atmosphere.mie_scattering", high,
         [](Environment &e) { e.atmosphere.mie_scattering = {5e-5F, 5e-5F, 5e-5F}; }},
        {"atmosphere.sun_angular_radius", low, [](Environment &e) { e.atmosphere.sun_angular_radius = .02F; }},
        {"atmosphere.enabled", high, [](Environment &e) { e.atmosphere.enabled = false; }},
    };
    for (const auto &change : changes) {
        auto after = change.before;
        change.apply(after);
        // The changed environment with the atmosphere disabled and the sun's irradiance set to @p sunlight.
        const auto lit_by = [&](anima::Vec3 sunlight) {
            auto disabled = after;
            disabled.atmosphere.enabled = false;
            disabled.sun.irradiance = sunlight;
            return disabled;
        };
        rig.draw("", view_projection, change.before, {scene});
        rig.draw("changed", view_projection, after, {scene});
        rig.draw("", view_projection, other, {scene});
        rig.draw("expected", view_projection, lit_by(anima::atmosphere_sunlight(after)), {scene});
        rig.draw("", view_projection, other, {scene});
        rig.draw("stale", view_projection, lit_by(anima::atmosphere_sunlight(change.before)), {scene});
        const auto at = blending_test::pixel_of(view_projection, center, rig.images["changed"]);
        const auto shown = gpu_check::pixel(rig.images["changed"], at[0], at[1]);
        const auto expected = gpu_check::pixel(rig.images["expected"], at[0], at[1]);
        const auto stale = gpu_check::pixel(rig.images["stale"], at[0], at[1]);
        std::cout << "ATMOSPHERE sunlit quad after changing " << change.input << ": " << gpu_check::text(shown)
                  << ", expected " << gpu_check::text(expected) << ", with the sunlight from before "
                  << gpu_check::text(stale) << '\n';
        rig.images.require(expected[0] > 20 && gpu_check::difference(shown, expected) <= 1,
                           std::string("A quad lit right after changing ") + change.input +
                               " does not show atmosphere_sunlight() of the changed environment",
                           {"changed", "expected"});
        rig.images.require(gpu_check::difference(expected, stale) > 10,
                           std::string("Changing ") + change.input +
                               " hardly changed the sunlight on the quad, so the check is blind",
                           {"expected", "stale"});
        rig.images.discard({"changed", "expected", "stale"});
    }
}

// What reads an input of the sky: the transmittance and multiple scattering tables, which a change to it must rebuild,
// or only the sky view table and the sky, which follow it without that rebuild.
enum class Reader { tables, sky };

// Each field that the transmittance and multiple scattering tables read rebuilds them when it changes: the frame drawn
// right after the change equals the one drawn once the tables were built afresh for the same medium, by way of another
// medium that differs in a second field. Tables left stale would draw the new medium's sky view over the old medium's
// tables, which differs, since each change is large and the sun low, where the tables weigh most. The same comparison
// shows that the tables read none of the sky's other inputs, which do not rebuild them: the ground's height, through
// the eye's altitude above it, Mie scattering's asymmetry, the sun's angular radius, and its direction and irradiance.
// Since the renderer redraws the sky view table only on a frame that changes what it reads, the comparison also shows
// that the eye's altitude, Mie scattering's asymmetry and the sun's direction each redraw it: a table left as it was
// would show the sky from before the change. The changes cover every field of Atmosphere but `enabled`, which is true
// whenever the tables are built. Each change must alter a least share of the frame, so that neither comparison passes
// for a renderer that ignores the input. Moving the eye and the ground together leaves the frame as it was, to within
// a level of rounding.
inline void check_updates(Rig &rig) {
    const auto view_projection = camera(rig.aspect(), {0, 1.7F, 0}, 10, 25);
    auto environment = earth();
    environment.sun.direction = vec(atmosphere_reference::direction(5, 0));
    environment.exposure = 6;
    // Five times Earth's ozone, so that moving and narrowing its layer shows.
    environment.atmosphere.ozone_absorption = {3.25e-6F, 9.4e-6F, 4.25e-7F};
    // The least share of the frame's pixels that a change must alter by more than 3 levels, so that the comparisons
    // after it cannot pass for a renderer that ignores it: most of the sky for most changes, and for the sun's angular
    // radius the ring that its disc gains when the radius grows from 0.0047 to 0.02 radians, about 0.18% of the frame.
    constexpr double sky = .05, disc = 5e-4;
    struct Change {
        const char *input;
        Reader reader;
        double least;
        void (*apply)(anima::Environment &);
    };
    using anima::Environment;
    const Change changes[]{
        {"atmosphere.planet_radius", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.planet_radius = 3'000'000; }},
        {"atmosphere.thickness", Reader::tables, sky, [](Environment &e) { e.atmosphere.thickness = 20'000; }},
        {"atmosphere.rayleigh_scattering", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.rayleigh_scattering = {1.2e-5F, 2.7e-5F, 6.6e-5F}; }},
        {"atmosphere.rayleigh_scale_height", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.rayleigh_scale_height = 16'000; }},
        {"atmosphere.mie_scattering", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.mie_scattering = {2e-5F, 2e-5F, 2e-5F}; }},
        {"atmosphere.mie_absorption", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.mie_absorption = {1e-5F, 1e-5F, 1e-5F}; }},
        {"atmosphere.mie_scale_height", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.mie_scale_height = 4'000; }},
        {"atmosphere.ozone_absorption", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.ozone_absorption = {0, 0, 0}; }},
        {"atmosphere.ozone_altitude", Reader::tables, sky, [](Environment &e) { e.atmosphere.ozone_altitude = 8'000; }},
        {"atmosphere.ozone_width", Reader::tables, sky, [](Environment &e) { e.atmosphere.ozone_width = 6'000; }},
        {"atmosphere.ground_albedo", Reader::tables, sky,
         [](Environment &e) { e.atmosphere.ground_albedo = {1, 1, 1}; }},
        // The eye, 1.7 m above the ground, then 2,001.7 m.
        {"atmosphere.ground_height", Reader::sky, sky, [](Environment &e) { e.atmosphere.ground_height = -2'000; }},
        {"atmosphere.mie_anisotropy", Reader::sky, sky, [](Environment &e) { e.atmosphere.mie_anisotropy = .3F; }},
        {"atmosphere.sun_angular_radius", Reader::sky, disc,
         [](Environment &e) { e.atmosphere.sun_angular_radius = .02F; }},
        {"sun.direction", Reader::sky, sky,
         [](Environment &e) { e.sun.direction = vec(atmosphere_reference::direction(20, 0)); }},
        {"sun.irradiance", Reader::sky, sky, [](Environment &e) { e.sun.irradiance = {.6F, .5F, .4F}; }},
    };
    rig.draw("first", view_projection, environment);
    for (const auto &change : changes) {
        auto changed = environment;
        change.apply(changed);
        auto detour = changed;
        if (std::string_view(change.input) == "atmosphere.thickness")
            detour.atmosphere.planet_radius += 100'000;
        else
            detour.atmosphere.thickness += 5'000;
        rig.draw("first", view_projection, environment);
        rig.draw("changed", view_projection, changed);
        rig.draw("", view_projection, detour);
        rig.draw("rebuilt", view_projection, changed);
        if (change.reader == Reader::tables)
            rig.images.require_same("changed", "rebuilt",
                                    std::string("Changing ") + change.input +
                                        " did not rebuild the atmosphere's tables");
        else
            rig.images.require_same("changed", "rebuilt",
                                    std::string("Changing ") + change.input +
                                        " drew another sky than rebuilding every table: a table that it does not "
                                        "rebuild reads it, or the sky view table missed it");
        std::cout << "ATMOSPHERE changing " << change.input << " altered "
                  << gpu_check::changed(rig.images["first"], rig.images["changed"]) << " of the frame's pixels\n";
        rig.images.require_changed("first", "changed", change.least,
                                   std::string("Changing ") + change.input +
                                       " left the frame as it was, so comparing it with rebuilt tables is blind");
    }
    auto broader = environment;
    broader.atmosphere.mie_anisotropy = .3F;
    rig.draw("broader", view_projection, broader);
    rig.images.require_changed("first", "broader", .05, "A broader Mie phase function left the sky as it was");
    // Across the sun's vertical plane, so that no edge of its disc can turn on rounding, with the horizon in view: an
    // eye 1 km from the world's origin sees the horizon where one near it does.
    const auto level = camera(rig.aspect(), {0, 1.7F, 0}, 10, 90);
    rig.draw("level", level, environment);
    auto raised = environment;
    raised.atmosphere.ground_height = 1'000;
    rig.draw("raised", camera(rig.aspect(), {0, 1'001.7F, 0}, 10, 90), raised);
    const auto moved = gpu_check::changed(rig.images["level"], rig.images["raised"], 1);
    rig.images.require(moved == 0,
                       "Raising the eye and the ground together changed " + std::to_string(moved) +
                           " of the sky's pixels by more than a level",
                       {"level", "raised"});
    rig.images.discard({"first", "changed", "rebuilt", "broader", "level", "raised"});
}

// The altitude that the sky is seen from stays at least 1 m below the atmosphere's top while the renderer holds it.
// 10,000 km from the world's origin, where the renderer holds that altitude through changes of up to 9.5 m (8 float
// epsilons of the distance), an eye 50 m up, above an atmosphere 11 m thick, sees the sky from 10 m. When the
// atmosphere thins to 3 m, the eye's altitude falls to 2 m, within 9.5 m of the 10 m held, which now lies 7 m above the
// top. The frame must match the one that the thin atmosphere shows after a detour through one 1 km thick, inside which
// the eye's altitude of 50 m lies beyond the hold from either.
inline void check_thinned(Rig &rig) {
    constexpr anima::Vec3 eye{6'000'000, 50, -8'000'000};
    const auto view_projection = camera(rig.aspect(), eye, 10, 25);
    auto thin = earth();
    thin.exposure = 6;
    // A thousand times Earth's Rayleigh scattering, so that a few meters of the medium show.
    thin.atmosphere.rayleigh_scattering = {5.802e-3F, 13.558e-3F, 33.1e-3F};
    thin.atmosphere.thickness = 3;
    auto thicker = thin, detour = thin;
    thicker.atmosphere.thickness = 11;
    detour.atmosphere.thickness = 1'000;
    rig.draw("", view_projection, detour);
    rig.draw("thin", view_projection, thin);
    rig.draw("", view_projection, detour);
    rig.draw("", view_projection, thicker);
    rig.draw("thinned", view_projection, thin);
    rig.images.require_same("thin", "thinned",
                            "Thinning the atmosphere under a distant eye kept the sky seen from above the new top");
    rig.images.discard({"thin", "thinned"});
}

// What report_cost() changes every frame, in the order it measures them.
enum class Changing { nothing, view_direction, eye_altitude, distant_eye_altitude, sun_elevation, medium };

// The median of @p values, or -1 when there are none.
inline double median(std::vector<double> values) {
    if (values.empty())
        return -1;
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
    return values[values.size() / 2];
}

// The share of the time that some work takes, under which a frame or a call that should skip that work shows that it
// did. A skip leaves none of the work's time and doing the work again all of it, so half leaves room for noise both
// ways.
constexpr double skipped_share = .5;

// The tables' memory, the time their dispatches take and the CPU time that set_environment() takes, as medians of 60
// frames after 10 more: while nothing changes; while the camera turns in place 100 km from the world's origin; while
// the eye's altitude changes every frame, which the sky view table reads, near the origin and then 100 km from it;
// while the sun's elevation changes, which the sky view table and the sun at the ground read; and while the medium
// changes, which every table and the sun at the ground read. Where the device reports timestamps, it requires what
// FrameProfile states. The frames that change nothing and those that only turn the camera, whose timestamps around
// the atmosphere bracket no commands, dispatch nothing: of each, at most noisy_share may take skipped_share of the
// atmosphere time of the frames that move the eye near the origin, which redraw the 192 by 108 sky view table. A count
// rather than a median, which a renderer that redraws on every other frame can pass or fail by one frame. 100 km
// out, the eye that the renderer recovers from each float view-projection moves by a few millimeters as the camera
// turns, past the tolerance's 1 mm floor, so the part of the tolerance that grows with the eye's distance, 0.095 m
// there, carries the skip. Raising the eye there by distant_rise, about twice that part, redraws the table: those
// frames' median must exceed 1 / skipped_share times that of the frames that change nothing.
inline void report_cost(Rig &rig) {
    const auto stats = rig.renderer.resource_stats();
    constexpr std::uint64_t texels = 256 * 64 + 64 * 32 + 192 * 108;
    require(stats.atmosphere_bytes >= texels * 8,
            "The atmosphere's tables take fewer bytes than their half-float texels: " +
                std::to_string(stats.atmosphere_bytes));
    constexpr std::array modes{std::pair{Changing::nothing, "nothing"},
                               std::pair{Changing::view_direction, "view direction"},
                               std::pair{Changing::eye_altitude, "eye altitude"},
                               std::pair{Changing::distant_eye_altitude, "distant eye altitude"},
                               std::pair{Changing::sun_elevation, "sun elevation"},
                               std::pair{Changing::medium, "medium"}};
    constexpr int warmup_frames = 10, measured_frames = 60;
    // The distant eye, 100 km from the world's origin and 1.7 m up, the camera's turn there in each frame, and how far
    // the eye rises there every other frame.
    constexpr anima::Vec3 distant_eye{60'000, 1.7F, -80'000};
    constexpr double turn_degrees = .5;
    constexpr float distant_rise = .2F;
    // The share of the measured frames that dispatch nothing which may still take skipped_share of a redraw, a margin
    // for timing noise. A renderer that redraws on every other turning frame, as a tolerance of 1 mm alone does at the
    // distant eye, exceeds it five times over.
    constexpr double noisy_share = .1;
    std::array<std::vector<double>, modes.size()> atmosphere_times;
    for (const auto &[changing, name] : modes) {
        auto &atmosphere_ms = atmosphere_times[std::size_t(changing)];
        std::vector<double> gpu_ms, environment_us;
        for (int frame = 0; frame < warmup_frames + measured_frames; ++frame) {
            const bool odd = frame % 2 != 0;
            auto environment = earth();
            environment.sun.direction = vec(atmosphere_reference::direction(10, 0));
            anima::Vec3 eye{0, 1.7F, 0};
            double azimuth = 25;
            switch (changing) {
            case Changing::nothing:
                break;
            case Changing::view_direction:
                eye = distant_eye;
                azimuth += turn_degrees * frame;
                break;
            case Changing::eye_altitude:
                eye.y = odd ? 1.8F : 1.7F;
                break;
            case Changing::distant_eye_altitude:
                eye = distant_eye;
                if (odd)
                    eye.y += distant_rise;
                break;
            case Changing::sun_elevation:
                environment.sun.direction = vec(atmosphere_reference::direction(odd ? 10.5 : 10, 0));
                break;
            case Changing::medium:
                environment.atmosphere.rayleigh_scale_height = odd ? 8'000.F : 8'001.F;
                break;
            }
            rig.draw("", camera(rig.aspect(), eye, 10, azimuth), environment);
            const auto profile = rig.renderer.frame_profile();
            if (frame >= warmup_frames) {
                environment_us.push_back(rig.environment_us);
                if (profile.gpu_available) {
                    atmosphere_ms.push_back(profile.gpu_atmosphere_ms);
                    gpu_ms.push_back(profile.gpu_ms);
                }
            }
        }
        std::cout << "BENCH {\"atmosphere_changing\":\"" << name << "\",\"atmosphere_bytes\":" << stats.atmosphere_bytes
                  << ",\"atmosphere_ms\":" << median(atmosphere_ms) << ",\"gpu_ms\":" << median(gpu_ms)
                  << ",\"set_environment_us\":" << median(environment_us) << "}\n";
    }
    const auto times = [&](Changing changing) -> const std::vector<double> & {
        return atmosphere_times[std::size_t(changing)];
    };
    const auto skipping = median(times(Changing::nothing)), redrawing = median(times(Changing::eye_altitude)),
               rising = median(times(Changing::distant_eye_altitude));
    // Each is -1 without timestamps.
    if (skipping < 0 || redrawing < 0 || rising < 0)
        return;
    // The atmosphere time from which a frame counts as one that redrew the sky view table, and how many such frames
    // each mode that dispatches nothing may have.
    const double redrawn_from = skipped_share * redrawing;
    const auto noisy = std::size_t(noisy_share * measured_frames);
    const auto redrawn = [&](Changing changing) {
        return std::size_t(std::count_if(times(changing).begin(), times(changing).end(),
                                         [&](double ms) { return ms >= redrawn_from; }));
    };
    const auto still = redrawn(Changing::nothing), turned = redrawn(Changing::view_direction);
    std::cout << "ATMOSPHERE " << still << " frames that change nothing and " << turned
              << " that only turn the camera, of " << measured_frames << " each, took at least " << redrawn_from
              << " ms of atmosphere dispatches, half of a redraw\n";
    require(still <= noisy, std::to_string(still) + " of " + std::to_string(measured_frames) +
                                " frames that change none of the atmosphere's inputs took at least half of the " +
                                std::to_string(redrawing) +
                                " ms of atmosphere dispatches of frames that move the eye, more than " +
                                std::to_string(noisy) + ", so they still dispatch");
    require(turned <= noisy, std::to_string(turned) + " of " + std::to_string(measured_frames) +
                                 " frames that only turn the camera took at least half of the " +
                                 std::to_string(redrawing) +
                                 " ms of atmosphere dispatches of frames that move the eye, more than " +
                                 std::to_string(noisy) + ", so turning redraws the sky view table");
    require(rising > skipping / skipped_share,
            "Frames that raise the eye " + std::to_string(distant_rise) + " m 100 km from the world's origin took " +
                std::to_string(rising) + " ms of atmosphere dispatches, not more than twice the " +
                std::to_string(skipping) +
                " ms of frames that change nothing, so they did not redraw the sky view table");
}

// set_environment() integrates the sun's transmittance again only when an input of atmosphere_sunlight() changes: over
// 200 pairs of calls, each setting an environment whose sun has moved and then the same environment again, the median
// time of the repeated calls must stay under skipped_share of the changed calls'. The two calls of a pair run moments
// apart, so that the load on the machine, which the device's own threads add to on a software driver, weighs on both.
inline void check_sunlight_cache(Rig &rig) {
    constexpr int pairs = 200;
    auto low = earth();
    low.sun.direction = vec(atmosphere_reference::direction(10, 0));
    auto high = low;
    high.sun.direction = vec(atmosphere_reference::direction(10.5, 0));
    const auto microseconds = [&](const anima::Environment &environment) {
        const auto before = std::chrono::steady_clock::now();
        rig.renderer.set_environment(environment);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
    };
    std::vector<double> changed, repeated;
    for (int pair = 0; pair < pairs; ++pair) {
        const auto &environment = pair % 2 != 0 ? low : high;
        changed.push_back(microseconds(environment));
        repeated.push_back(microseconds(environment));
    }
    const auto integrating = median(changed), holding = median(repeated);
    std::cout << "ATMOSPHERE set_environment() took " << integrating << " microseconds after the sun moved and "
              << holding << " for the same environment again\n";
    require(holding < skipped_share * integrating,
            "Setting the same environment again took " + std::to_string(holding) +
                " microseconds, not under half of the " + std::to_string(integrating) +
                " that setting one whose sun moved took, so it integrated the sunlight again");
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --atmosphere OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Rig rig(argv[2]);
    check_earth(rig);
    check_haze(rig);
    check_sunlight(rig);
    check_sunlight_updates(rig);
    check_updates(rig);
    check_thinned(rig);
    report_cost(rig);
    check_sunlight_cache(rig);
    const auto stats = rig.renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Atmosphere validation failed");
    std::cout << "PASS atmosphere: the sky from the ground and from 2 km in haze matches an independent evaluation of "
                 "the documented integral, reddens toward a low sun and darkens through twilight, lit surfaces show "
                 "atmosphere_sunlight() from the frame its inputs change, the medium alone rebuilds its tables and the "
                 "sky follows the eye's altitude, staying 1 m below a thinned atmosphere's top, frames that change "
                 "none of the tables' inputs dispatch nothing, turning the camera among them, raising a distant eye by "
                 "about twice the tolerance redraws the sky, and set_environment() integrates the sunlight only when "
                 "its inputs change, with clean validation\n";
    return 0;
}
} // namespace atmosphere_test
