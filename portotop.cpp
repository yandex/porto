#include <string>
#include <vector>
#include <list>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <unordered_set>

#include "libporto.hpp"
#include "util/namespace.hpp"

extern "C" {
#include <ncurses.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
};

class TConsoleScreen {
private:
    WINDOW *Wnd;

public:
    int Width() {
        return getmaxx(Wnd);
    }
    int Height() {
        return getmaxy(Wnd);
    }

    TConsoleScreen() {
        Wnd = initscr();
        clear();
        cbreak();
        noecho();
        intrflush(stdscr, true);
        keypad(stdscr, true);
        SetTimeout(3000);
        curs_set(0);
    }
    ~TConsoleScreen() {
        endwin();
    }
    void SetTimeout(int ms) {
        timeout(ms);
    }
    template<class T>
    void PrintAt(T arg, int x, int y, int width, bool leftaligned = false, int attr = 0) {
        PrintAt(std::to_string(arg), x, y, width, leftaligned, attr);
    }
    void PrintAt(std::string str0, int x0, int y0, int w0, bool leftaligned = false,
                 int attr = 0) {
        if (x0 + w0 < 0 || x0 >= Width())
            return;

        int x = x0 < 0 ? 0 : x0;
        int w = w0 - (x - x0);
        if (x + w >= Width())
            w = Width() - x;

        std::string str;
        if ((int)str0.length() > x - x0)
            str = str0.substr(x - x0, w);
        else
            str = std::string(w, ' ');

        if (attr)
            attron(attr);
        mvprintw(y0, x, (leftaligned ? "%-*s" : "%*s"), w, str.c_str());
        if (attr)
            attroff(attr);
    }
    void Refresh() {
        refresh();
    }
    void Clear() {
        clear();
    }
    int Getch() {
        return wgetch(Wnd);
    }
    void Save() {
        def_prog_mode();
        endwin();
    }
    void Restore() {
        tcsetpgrp(1, getpgrp());
        reset_prog_mode();
        refresh();
    }
    int Dialog(std::string text, const std::vector<std::string> &buttons) {
        int selected = 0;

        int x0 = Width() / 2 - text.length() / 2;
        int y0 = Height() / 2 - 3;

        int w = 0;
        for (auto &b : buttons)
            w += b.length() + 1;
        int x00 = Width() / 2 - w / 2;

        while (true) {
            Clear();

            PrintAt(text, x0, y0, text.length(), false);

            int x = x00;
            int n = 0;
            for (auto &b : buttons) {
                PrintAt(b, x, y0 + 2, b.length(), false, selected == n ? A_REVERSE : 0);
                x += 1 + b.length();
                n++;
            }

            switch(Getch()) {
            case KEY_LEFT:
                if (--selected < 0)
                    selected = 0;
                break;
            case KEY_RIGHT:
                if ((unsigned long) ++selected > buttons.size() - 1)
                    selected = buttons.size() - 1;
                break;
            case '\n':
                return selected;
            }

            Refresh();
        }

        return -1;
    }
    void ErrorDialog(TPortoAPI *api) {
        std::string message;
        int error;

        api->GetLastError(error, message);

        if (error)
            Dialog(message, {"Ok"});
        else
            Dialog("Unknown error occured (probably, simple you aren't root)", {"Ok"});
    }
    void ErrorDialog(std::string message, int error) {
        if (error != -1)
            Dialog("Done", {"Ok"});
        else
            Dialog(strerror(errno), {"Ok"});
    }
    void InfoDialog(std::vector<std::string> lines) {
        unsigned int w = 0;
        for (auto &l : lines)
            if (l.length() > w)
                w = l.length();
        int x0 = Width() / 2 - w / 2;
        int y0 = Height() / 2 - lines.size() / 2;

        while (true) {
            Clear();

            int n = 0;
            for (auto &l : lines) {
                PrintAt(l, x0, y0 + n, l.length(), false);
                n++;
            }

            switch(Getch()) {
            case 0:
            case -1:
                break;
            default:
                return;
            }

            Refresh();
        }
    }
    void HelpDialog() {
        std::vector<std::string> help =
            {"horizontal arrows - change sorting/scroll",
             "vertical arrows / j,k - select container/scroll",
             "tab - expand subcontainers",
             "s - start/stop container",
             "p - pause/resume container",
             "K - kill container",
             "d - destroy container",
             "g - show container properties",
             "o - show container stdout",
             "e - show container stderr",
             "w - save portotop config",
             "l - load portotop config",
             "enter - run top in container",
             "b - run bash in container",
             "space - pause",
             "1,2,3,5,0 - set update time to 1s,2s,3s,5s and 10s",
             "q - quit",
             "h,? - help"};
        InfoDialog(help);
    }
};

