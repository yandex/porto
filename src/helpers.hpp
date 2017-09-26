#include <cgroup.hpp>
#include <string>
#include <vector>
#include "util/path.hpp"

TError RunCommand(const std::vector<std::string> &command,
                  const TFile &dir = TFile(),
                  const TFile &input = TFile(),
                  const TFile &output = TFile());
TError CopyRecursive(const TPath &src, const TPath &dst);
TError ClearRecursive(const TPath &path);
TError RemoveRecursive(const TPath &path);
