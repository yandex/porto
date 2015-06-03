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
    void Clear();
    int Getch();
    void Save();
    void Restore();
    int Dialog(std::string text, const std::vector<std::string> &buttons);
    void ErrorDialog(TPortoAPI *api);
    void ErrorDialog(std::string message, int error);
    void InfoDialog(std::vector<std::string> lines);
    void HelpDialog();
private:
    WINDOW *Wnd;
};

typedef std::function<std::string(std::string, std::string, unsigned long)> TProcessor;
typedef std::function<std::string(std::string)> TPrinter;

typedef std::function<std::string(TPortoContainer&, TColumn&)> TColumnProcessor;
typedef std::function<std::string(TPortoContainer&, TColumn&, std::string)> TColumnPrinter;

class TPortoContainer {
public:
    ~TPortoContainer();
    static TPortoContainer* ContainerTree(std::vector<std::string> &containers);
    std::string GetContainer();
    int GetLevel();
    void for_each(std::function<void (TPortoContainer&)> fn, int maxlevel,
                  bool check_root = false);
    void Sort(TColumn &column);
    bool IsSelected();
    void Select(bool select);
    int GetMaxLevel();
    int RowCount(int max_level);
    std::string ContainerAt(int n, int max_level);
    bool HasChildren();
    TPortoContainer* GetRoot() const;
private:
    TPortoContainer(std::string container);
    TPortoContainer* GetParent(int level);
    TPortoContainer* Root = nullptr;
    TPortoContainer* Parent = nullptr;
    std::list<TPortoContainer*> Children;
    std::string Container;
    int Level = 0;
    bool Selected = false;
};

class TPortoValueCache {
public:
    void Register(TPortoValue &value);
    void Unregister(TPortoValue &value);
    int Update(TPortoAPI *Api);
private:
    std::unordered_set<TPortoValue*> Items;
    bool CacheSelector = false;
    std::map<std::string, std::map<std::string, TPortoGetResponse>> Cache[2];
    struct timespec Now = {0};
    struct timespec LastUpdate = {0};
    unsigned long GoneMs;
};

typedef std::function<std::string(std::string, std::string, unsigned long)> TProcessor;
typedef std::function<std::string(std::string)> TPrinter;

class TPortoValue {
public:
    TPortoValue();
    TPortoValue(const TPortoValue &src, const std::string &container);
    TPortoValue(const TPortoValue &src);
    TPortoValue(TPortoValueCache &cache, const std::string &variable,
                const std::string &container, TProcessor processor = nullptr,
                TPrinter printer = nullptr);
    TPortoValue(const std::string &value);
    ~TPortoValue();
    const std::string GetContainer() const;
    const std::string GetVariable() const;
    void ProcessValue(std::string curr, std::string old, unsigned long gone);
    std::string GetSortValue() const;
    std::string GetPrintValue() const;
    int GetLength() const;
private:
    TPortoValueCache *Cache;
    std::string Variable; // property or data
    std::string Container;

    TProcessor Processor = nullptr;
    TPrinter Printer = nullptr;

    std::string Saved;
    std::string Printed;
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
    TColumn(std::string title, TPortoValue var, TColumnProcessor processor = nullptr,
            TColumnPrinter printer = nullptr, bool left_aligned = false);
    int PrintTitle(int x, int y, TConsoleScreen &screen);
    int Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen);
    void Update(TPortoAPI *api, TPortoContainer* tree, int maxlevel);
    std::string At(TPortoContainer &row, bool print = false, bool cached = false);
    void Highlight(bool enable);
    void UpdateWidth();
    int GetWidth();
    void SetWidth(int width);
private:
    TPortoValue Title;
    TPortoValue RootValue;

    int Width;
    std::unordered_map<std::string, TPortoValue> Cache;
    bool Selected = false;
    bool LeftAligned = false;

    TColumnProcessor Processor;
    TColumnPrinter Printer;
};

class TPortoTop {
public:
    TPortoTop(TPortoAPI *api, std::string config);
    void Print(TConsoleScreen &screen);
    void AddColumn(const TColumn &c);
    bool AddColumn(std::string desc);
    int Update(TConsoleScreen &screen);
    void ChangeSelection(int x, int y, TConsoleScreen &screen);
    void Expand();
    int StartStop(TPortoAPI *api);
    int PauseResume(TPortoAPI *api);
    int Kill(TPortoAPI *api, int signal);
    int Destroy(TPortoAPI *api);
    void LessPortoctl(std::string container, std::string cmd);
    int RunCmdInContainer(TPortoAPI *api, TConsoleScreen &screen, std::string cmd);
    std::string SelectedContainer();
    int UpdateColumns();
    int SaveConfig();
    int LoadConfig();
private:
    void PrintTitle(int y, TConsoleScreen &screen);
    int PrintCommon(TConsoleScreen &screen);

    TPortoAPI *Api = nullptr;
    TPortoValueCache Cache;
    std::string ConfigFile;
    std::vector<std::string> Config;
    std::vector<TColumn> Columns;
    TPortoContainer* ContainerTree = nullptr;
    int SelectedRow = 0;
    int SelectedColumn = 0;
    int FirstX = 0;
    int FirstRow = 0;
    int MaxRows = 0;
    int DisplayRows = 0;
    int MaxLevel = 1;
    int MaxMaxLevel = 1;
    std::vector<std::vector<TCommonValue> > Common;
};