class TPortoValue;

class TPortoValueCache {
public:
    void Register(TPortoValue &value) {
        Items.insert(&value);
    }
    void Unregister(TPortoValue &value) {
        Items.erase(&value);
    }
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
    TPortoValue() : Cache(nullptr), Saved() {
    }
    TPortoValue(const TPortoValue &src, const std::string &container) :
        Cache(src.Cache),
        Variable(src.Variable),
        Container(container),
        Processor(src.Processor),
        Printer(src.Printer),
        Saved(src.Saved),
        Printed(src.Printed) {
        if (Cache)
            Cache->Register(*this);
    }
    TPortoValue(const TPortoValue &src) : Cache(src.Cache),
                                          Variable(src.Variable),
                                          Container(src.Container),
                                          Processor(src.Processor),
                                          Printer(src.Printer),
                                          Saved(src.Saved),
                                          Printed(src.Printed) {
        if (Cache)
            Cache->Register(*this);
    }
    TPortoValue(TPortoValueCache &cache, const std::string &variable,
                const std::string &container, TProcessor processor = nullptr,
                TPrinter printer = nullptr) :
        Cache(&cache), Variable(variable), Container(container),
        Processor(processor), Printer(printer) {
        Cache->Register(*this);
    }
    // static fields like title
    TPortoValue(const std::string &value) : Cache(nullptr), Saved(value),
                                            Printed(value) {
    }
    ~TPortoValue() {
        if (Cache)
            Cache->Unregister(*this);
    }
    const std::string GetContainer() const {
        return Container;
    }
    const std::string GetVariable() const {
        return Variable;
    }
    void ProcessValue(std::string curr, std::string old, unsigned long gone) {
        if (Processor)
            Saved = Processor(curr, old, gone);
        else
            Saved = curr;
        if (Printer)
            Printed = Printer(Saved);
        else
            Printed = Saved;
    }
    std::string GetSortValue() const {
        return Saved;
    }
    std::string GetPrintValue() const {
        return Printed;
    }
    int GetLength() const {
        return Printed.length();
    }
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
    TCommonValue(const std::string &label, const TPortoValue &val) :
        Label(label), Value(val) {
    }
    std::string GetLabel() {
        return Label;
    }
    TPortoValue& GetValue() {
        return Value;
    }
private:
    std::string Label;
    TPortoValue Value;
};

int TPortoValueCache::Update(TPortoAPI *api) {
    LastUpdate = Now;
    clock_gettime(CLOCK_MONOTONIC, &Now);
    GoneMs = 1000 * (Now.tv_sec - LastUpdate.tv_sec) +
        (Now.tv_nsec - LastUpdate.tv_nsec) / 1000000;

    std::unordered_set<std::string> containers;
    std::unordered_set<std::string> variables;

    for (auto &iter : Items) {
        containers.insert(iter->GetContainer());
        variables.insert(iter->GetVariable());
    }

    std::vector<std::string> _containers;
    for (auto &iter : containers)
        _containers.push_back(iter);

    std::vector<std::string> _variables;
    for (auto &iter : variables)
        _variables.push_back(iter);

    CacheSelector = !CacheSelector;
    Cache[CacheSelector].clear();
    int ret = api->Get(_containers, _variables, Cache[CacheSelector]);

    for (auto &iter : Items) {
        std::string c = iter->GetContainer();
        std::string v = iter->GetVariable();
        iter->ProcessValue(Cache[CacheSelector][c][v].Value,
                           Cache[!CacheSelector][c][v].Value, GoneMs);
    }

    return ret;
}

