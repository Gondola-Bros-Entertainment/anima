#include "viewer_options.hpp"
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
namespace anima::viewer {
std::uint64_t positive_number(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 || value > 1'000'000)
        throw std::invalid_argument("Expected an integer between 1 and 1000000");
    return value;
}
double nonnegative_number(std::string_view text) {
    double value = 0;
    std::istringstream parsed{std::string(text)};
    parsed.imbue(std::locale::classic());
    parsed >> std::noskipws >> value;
    if (!parsed || !parsed.eof() || !std::isfinite(value) || value < 0)
        throw std::invalid_argument("Expected a finite nonnegative number");
    return value;
}
bool parse_viewer_argument(ViewerOptions &options, int &i, int argc, char **argv) {
    const std::string_view argument{argv[i]};
    const auto next = [&]() -> std::string_view {
        if (++i >= argc)
            throw std::invalid_argument("Missing value for " + std::string(argument));
        return argv[i];
    };
    if (argument == "--help" || argument == "-h")
        options.help = true;
    else if (argument == "--validation")
        options.renderer.validation = true;
    else if (argument == "--empty")
        options.empty = true;
    else if (argument == "--frames")
        options.frames = positive_number(next());
    else if (argument == "--timeout")
        options.timeout_seconds = positive_number(next());
    else if (argument == "--capture")
        options.renderer.capture = next();
    else if (argument == "--asset")
        options.asset = next();
    else if (argument == "--manifest")
        options.manifest = next();
    else if (argument == "--clip")
        options.clip = next();
    else if (argument == "--time")
        options.pose_time = nonnegative_number(next());
    else if (argument == "--paused")
        options.paused = true;
    else if (argument == "--no-present-fences")
        options.renderer.disable_present_fences = true;
    else
        return false;
    return true;
}
void validate_viewer_options(ViewerOptions &options) {
    if (options.empty && (!options.asset.empty() || !options.manifest.empty()))
        throw std::invalid_argument("--empty cannot be combined with an asset or manifest");
    options.renderer.diagnostic_triangle = !options.empty;
    if (!options.asset.empty() && !options.manifest.empty())
        throw std::invalid_argument(
            "Use either --asset for static preview or --manifest for animated equipment preview");
    if (options.manifest.empty() && (!options.clip.empty() || options.paused || options.pose_time))
        throw std::invalid_argument("Playback options require --manifest");
}
ViewerOptions parse_viewer_options(int argc, char **argv) {
    ViewerOptions options;
    for (int i = 1; i < argc; ++i)
        if (!parse_viewer_argument(options, i, argc, argv))
            throw std::invalid_argument("Unknown option: " + std::string(argv[i]));
    validate_viewer_options(options);
    return options;
}
std::string_view viewer_usage() {
    return "Anima SDL3 / Vulkan foundation\n"
           "  --manifest PATH    Model from its asset manifest (static when no clips)\n"
           "  --clip NAME        Initial clip (default first declared); --paused holds the selected pose\n"
           "  --time SECONDS     Initial pose time (silent seek)\n"
           "  --asset FILE.glb   Preview an embedded GLB in its static default/bind pose\n"
           "  --empty            Clear the background (the default viewer shows a diagnostic triangle)\n"
           "  --validation       Require Khronos validation; fail on warnings/errors\n"
           "  --frames N         Exit after N presented frames (20 second deadline by default)\n"
           "  --timeout N        Frame deadline in seconds\n"
           "  --capture PATH     Save the first rendered frame as RGB PPM\n"
           "  --no-present-fences Exercise Vulkan 1.1 retirement fallback\n"
           "Drag/arrows: orbit. Wheel: zoom. F: frame. Escape: quit.\n"
           "1/2/3 manifest clip / Space pause-play / R restart / B bind pose\n";
}
} // namespace anima::viewer
