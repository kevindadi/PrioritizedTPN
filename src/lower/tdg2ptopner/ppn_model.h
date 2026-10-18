#ifndef PPN_MODEL_H
#define PPN_MODEL_H

#include <string>
#include <vector>

namespace ptopner_export {

struct PpnTransition {
  std::string name;
  std::vector<short> preset;
  std::vector<short> postset;
  int time = 0;
  float prior = 0.0F;
  bool is_suspend = false;
};

struct PpnPlace {
  std::string name;
  short token = 0;
};

struct PpnModel {
  std::vector<PpnPlace> places;
  std::vector<PpnTransition> transitions;
};

}  // namespace ptopner_export

#endif  // PPN_MODEL_H
