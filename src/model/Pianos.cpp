#include "model/PianoData.hpp"

#include "model/pianos/meta/CalibrationData.hpp"
#include "model/pianos/meta/PartialTuning.hpp"
#include "model/pianos/meta/SpectrumCalibration.hpp"
#include "model/pianos/steinway/CalibrationData.hpp"
#include "model/pianos/steinway/PartialTuning.hpp"
#include "model/pianos/steinway/SpectrumCalibration.hpp"

#include <stdexcept>

namespace pianocraft {
namespace {

namespace meta = pianos::meta;
namespace steinway = pianos::steinway;

// Each piano's generated tables (src/model/pianos/<name>/), in PianoData's order. The first is the default.
const PianoData builtIn[] = {
    {"Upright", meta::measuredCalibration, meta::soundboardCalibration, meta::strikePositions, meta::strikeSpread,
     meta::radiationDb, meta::partialDb, meta::velocityDb, meta::brighteningDb, meta::promptDecayLog2,
     meta::aftersoundDecayLog2, meta::aftersoundLevelDb, meta::partialCents, 0.03, 1.0},
    {"Grand", steinway::measuredCalibration, steinway::soundboardCalibration, steinway::strikePositions,
     steinway::strikeSpread, steinway::radiationDb, steinway::partialDb, steinway::velocityDb, steinway::brighteningDb,
     steinway::promptDecayLog2, steinway::aftersoundDecayLog2, steinway::aftersoundLevelDb, steinway::partialCents,
     0.03, 0.29},
};

} // namespace

std::size_t pianoCount() {
    return sizeof(builtIn) / sizeof(builtIn[0]);
}

const PianoData& piano(std::size_t index) {
    if (index >= pianoCount()) {
        throw std::out_of_range("No such piano");
    }
    return builtIn[index];
}

} // namespace pianocraft
