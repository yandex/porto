#pragma once

#include <string>
#include <vector>
#include <map>
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
#include <menu.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
}

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
    void InfoDialog(std::vector<std::string> lines);
    void HelpDialog();
    void ColumnsMenu(std::vector<TColumn> &columns);
private:
    WINDOW *Wnd;
};

namespace PortoTreeTags {
    static const uint64_t None = 0x0;
    static const uint64_t Self = 0x1;
}

class TPortoContainer : public std::enable_shared_from_this<TPortoContainer> {
public:
    TPortoContainer(std::string container);
    static std::shared_ptr<TPortoContainer> ContainerTree(Porto::Connection &api);
    std::string GetName();
    int GetLevel();
    void ForEach(std::function<void (std::shared_ptr<TPortoContainer> &)> fn,
                 int maxlevel);
    void SortTree(TColumn &column);
    int GetMaxLevel();
    int ChildrenCount();
    std::string ContainerAt(int n, int max_level);
    uint64_t Tag = PortoTreeTags::None;
    std::list<std::shared_ptr<TPortoContainer>> Children;
private:
    std::shared_ptr<TPortoContainer> GetParent(int level);
    std::weak_ptr<TPortoContainer> Root;
    std::weak_ptr<TPortoContainer> Parent;
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
}

class TPortoValue {
public:
    TPortoValue();
    TPortoValue(const TPortoValue &src);
    TPortoValue(const TPortoValue &src, std::shared_ptr<TPortoContainer> &container);
    TPortoValue(std::shared_ptr<TPortoValueCache> &cache,
                std::shared_ptr<TPortoContainer> &container,
                const std::string &variable, int flags, double multiplier = 1);
    ~TPortoValue();
    void Process();
    std::string GetValue() const;
    int GetLength() const;
    bool operator< (const TPortoValue &v);
private:
    std::shared_ptr<TPortoValueCache> Cache;
    std::shared_ptr<TPortoContainer> Container;
    std::string Variable; // property or data

    int Flags;

    std::string AsString;
    double AsNumber = 0.0;
    double Multiplier = 0.0;
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
private:
    TPortoValue RootValue;

    int Width;
    std::unordered_map<std::string, TPortoValue> Cache;
    bool Selected = false;
    bool LeftAligned;

public:
    TColumn(std::string title, std::string desc,
            TPortoValue var, bool left_aligned, bool hidden);

    bool Hidden;
    std::string Title;
    std::string Description;

    int PrintTitle(int x, int y, TConsoleScreen &screen);
    int Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, int attr);
    void ClearCache();
    void Update(std::shared_ptr<TPortoContainer> &tree, int maxlevel);
    void Process();
    TPortoValue& At(TPortoContainer &row);
    void Highlight(bool enable);
    int GetWidth();
    void SetWidth(int width);
};

class TPortoTop {
public:
    TPortoTop(Porto::Connection *api, const std::vector<std::string> &args);
    void Update();
    void Process();
    void Sort();
    void Print(TConsoleScreen &screen);

    bool AddColumn(std::string title, std::string signal, std::string desc,
                   bool hidden = false);
    void MarkRow();
    void HideRows();

    void ChangeSelection(int x, int y, TConsoleScreen &screen);
    void ChangeView(int x, int y);
    void Expand();

    int StartStop();
    int PauseResume();
    int Kill(int signal);
    int Destroy();
    void LessPortoctl(std::string container, std::string cmd);
    int RunCmdInContainer(TConsoleScreen &screen, std::string cmd);
    std::string SelectedContainer;

    int Delay = 3000;
    int FirstDelay = 300;
    bool Paused = false;
    std::vector<TColumn> Columns;
    int SelectedColumn = 0;

private:
    void AddCommon(int row, const std::string &title, const std::string &var,
                   std::shared_ptr<TPortoContainer> &container,
                   int flags, double multiplier = 1.0);
    void AddColumn(const TColumn &c);
    void PrintTitle(int y, TConsoleScreen &screen);
    int PrintCommon(TConsoleScreen &screen);

    Porto::Connection *Api;
    std::shared_ptr<TPortoValueCache> Cache;
    std::shared_ptr<TPortoContainer> RootContainer;
    std::vector<std::vector<TCommonValue>> Common;
    std::shared_ptr<TPortoContainer> ContainerTree;

    int SelectedRow = 0;
    int FirstX = 0;
    int FirstRow = 0;
    int MaxRows = 0;
    int DisplayRows = 0;
    int MaxLevel = 100;

    int NextColor = 1;
    std::map<std::string, int> RowColor;
    bool FilterMode = false;
};

