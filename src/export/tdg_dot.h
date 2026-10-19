#ifndef EXPORT_TDG_DOT_H
#define EXPORT_TDG_DOT_H

#include <string>

namespace tdg {
class TDG;
}

namespace tdg {

// Renders the TDG as Graphviz DOT.
std::string to_dot_string(const TDG& tdg);

// Writes the Graphviz DOT export. Returns false when the file cannot be
// opened, written, flushed, or closed.
bool export_to_dot(const TDG& tdg, const std::string& output_path);

}  // namespace tdg

#endif  // EXPORT_TDG_DOT_H
