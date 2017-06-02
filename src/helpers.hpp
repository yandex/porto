#include <cgroup.hpp>
#include <string>
#include <vector>
#include "util/path.hpp"

TError RunCommand(const std::vector<std::string> &command, const TPath &cwd,
		  const TFile &input = TFile(), const TFile &output = TFile());
TError CopyRecursive(const TPath &src, const TPath &dst);
TError ClearRecursive(const TPath &path);
TError RemoveRecursive(const TPath &path);
TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target);
