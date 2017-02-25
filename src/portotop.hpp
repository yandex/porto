#pragma once

#include <string>
#include <vector>
#include <list>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <unordered_set>

#include "util/namespace.hpp"
#include "util/string.hpp"
#include "libporto.hpp"

extern "C" {
#include <ncurses.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
};

class TColumn;
class TPortoValueCache;
class TPortoContainer;
class TCommonValue;
class TPortoValue;

class TConsoleScreen {
public:
    TConsoleScreen();
    ~TConsoleScreen();
    int Width();
    int Height();
    void SetTimeout(int ms);
    template<class T>
    void PrintAt(T arg, int x, int y, int width, bool leftaligned = false, int attr = 0);
    void PrintAt(std::string str0, int x0, int y0, int w0, bool leftaligned = false,
                 int attr = 0);
    void Refresh();
    void Erase();
    void Clear();
    int Getch();
    void Save();
    void Restore();
    int Dialog(std::string text, const std::vector<std::string> &buttons);
    void ErrorDialog(Porto::Connection &api);
    void ErrorDialog(std::string message, int error);
    void InfoDialog(std::vector<std::string> lines);
    void HelpDialog();
private:
    WINDOW *Wnd;
};

class TPortoContainer {
public:
    TPortoContainer(std::string container);
    static TPortoContainer* ContainerTree(Porto::Connection &api);
    ~TPortoContainer();
    std::string GetName();
    int GetLevel();
    void ForEach(std::function<void (TPortoContainer&)> fn, int maxlevel);
    void SortTree(TColumn &column);
    int GetMaxLevel();
    int ChildrenCount();
    std::string ContainerAt(int n, int max_level);
    TPortoContainer* GetRoot() const;
private:
    TPortoContainer* GetParent(int level);
    TPortoContainer* Root = nullptr;
    TPortoContainer* Parent = nullptr;
    std::list<TPortoContainer*> Children;
    std::string Container;
    int Level = 0;
};

class TPortoValueCache {
public:
    void Register(const std::string &container, const std::string &variable);
    void Unregister(const std::string &container, const std::string &variable);
    std::string GetValue(const std::string &container, const std::string &variable,
                         bool prev);
    uint64_t GetDt();
    int Update(Porto::Connection &api);
    std::string Version, Revision;
private:
    std::unordered_map<std::string, unsigned long> Containers;
    std::unordered_map<std::string, unsigned long> Variables;
    bool CacheSelector = false;
    std::map<std::string, std::map<std::string, Porto::GetResponse>> Cache[2];
    uint64_t Time[2] = {0, 0};
};

namespace ValueFlags {
    static const int Raw = 0x0;
    static const int Container = 0x1;
    static const int Map = 0x2;
    static const int DfDt = 0x4;
    static const int PartOfRoot = 0x8;
    static const int Seconds = 0x10;
    static const int Bytes = 0x20;
    static const int Percents = 0x40;
    static const int Multiplier = 0x80;
    static const int State = 0x100;
};

class TPortoValue {
public:
    TPortoValue();
    TPortoValue(const TPortoValue &src);
    TPortoValue(const TPortoValue &src, TPortoContainer *container);
    TPortoValue(TPortoValueCache &cache, TPortoContainer *container,
                const std::string &variable, int flags, double multiplier = 1);
    ~TPortoValue();
    void Process();
    std::string GetValue() const;
    int GetLength() const;
    bool operator< (const TPortoValue &v);
private:
    TPortoValueCache *Cache;
    TPortoContainer *Container;
    std::string Variable; // property or data

    int Flags;

    std::string AsString;
    double AsNumber;
    double Multiplier;
};

class TCommonValue {
public:
    TCommonValue(const std::string &label, const TPortoValue &val);
    std::string GetLabel();
    TPortoValue& GetValue();
private:
    std::string Label;
    TPortoValue Value;
};

class TColumn {
public:
    TColumn(std::string title, TPortoValue var, bool left_aligned = false);
    int PrintTitle(int x, int y, TConsoleScreen &screen);
    int Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, bool selected);
    void ClearCache();
    void Update(TPortoContainer* tree, int maxlevel);
    void Process();
    TPortoValue& At(TPortoContainer &row);
    void Highlight(bool enable);
    int GetWidth();
    void SetWidth(int width);
private:
    std::string Title;
    TPortoValue RootValue;

    int Width;
    std::unordered_map<std::string, TPortoValue> Cache;
    bool Selected = false;
    bool LeftAligned = false;
};

class TPortoTop {
public:
    TPortoTop(Porto::Connection *api, const std::vector<std::string> &args);
    void Update();
    void Process();
    void Sort();
    void Print(TConsoleScreen &screen);

    bool AddColumn(std::string desc);

    int RecreateColumns();

    void ChangeSelection(int x, int y, TConsoleScreen &screen);
    void Expand();

    int StartStop();
    int PauseResume();
    int Kill(int signal);
    int Destroy();
    void LessPortoctl(std::string container, std::string cmd);
    int RunCmdInContainer(TConsoleScreen &screen, std::string cmd);
    std::string SelectedContainer;

    int Delay = 3000;
    bool Paused = false;

private:
    void AddCommon(int row, const std::string &title, const std::string &var,
                   TPortoContainer &container, int flags, double multiplier = 1.0);
    void AddColumn(const TColumn &c);
    void PrintTitle(int y, TConsoleScreen &screen);
    int PrintCommon(TConsoleScreen &screen);

    Porto::Connection *Api;
    TPortoValueCache Cache;
    std::vector<std::string> Config;
    TPortoContainer RootContainer;
    std::vector<std::vector<TCommonValue>> Common;
    std::unique_ptr<TPortoContainer> ContainerTree;
    std::vector<TColumn> Columns;

    int SelectedRow = 0;
    int SelectedColumn = 0;
    int FirstX = 0;
    int FirstRow = 0;
    int MaxRows = 0;
    int DisplayRows = 0;
    int MaxLevel = 100;
};

