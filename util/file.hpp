#pragma once

#include <string>
#include <vector>

#include "error.hpp"
#include "util/path.hpp"

class TFile {
    const TPath Path;
    const int Mode; // currently used only by WriteStringNoAppend

    TError Write(int flags, const std::string &str) const;

public:
    TFile(const TPath &path, const int mode = 0600) : Path(path), Mode(mode) { };

    const TPath &GetPath() const { return Path; }
    bool Exists() const { return Path.Exists(); }

    TError Touch() const;
    TError Remove(bool silent = false) const;

    TError AsString(std::string &value) const;
    TError AsInt(int &value) const;
    TError AsUint64(uint64_t &value) const;
    TError AsLines(std::vector<std::string> &value) const;
    TError LastStrings(const size_t size, std::string &value) const;

    TError WriteStringNoAppend(const std::string &str) const;
    TError AppendString(const std::string &str) const;
    TError Truncate(size_t size) const;
    off_t GetSize() const;
};