class TColumn;
class TPortoContainer {
    TPortoContainer(std::string container) : Container(container) {
        if (Container == "/")
            Level = 0;
        else
            Level = 1 + std::count(container.begin(), container.end(), '/');
    }
    TPortoContainer* GetParent(int level) {
        if (Parent) {
            if (Parent->GetLevel() == level)
                return Parent;
            else
                return Parent->GetParent(level);
        } else
            return nullptr;
    }
public:
    ~TPortoContainer() {
        for (auto &c : Children)
            delete c;
    }
    static TPortoContainer* ContainerTree(std::vector<std::string> &containers) {
        TPortoContainer *root = nullptr;
        TPortoContainer *curr = nullptr;
        TPortoContainer *prev = nullptr;
        int level = 0;

        std::sort(containers.begin(), containers.end());

        for (auto &c : containers) {
            curr = new TPortoContainer(c);
            level = curr->GetLevel();
            if (!root) {
                /* assume that / container is first in the list */
                if (c == "/") {
                    root = curr;
                    prev = curr;
                    continue;
                } else
                    break;
            } else if (level > prev->GetLevel())
                curr->Parent = prev;
            else if (level == prev->GetLevel())
                curr->Parent = prev->Parent;
            else /* level < prev->GetLevel() */
                curr->Parent = prev->GetParent(level - 1);
            curr->Root = root;

            curr->Parent->Children.push_back(curr);
            prev = curr;
        }
        return root;
    }
    std::string GetContainer() {
        return Container;
    }
    int GetLevel() {
        return Level;
    }
    void for_each(std::function<void (TPortoContainer&)> fn, int maxlevel,
                  bool check_root = false) {
        if ((Level || check_root) && Level <= maxlevel)
            fn(*this);
        if (Level < maxlevel)
            for (auto &c : Children)
                c->for_each(fn, maxlevel);
    }
    void Sort(TColumn &column);
    bool IsSelected() {
        return Selected;
    }
    void Select(bool select) {
        Selected = select;
    }
    int GetMaxLevel() {
        int level = Level;
        for (auto &c : Children)
            if (c->GetMaxLevel() > level)
                level = c->GetMaxLevel();
        return level;
    }
    int RowCount(int max_level) {
        int count = Level > 0 ? 1 : 0;
        if (Level < max_level)
            for (auto &c : Children)
                count += c->RowCount(max_level);
        return count;
    }
    std::string ContainerAt(int n, int max_level) {
        TPortoContainer *ret = this;
        int i = 0;
        for_each([&] (TPortoContainer &row) {
                if (i++ == n)
                    ret = &row;
            }, max_level);
        return ret->GetContainer();
    }
    bool HasChildren() {
        return Children.size() > 0;
    }
    TPortoContainer* GetRoot() const {
        return Root;
    }
private:
    TPortoContainer* Root = nullptr;
    TPortoContainer* Parent = nullptr;
    std::list<TPortoContainer*> Children;
    std::string Container;
    int Level = 0;
    bool Selected = false;
};

static TPrinter nice_number(int base) {
    return [=] (std::string raw) {
        try {
            char buf[20];
            char s = 0;

            double v = stod(raw);
            if (v > base * base * base) {
                v /= base * base * base;
                s = 'G';
            } else if (v > base * base) {
                v /= base * base;
                s = 'M';
            } else if (v > base) {
                v /= base;
                s = 'k';
            }

            snprintf(buf, sizeof(buf), "%.1lf%c", v, s);
            return std::string(buf);
        } catch (...) {
            return raw;
        }
    };
}

static TPrinter nice_seconds(double multiplier = 1) {
    return [=] (std::string raw) {
        try {
            char buf[40];

            double seconds = stod(raw) / multiplier;
            double minutes = std::floor(seconds / 60);
            seconds -= minutes * 60;

            snprintf(buf, sizeof(buf), "%4.lf:%2.2lf", minutes, seconds);
            return std::string(buf);
        } catch (...) {
            return std::string();
        }
    };
}

static TProcessor map_summ(std::string data) {
    return [=] (std::string value, std::string unused, unsigned long unused2) {
        std::vector<std::string> values;
        unsigned long start_v = 0;
        for (unsigned long off = 0; off < value.length(); off++) {
            if (value[off] == ':') {
                start_v = off + 2; // key: value
            } else if (value[off] == ';') {
                values.push_back(value.substr(start_v, off - start_v));
            }
        }
        values.push_back(value.substr(start_v));

        unsigned long ret = 0;
        try {
            for (auto &s : values)
                ret += stoull(s);
        } catch (...) {
            ret = 0;
        }

        return std::to_string(ret);
    };
}

static TProcessor diff() {
    return [=] (std::string value, std::string prev_value, unsigned long gone_ms) {
        try {
            unsigned long c = stoull(value);
            unsigned long prev = stoull(prev_value);

            std::string str = std::to_string((1000.0d * (c - prev)) / gone_ms);

            return str;
        } catch (...) {
            return std::string();
        }
    };
}

