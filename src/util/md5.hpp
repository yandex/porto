#pragma once

#include "util/path.hpp"

TError Md5Sum(TFile &file, std::string &sum);
void Md5Sum(const std::string &value, std::string &sum);
void Md5Sum(const std::string &salt, const std::string &value, std::string &sum);

std::string GenerateSalt();
