#include "snapshot.hpp"

namespace blob {
bool fetch_depth_snapshot(Venue, const std::string&, int, DepthEvent&, std::string& err, std::string*) {
  err = "REST snapshot unavailable (built without ENABLE_LIVE)";
  return false;
}
}  // namespace blob