typedef std::function<std::string(TPortoContainer&, TColumn&)> TColumnProcessor;
typedef std::function<std::string(TPortoContainer&, TColumn&, std::string)> TColumnPrinter;

class TColumn {
public:
    TColumn(std::string title, TPortoValue var, TColumnProcessor processor = nullptr,
            TColumnPrinter printer = nullptr, bool left_aligned = false) :
        Title(title), RootValue(var), LeftAligned(left_aligned),
        Processor(processor), Printer(printer) {
        Width = title.length();
    }
    int PrintTitle(int x, int y, TConsoleScreen &screen) {
        screen.PrintAt(Title.GetPrintValue(), x, y, Width, LeftAligned,
                      A_BOLD | (Selected ? A_UNDERLINE : 0));
        return Width;
    }
    int Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen) {
        std::string p;
        if (Printer)
            p = Printer(row, *this, At(row, true));
        else
            p = At(row, true);

        screen.PrintAt(p, x, y, Width, LeftAligned, row.IsSelected() ? A_REVERSE : 0);
        return Width;
    }
    void Update(TPortoAPI *api, TPortoContainer* tree, int maxlevel) {
        Cache.clear();
        tree->for_each([&] (TPortoContainer &row) {
                TPortoValue val(RootValue, row.GetContainer());
                Cache.insert(std::make_pair(row.GetContainer(), val));
            }, maxlevel, true);
    }
    std::string At(TPortoContainer &row, bool print = false, bool cached = false) {
        if (Processor && !cached)
            return Processor(row, *this);
        else
            return print ? Cache[row.GetContainer()].GetPrintValue() :
                Cache[row.GetContainer()].GetSortValue();
    }
    void Highlight(bool enable) {
        Selected = enable;
    }
    void UpdateWidth() {
        for (auto &iter : Cache) {
            int w = iter.second.GetLength();
            if (w > Width)
                Width = w;
        }
        if (Title.GetLength() > Width)
            Width = Title.GetLength();
    }
    int GetWidth() {
        return Width;
    }
    void SetWidth(int width) {
        Width = width;
    }
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

void TPortoContainer::Sort(TColumn &column) {
    Children.sort([&] (TPortoContainer *row1, TPortoContainer *row2) {
            std::string str1 = column.At(*row1);
            std::string str2 = column.At(*row2);

            bool numeric_1 = false, numeric_2 = false;
            double v1, v2;

            try {
                v1 = stod(str1);
                numeric_1 = true;
            } catch (...) {}
            try {
                v2 = stod(str2);
                numeric_2 = true;
            } catch (...) {}

            if (numeric_1 && numeric_2)
                return v1 > v2;
            if (!numeric_1 && !numeric_2)
                return str1 < str2;
            return numeric_1;
        });
    for (auto &c : Children)
        c->Sort(column);
}

static TColumnProcessor part_of_root() {
    return [] (TPortoContainer &row, TColumn &column) {
        try {
            std::string _curr = column.At(row, false, true);
            std::string _root = column.At(*row.GetRoot(), false, true);

            double curr = stod(_curr);
            double root = stod(_root);

            return std::to_string(curr / root);
        } catch (...) {
            return std::string("error");
        }
    };
}

static TColumnPrinter nice_percents() {
    return [] (TPortoContainer &row, TColumn &column, std::string raw) {
        try {
            char buf[20];
            snprintf(buf, sizeof(buf), "%.1lf%%", 100 * stod(raw));
            return std::string(buf);
        } catch (...) {
            return std::string();
        }
        return raw;
    };
}

