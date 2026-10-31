#pragma once
#include <opxy/draw.hpp>
#include <opxy/meter.hpp>

namespace perfmon {
const opxy::u32 kRows = 10;
using Frame = opxy::draw::Frame<0, 0, 1, 32, 1>;
struct DisplayState { opxy::u32 frames, first; };
}
