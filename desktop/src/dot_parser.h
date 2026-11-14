#pragma once

#include <QString>

#include "app_model.h"

namespace dot {

PetriNet parsePetri(const QString& text);
StateGraph parseStateGraph(const QString& text);

}  // namespace dot