class TPortoTop {
    void PrintTitle(int y, TConsoleScreen &screen) {
        int x = FirstX;
        for (auto &c : Columns)
            x += 1 + c.PrintTitle(x, y, screen);
    }
    int PrintCommon(TConsoleScreen &screen) {
        int x = 0;
        int y = 0;
        for (auto &line : Common) {
            for (auto &item : line) {
                std::string p = item.GetLabel();
                screen.PrintAt(p, x, y, p.length());
                x += p.length();
                p = item.GetValue().GetPrintValue();
                screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
                x += p.length() + 1;
            }
            y++;
            x = 0;
        }
        return y;
    }
public:
    void Print(TConsoleScreen &screen) {
        screen.Clear();

        int at_row = 1 + PrintCommon(screen);
        MaxRows = ContainerTree->RowCount(MaxLevel);
        DisplayRows = std::min(screen.Height() - at_row, MaxRows);
        ChangeSelection(0, 0, screen);

        PrintTitle(at_row - 1, screen);
        int y = 0;
        ContainerTree->for_each([&] (TPortoContainer &row) {
                if (y >= FirstRow && y < MaxRows) {
                    if (y == FirstRow + SelectedRow)
                        row.Select(true);
                    int x = FirstX;
                    for (auto &c : Columns)
                        x += 1 + c.Print(row, x, at_row + y - FirstRow, screen);
                    row.Select(false);
                }
                y++;
            }, MaxLevel);
        screen.Refresh();
    }
    void AddColumn(const TColumn &c) {
        Columns.push_back(c);
    }
    bool AddColumn(std::string desc) {
        TProcessor processor = nullptr;
        TPrinter printer = nullptr; // "", "b", "1E9s", "%"
        TColumnProcessor column_processor = nullptr;
        TColumnPrinter column_printer = nullptr;
        size_t off = 0;
        std::string data;

        off = desc.find(':');
        std::string title = desc.substr(0, off);
        desc = desc.substr(off + 2);

        if (desc.length() > 4 && desc[0] == 'S' && desc[1] == '(') {
            off = desc.find(')');
            data = desc.substr(2, off == std::string::npos ?
                               std::string::npos : off - 2);
            processor = map_summ(data);
        } else {
            off = desc.find('\'');
            if (off == std::string::npos)
                off = desc.find(' ');
            if (off == std::string::npos)
                off = desc.find('%');

            data = desc.substr(0, off);
        }

        double base = 1;
        printer = nice_number(1000);

        if (off != std::string::npos) {
            for (; off < desc.length(); off++) {
                switch (desc[off]) {
                case 'b':
                case 'B':
                    printer = nice_number(1024);
                    break;
                case 's':
                case 'S':
                    printer = nice_seconds(base);
                    break;
                case '\'':
                    processor = diff();
                    break;
                case '%':
                    column_processor = part_of_root();
                    column_printer = nice_percents();
                    break;
                case ' ':
                    break;
                default:
                    try {
                        size_t tmp;
                        base = stod(desc.substr(off), &tmp);
                        off += tmp - 1;
                    } catch (...) {
                    }
                    break;
                }
            }
        }

        TPortoValue v(Cache, data, "/", processor, printer);
        Columns.push_back(TColumn(title, v, column_processor, column_printer));
        return true;
    }
    int Update(TConsoleScreen &screen) {
        std::vector<std::string> containers;
        int ret = Api->List(containers);
        if (ret)
            return ret;

        if (ContainerTree)
            delete ContainerTree;

        ContainerTree = TPortoContainer::ContainerTree(containers);
        if (ContainerTree) {
            MaxMaxLevel = ContainerTree->GetMaxLevel();

            for (auto &column : Columns)
                column.Update(Api, ContainerTree, MaxLevel);

            int ret = Cache.Update(Api);
            if (ret)
                return ret;

            int width = 0;
            for (auto &column : Columns) {
                column.UpdateWidth();
                width += column.GetWidth();
            }

            if (width > screen.Width()) {
                int excess = width - screen.Width();
                int current = Columns[0].GetWidth();
                if (current > 30) {
                    current -= excess;
                    if (current < 30)
                        current = 30;
                }
                Columns[0].SetWidth(current);
            }

            ContainerTree->Sort(Columns[SelectedColumn]);
        }

        return 0;
    }
    void ChangeSelection(int x, int y, TConsoleScreen &screen) {
        SelectedRow += y;
        if (SelectedRow < 0) {
            SelectedRow = 0;
            FirstRow += y;
            if (FirstRow < 0)
                FirstRow = 0;
        }
        if (SelectedRow > DisplayRows - 1) {
            SelectedRow = DisplayRows - 1;
            FirstRow += y;
            if (FirstRow > MaxRows - DisplayRows)
                FirstRow = MaxRows - DisplayRows;
        }

        Columns[SelectedColumn].Highlight(false);
        SelectedColumn += x;
        if (SelectedColumn < 0) {
            SelectedColumn = 0;
        } else if (SelectedColumn > (int)Columns.size() - 1) {
            SelectedColumn = Columns.size() - 1;
        }
        Columns[SelectedColumn].Highlight(true);

        if (x == 0 && y == 0) {
            int i = 0;
            int x = 0;
            for (auto &c : Columns) {
                if (i == SelectedColumn && FirstX + x <= 0) {
                    FirstX = x;
                    break;
                }
                x += c.GetWidth() + 1;
                if (i == SelectedColumn && x > screen.Width()) {
                    FirstX = -x + screen.Width();
                    break;
                }
                i++;
            }
        }
    }
    void Expand() {
        if (++MaxLevel > MaxMaxLevel)
            MaxLevel = 1;
    }
    int StartStop(TPortoAPI *api) {
        std::string state;
        int ret = api->GetData(SelectedContainer(), "state", state);
        if (ret)
            return ret;
        if (state == "running" || state == "dead" || state == "meta")
            return api->Stop(SelectedContainer());
        else
            return api->Start(SelectedContainer());
    }
    int PauseResume(TPortoAPI *api) {
        std::string state;
        int ret = api->GetData(SelectedContainer(), "state", state);
        if (ret)
            return ret;
        if (state == "paused")
            return api->Resume(SelectedContainer());
        else
            return api->Pause(SelectedContainer());
    }
    int Kill(TPortoAPI *api, int signal) {
        return api->Kill(SelectedContainer(), signal);
    }
    int Destroy(TPortoAPI *api) {
        return api->Destroy(SelectedContainer());
    }
    void LessPortoctl(std::string container, std::string cmd) {
        std::string s(program_invocation_name);
        s += " get " + container + " " + cmd + " | less";
        (void)system(s.c_str());
    }
    int RunCmdInContainer(TPortoAPI *api, TConsoleScreen &screen, std::string cmd) {
        bool enter = (SelectedContainer() != "/");
        int ret = -1;

        if (enter && getuid()) {
            screen.Dialog("You have to be root to enter containers.", {"Ok"});
            return -1;
        }

        screen.Save();
        switch (fork()) {
        case -1:
            ret = errno;
            break;
        case 0:
        {
            if (enter)
                exit(execlp(program_invocation_name, program_invocation_name,
                           "enter", SelectedContainer().c_str(), cmd.c_str(), nullptr));
            else
                exit(execlp(cmd.c_str(), cmd.c_str(), nullptr));
            break;
        }
        default:
        {
            wait(&ret);
            break;
        }
        }
        screen.Restore();

        if (ret)
            screen.Dialog(strerror(ret), {"Ok"});

        return ret;
    }
    std::string SelectedContainer() {
        return ContainerTree->ContainerAt(FirstRow + SelectedRow, MaxLevel);
    }
    TPortoTop(TPortoAPI *api, std::string config) : Api(api) {
        if (config.size() == 0)
            ConfigFile = std::string(getenv("HOME")) + "/.portotop";
        else
            ConfigFile = config;

        Common = {{TCommonValue("Containers running: ",
                                TPortoValue(Cache, "porto_stat[running]", "/")),
                   TCommonValue("total: ",
                                TPortoValue(Cache, "porto_stat[created]", "/")),
                   TCommonValue("Porto errors: ",
                                TPortoValue(Cache, "porto_stat[errors]", "/")),
                   TCommonValue("warnings: ",
                                TPortoValue(Cache, "porto_stat[warnings]", "/"))}};

        if (LoadConfig() != -1)
            return;

        Config = {"state: state",
                  "time: time s",

                  /* CPU */
                  "policy: cpu_policy",
                  "cpu%: cpu_usage'%",
                  "cpu: cpu_usage 1e9s",

                  /* Memory */
                  "memory: memory_usage b",
                  "limit: memory_limit b",
                  "guarantee: memory_guarantee b",

                  /* I/O */
                  "maj/s: major_faults'",
                  "read b/s: S(io_read)' b",
                  "write b/s: S(io_write)' b",

                  /* Network */
                  "net b/s: S(net_bytes) 'b",
        };
        UpdateColumns();
    }
    int UpdateColumns() {
        Columns.clear();
        TColumnPrinter nice_container = [] (TPortoContainer &row, TColumn &column,
                                            std::string unused) {
            std::string name = row.GetContainer();
            int level = row.GetLevel();
            if (level > 0)
                name = (row.HasChildren() ? "+" : "-") +
                    name.substr(1 + name.rfind('/'));
            return std::string(level - 1, ' ') + name;

            return name;
        };
        AddColumn(TColumn("container", TPortoValue(Cache, "absolute_name", "/"),
                                                   nullptr, nice_container, true));

        for (auto &c : Config)
            AddColumn(c);

        return 0;
    }
    int SaveConfig() {
        std::ofstream out(ConfigFile);
        if (out.is_open()) {
            for (auto &s : Config)
                out << s << std::endl;
            return 0;
        } else
            return -1;
    }
    int LoadConfig() {
        int ret = 0;
        Config.clear();
        std::ifstream in(ConfigFile);

        if (in.is_open()) {
            for (std::string line; getline(in, line);)
                if (line.size()) {
                    Config.push_back(line);
                    ret++;
                }

            UpdateColumns();

            return ret;
        }

        return -1;
    }
private:
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

static bool exit_immediatly = false;
void exit_handler(int unused) {
    exit_immediatly = true;
}

int portotop(TPortoAPI *api, std::string config) {
    signal(SIGINT, exit_handler);
    signal(SIGTERM, exit_handler);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    TPortoTop top(api, config);

    /* Main loop */
    TConsoleScreen screen;
    bool paused = false;
    while (true) {
        if (exit_immediatly)
            break;

        if (!paused && top.Update(screen))
            break;

        top.Print(screen);

        switch (screen.Getch()) {
        case 'q':
        case 'Q':
            return EXIT_SUCCESS;
            break;
        case 'k':
        case KEY_UP:
            top.ChangeSelection(0, -1, screen);
            break;
        case KEY_PPAGE:
            top.ChangeSelection(0, -10, screen);
            break;
        case 'j':
        case KEY_DOWN:
            top.ChangeSelection(0, 1, screen);
            break;
        case KEY_NPAGE:
            top.ChangeSelection(0, 10, screen);
            break;
        case KEY_LEFT:
            top.ChangeSelection(-1, 0, screen);
            break;
        case KEY_RIGHT:
            top.ChangeSelection(1, 0, screen);
            break;
        case '\t':
            top.Expand();
            break;
        case ' ':
            paused = !paused;
            break;
        case 's':
        case 'S':
            if (screen.Dialog("Start/stop container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.StartStop(api))
                    screen.ErrorDialog(api);
            break;
        case 'p':
        case 'P':
            if (screen.Dialog("Pause/resume container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.PauseResume(api))
                    screen.ErrorDialog(api);
            break;
        case 'K':
        {
            int signal = -1;
            switch (screen.Dialog("Kill container " + top.SelectedContainer(),
                                  {"Cancel", "SIGTERM", "SIGINT", "SIGKILL", "SIGHUP"})) {
            case 1:
                signal = SIGTERM;
                break;
            case 2:
                signal = SIGINT;
                break;
            case 3:
                signal = SIGKILL;
                break;
            case 4:
                signal = SIGHUP;
                break;
            }
            if (signal > 0)
                if (top.Kill(api, signal))
                    screen.ErrorDialog(api);
            break;
        }
        case 'd':
        case 'D':
            if (screen.Dialog("Destroy container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.Destroy(api))
                    screen.ErrorDialog(api);
            break;
        case '\n':
            top.RunCmdInContainer(api, screen, "top");
            break;
        case 'b':
        case 'B':
            top.RunCmdInContainer(api, screen, "bash");
            break;
        case 'g':
        case 'G':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "");
            screen.Restore();
            break;
        case 'o':
        case 'O':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "stdout");
            screen.Restore();
            break;
        case 'e':
        case 'E':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer(), "stderr");
            screen.Restore();
            break;
        case 'l':
        case 'L':
            screen.ErrorDialog("Can't load config", top.LoadConfig());
            break;
        case 'w':
        case 'W':
            screen.ErrorDialog("Can't save config", top.SaveConfig());
            break;
        case '1':
            screen.SetTimeout(1000);
            break;
        case '2':
            screen.SetTimeout(2000);
            break;
        case '3':
            screen.SetTimeout(3000);
            break;
        case '5':
            screen.SetTimeout(5000);
            break;
        case '0':
            screen.SetTimeout(10000);
            break;
        case 0:
        case -1:
        case KEY_RESIZE:
        case KEY_MOUSE:
            break;
        case 'h':
        case '?':
        default:
            screen.HelpDialog();
            break;
        }
    }

    return EXIT_SUCCESS;
}
