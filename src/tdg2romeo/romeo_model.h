#ifndef TDG2ROMEO_ROMEO_MODEL_H
#define TDG2ROMEO_ROMEO_MODEL_H

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace romeo {

struct RomeoTimeInterval {
  int earliest = 0;
  int latest = 0;
  bool left_open = false;
  bool right_open = false;

  static RomeoTimeInterval immediate() {
    return {0, 0, false, false};
  }

  static RomeoTimeInterval point(int value) {
    return {value, value, false, false};
  }

  static RomeoTimeInterval closed(int earliest_value, int latest_value) {
    return {earliest_value, latest_value, false, false};
  }
};

struct RomeoAssignment {
  std::string place;
  int delta = 0;
};

struct RomeoTransition {
  std::string name;
  RomeoTimeInterval interval = RomeoTimeInterval::immediate();
  std::optional<int> priority;
  std::optional<std::string> allow;
  std::vector<RomeoAssignment> intermediate;
  std::string when_guard = "true";
  std::vector<RomeoAssignment> updates;
};

struct RomeoPlace {
  std::string name;
  int initial_tokens = 0;
};

struct RomeoModel {
  std::vector<RomeoPlace> places;
  std::vector<RomeoTransition> transitions;
};

class RomeoModelBuilder {
 public:
  std::string& place(const std::string& name, int initial = 0);
  void add_transition(RomeoTransition transition);

  RomeoModel build() const {
    return {places_, transitions_};
  }

 private:
  std::vector<RomeoPlace> places_;
  std::vector<RomeoTransition> transitions_;
};

std::string render_romeo_cts(const RomeoModel& model);

}  // namespace romeo

#endif  // TDG2ROMEO_ROMEO_MODEL_H
