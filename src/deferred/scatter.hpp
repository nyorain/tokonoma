#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/debug.hpp>
#include <dlg/dlg.hpp>

using namespace doi::types;

// no final pass idea yet. Should it work for all lights? store color
// or just r8 blend values for lights? maybe we can use some free
// alpha slots in light or bloom buffers?
// postponed for now
