#include <unordered_map>

#include "portotop.hpp"

static double ParseNumber(const std::string &str) {
    try {
        return stod(str);
    } catch (...) {
        return NAN;
    }
}

static double ParseValue(const std::string &value, bool map) {
    if (!map)
        return ParseNumber(value);

    double ret = 0;
    unsigned long start_v = 0;
    for (unsigned long off = 0; off < value.length(); off++) {
        if (value[off] == ':')
            start_v = off + 2; // key: value
        else if (value[off] == ';')
            ret += ParseNumber(value.substr(start_v, off - start_v));
    }
    return ret + ParseNumber(value.substr(start_v));
};

static std::string PrintNumber(double number, int base,
                               bool seconds, bool percents,
                               double multiplier) {
    char buf[20];

    number /= multiplier;

    if (percents) {
        snprintf(buf, sizeof(buf), "%.1lf%%", 100 * number);
    } else if (seconds) {
        double seconds = number;
        double minutes = std::floor(seconds / 60);
        seconds -= minutes * 60;

        snprintf(buf, sizeof(buf), "%4.lf:%2.2lf", minutes, seconds);
    } else {
        char s = 0;
        if (number > base * base * base) {
            number /= base * base * base;
            s = 'G';
        } else if (number > base * base) {
            number /= base * base;
            s = 'M';
        } else if (number > base) {
            number /= base;
            s = 'k';
        }

        snprintf(buf, sizeof(buf), "%.1lf%c", number, s);
    }

    return std::string(buf);
};

static double DfDt(double curr, double prev, unsigned long gone_ms) {
    return 1000.0d * (curr - prev) / gone_ms;
};

static double PartOf(double value, double total) {
    return value / total;
}
////////////////////////////////////////////////////////////////////////////////

int TConsoleScreen::Width() {
    return getmaxx(Wnd);
}
int TConsoleScreen::Height() {
    return getmaxy(Wnd);
}

TConsoleScreen::TConsoleScreen() {
    Wnd = initscr();
    clear();
    cbreak();
    noecho();
    intrflush(stdscr, true);
    keypad(stdscr, true);
    SetTimeout(3000);
    curs_set(0);
}
TConsoleScreen::~TConsoleScreen() {
    endwin();
}
void TConsoleScreen::SetTimeout(int ms) {
    timeout(ms);
}
template<class T>
void TConsoleScreen::PrintAt(T arg, int x, int y, int width, bool leftaligned, int attr) {
    PrintAt(std::to_string(arg), x, y, width, leftaligned, attr);
}
void TConsoleScreen::PrintAt(std::string str0, int x0, int y0, int w0, bool leftaligned,
             int attr) {
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
void TConsoleScreen::Refresh() {
    refresh();
}
void TConsoleScreen::Clear() {
    clear();
}
int TConsoleScreen::Getch() {
    return wgetch(Wnd);
}
void TConsoleScreen::Save() {
    def_prog_mode();
    endwin();
}
void TConsoleScreen::Restore() {
    tcsetpgrp(1, getpgrp());
    reset_prog_mode();
    refresh();
}
int TConsoleScreen::Dialog(std::string text, const std::vector<std::string> &buttons) {
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
void TConsoleScreen::ErrorDialog(TPortoAPI &api) {
    std::string message;
    int error;

    api.GetLastError(error, message);

    if (error)
        Dialog(message, {"Ok"});
    else
        Dialog("Unknown error occured (probably, simple you aren't root)", {"Ok"});
}
void TConsoleScreen::ErrorDialog(std::string message, int error) {
    if (error != -1)
        Dialog("Done", {"Ok"});
    else
        Dialog(strerror(errno), {"Ok"});
}
void TConsoleScreen::InfoDialog(std::vector<std::string> lines) {
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
void TConsoleScreen::HelpDialog() {
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

void TPortoValueCache::Register(const std::string &container,
                                const std::string &variable) {
    if (Containers.find(container) == Containers.end())
        Containers[container] = 1;
    else
        Containers[container]++;
    if (Variables.find(variable) == Variables.end())
        Variables[variable] = 1;
    else
        Variables[variable]++;
}
void TPortoValueCache::Unregister(const std::string &container,
                                  const std::string &variable) {
    auto c = Containers.find(container);
    if (c != Containers.end()) {
        if (c->second == 1)
            Containers.erase(c);
        else
            c->second--;
    }
    auto v = Variables.find(variable);
    if (v != Variables.end()) {
        if (v->second == 1)
            Variables.erase(v);
        else
            v->second--;
    }
}

std::string TPortoValueCache::GetValue(const std::string &container,
                                       const std::string &variable,
                                       bool prev) {
    return Cache[CacheSelector ^ prev][container][variable].Value;
}

int TPortoValueCache::Update(TPortoAPI &api) {
    std::vector<std::string> _containers;
    for (auto &iter : Containers)
        _containers.push_back(iter.first);

    std::vector<std::string> _variables;
    for (auto &iter : Variables)
        _variables.push_back(iter.first);

    CacheSelector = !CacheSelector;
    Cache[CacheSelector].clear();
    int ret = api.Get(_containers, _variables, Cache[CacheSelector]);

    return ret;
}

TPortoValue::TPortoValue() : Cache(nullptr), Container(nullptr), Flags(ValueFlags::Raw) {
}

TPortoValue::TPortoValue(const TPortoValue &src) :
    Cache(src.Cache), Container(src.Container), Variable(src.Variable), Flags(src.Flags),
    Multiplier(src.Multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::TPortoValue(const TPortoValue &src, TPortoContainer *container) :
    Cache(src.Cache), Container(container), Variable(src.Variable), Flags(src.Flags),
    Multiplier(src.Multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::TPortoValue(TPortoValueCache &cache, TPortoContainer *container,
                             const std::string &variable, int flags, double multiplier) :
    Cache(&cache), Container(container), Variable(variable), Flags(flags),
    Multiplier(multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::~TPortoValue() {
    if (Cache && Container)
        Cache->Unregister(Container->GetName(), Variable);
}

void TPortoValue::Process(unsigned long gone) {
    if (Flags == ValueFlags::Container) {
        std::string name = Container->GetName();
        int level = Container->GetLevel();
        if (level > 0) {
            name = (Container->HasChildren() ? "+ " : "- ") +
                name.substr(1 + name.rfind('/'));
            AsString = std::string(2 * (level - 1), ' ') + name;
        }
        return;
    }

    AsString = Cache->GetValue(Container->GetName(), Variable, false);
    if (Flags == ValueFlags::Raw || AsString.length() == 0)
        return;

    AsNumber = ParseValue(AsString, Flags & ValueFlags::Map);

    if (Flags & ValueFlags::DfDt) {
        std::string old = Cache->GetValue(Container->GetName(), Variable, true);
        if (old.length() == 0)
            old = AsString;
        AsNumber = DfDt(AsNumber, ParseValue(old, Flags & ValueFlags::Map), gone);
    }

    if (Flags & ValueFlags::PartOfRoot) {
        std::string root_raw = Cache->GetValue("/", Variable, false);
        double root_number;

        root_number = ParseValue(root_raw, Flags & ValueFlags::Map);

        if (Flags & ValueFlags::DfDt) {
            std::string old = Cache->GetValue("/", Variable, true);
            if (old.length() == 0)
                old = root_raw;
            root_number = DfDt(root_number, ParseValue(old, Flags & ValueFlags::Map), gone);
        }

        AsNumber = PartOf(AsNumber, root_number);
    }

    int base = (Flags & ValueFlags::Bytes) ? 1024 : 1000;
    bool seconds = Flags & ValueFlags::Seconds;
    bool percents = Flags & ValueFlags::Percents;
    double multiplier = (Flags & ValueFlags::Multiplier) ? Multiplier : 1;
    AsString = PrintNumber(AsNumber, base, seconds, percents, multiplier);
}
std::string TPortoValue::GetValue() const {
    return AsString;
}
int TPortoValue::GetLength() const {
    return AsString.length();
}
bool TPortoValue::operator< (const TPortoValue &v) {
    if (Flags == ValueFlags::Raw)
        return AsString < v.AsString;
    else if (Flags == ValueFlags::Container)
        return Container->GetName() < v.Container->GetName();
    else
        return AsNumber > v.AsNumber;
}

TCommonValue::TCommonValue(const std::string &label, const TPortoValue &val) :
    Label(label), Value(val) {
}
std::string TCommonValue::GetLabel() {
    return Label;
}
TPortoValue& TCommonValue::GetValue() {
    return Value;
}

TPortoContainer::TPortoContainer(std::string container) : Container(container) {
    if (Container == "/")
        Level = 0;
    else
        Level = 1 + std::count(container.begin(), container.end(), '/');
}
TPortoContainer* TPortoContainer::GetParent(int level) {
    if (Parent) {
        if (Parent->GetLevel() == level)
            return Parent;
        else
            return Parent->GetParent(level);
    } else
        return nullptr;
}

TPortoContainer::~TPortoContainer() {
    for (auto &c : Children)
        delete c;
}

TPortoContainer* TPortoContainer::ContainerTree(TPortoAPI &api) {
    std::vector<std::string> containers;
    int ret = api.List(containers);
    if (ret)
        return nullptr;

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
std::string TPortoContainer::GetName() {
    return Container;
}
int TPortoContainer::GetLevel() {
    return Level;
}
void TPortoContainer::ForEachChild(std::function<void (TPortoContainer&)> fn, int maxlevel,
                               bool check_root) {
    if ((Level || check_root) && Level <= maxlevel)
        fn(*this);
    if (Level < maxlevel)
        for (auto &c : Children)
            c->ForEachChild(fn, maxlevel);
}
int TPortoContainer::GetMaxLevel() {
    int level = Level;
    for (auto &c : Children)
        if (c->GetMaxLevel() > level)
            level = c->GetMaxLevel();
    return level;
}
int TPortoContainer::ChildrenCount(int max_level) {
    int count = Level > 0 ? 1 : 0;
    if (Level < max_level)
        for (auto &c : Children)
            count += c->ChildrenCount(max_level);
    return count;
}
std::string TPortoContainer::ContainerAt(int n, int max_level) {
    TPortoContainer *ret = this;
    int i = 0;
    ForEachChild([&] (TPortoContainer &row) {
            if (i++ == n)
                ret = &row;
        }, max_level);
    return ret->GetName();
}
bool TPortoContainer::HasChildren() {
    return Children.size() > 0;
}
TPortoContainer* TPortoContainer::GetRoot() const {
    return Root;
}

TColumn::TColumn(std::string title, TPortoValue var, bool left_aligned) :
    Title(title), RootValue(var), LeftAligned(left_aligned) {
    Width = title.length();
}
int TColumn::PrintTitle(int x, int y, TConsoleScreen &screen) {
    screen.PrintAt(Title, x, y, Width, LeftAligned,
                   A_BOLD | (Selected ? A_UNDERLINE : 0));
    return Width;
}
int TColumn::Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, bool selected) {
    std::string p = At(row).GetValue();
    screen.PrintAt(p, x, y, Width, LeftAligned, selected ? A_REVERSE : 0);
    return Width;
}
void TColumn::Update(TPortoAPI &api, TPortoContainer* tree, int maxlevel) {
    tree->ForEachChild([&] (TPortoContainer &row) {
            TPortoValue val(RootValue, &row);
            Cache.insert(std::make_pair(row.GetName(), val));
        }, maxlevel, true);
}
TPortoValue& TColumn::At(TPortoContainer &row) {
    return Cache[row.GetName()];
}
void TColumn::Highlight(bool enable) {
    Selected = enable;
}
void TColumn::Process(unsigned long gone) {
    for (auto &iter : Cache) {
        iter.second.Process(gone);

        int w = iter.second.GetLength();
        if (w > Width)
            Width = w;
    }
}
int TColumn::GetWidth() {
    return Width;
}
void TColumn::SetWidth(int width) {
    Width = width;
}
void TColumn::ClearCache() {
    Cache.clear();
}

void TPortoContainer::SortTree(TColumn &column) {
    Children.sort([&] (TPortoContainer *row1, TPortoContainer *row2) {
            return column.At(*row1) < column.At(*row2);
        });
    for (auto &c : Children)
        c->SortTree(column);
}

void TPortoTop::PrintTitle(int y, TConsoleScreen &screen) {
    int x = FirstX;
    for (auto &c : Columns)
        x += 1 + c.PrintTitle(x, y, screen);
}
int TPortoTop::PrintCommon(TConsoleScreen &screen) {
    int x = 0;
    int y = 0;
    for (auto &line : Common) {
        for (auto &item : line) {
            std::string p = item.GetLabel();
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = item.GetValue().GetValue();
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;
        }
        y++;
        x = 0;
    }
    return y;
}

void TPortoTop::Print(TConsoleScreen &screen) {
    screen.Clear();

    int at_row = 1 + PrintCommon(screen);

    if (ContainerTree) {
        MaxRows = ContainerTree->ChildrenCount(MaxLevel);
        DisplayRows = std::min(screen.Height() - at_row, MaxRows);
        ChangeSelection(0, 0, screen);

        PrintTitle(at_row - 1, screen);
        int y = 0;
        ContainerTree->ForEachChild([&] (TPortoContainer &row) {
                if (y >= FirstRow && y < MaxRows) {
                    bool selected = y == FirstRow + SelectedRow;
                    int x = FirstX;
                    for (auto &c : Columns)
                        x += 1 + c.Print(row, x, at_row + y - FirstRow, screen, selected);
                }
                y++;
            }, MaxLevel);
        screen.Refresh();
    }
}
void TPortoTop::AddColumn(const TColumn &c) {
    Columns.push_back(c);
}
bool TPortoTop::AddColumn(std::string desc) {
    int flags = ValueFlags::Raw;
    size_t off = 0;
    std::string data;

    off = desc.find(':');
    std::string title = desc.substr(0, off);
    desc = desc.substr(off + 2);

    if (desc.length() > 4 && desc[0] == 'S' && desc[1] == '(') {
        off = desc.find(')');
        data = desc.substr(2, off == std::string::npos ?
                           std::string::npos : off - 2);
        flags |= ValueFlags::Map;
    } else {
        off = desc.find('\'');
        if (off == std::string::npos)
            off = desc.find(' ');
        if (off == std::string::npos)
            off = desc.find('%');

        data = desc.substr(0, off);
    }

    double multiplier = 1;

    if (off != std::string::npos) {
        for (; off < desc.length(); off++) {
            switch (desc[off]) {
            case 'b':
            case 'B':
                flags |= ValueFlags::Bytes;
                break;
            case 's':
            case 'S':
                flags |= ValueFlags::Seconds;
                break;
            case '\'':
                flags |= ValueFlags::DfDt;
                break;
            case '%':
                flags |= ValueFlags::PartOfRoot;
                flags |= ValueFlags::Percents;
                break;
            case ' ':
                break;
            default:
                try {
                    size_t tmp;
                    multiplier = stod(desc.substr(off), &tmp);
                    off += tmp - 1;
                    flags |= ValueFlags::Multiplier;
                } catch (...) {
                }
                break;
            }
        }
    }

    TPortoValue v(Cache, &RootContainer, data, flags, multiplier);
    Columns.push_back(TColumn(title, v));
    return true;
}
int TPortoTop::Update(TConsoleScreen &screen) {
    struct timespec Now = {0};
    clock_gettime(CLOCK_MONOTONIC, &Now);
    unsigned long gone = 1000 * (Now.tv_sec - LastUpdate.tv_sec) +
        (Now.tv_nsec - LastUpdate.tv_nsec) / 1000000;
    LastUpdate = Now;

    for (auto &column : Columns)
        column.ClearCache();

    ContainerTree.reset(TPortoContainer::ContainerTree(*Api));
    if (ContainerTree) {
        MaxMaxLevel = ContainerTree->GetMaxLevel();

        for (auto &column : Columns)
            column.Update(*Api, ContainerTree.get(), MaxLevel);

        int ret = Cache.Update(*Api);
        if (ret)
            return ret;

        int width = 0;
        for (auto &column : Columns) {
            column.Process(gone);
            width += column.GetWidth();
        }

        for (auto &line : Common)
            for (auto &item : line)
                item.GetValue().Process(gone);

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

        ContainerTree->SortTree(Columns[SelectedColumn]);
    }

    return 0;
}
void TPortoTop::ChangeSelection(int x, int y, TConsoleScreen &screen) {
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
void TPortoTop::Expand() {
    if (++MaxLevel > MaxMaxLevel)
        MaxLevel = 1;
}
int TPortoTop::StartStop() {
    std::string state;
    int ret = Api->GetData(SelectedContainer(), "state", state);
    if (ret)
        return ret;
    if (state == "running" || state == "dead" || state == "meta")
        return Api->Stop(SelectedContainer());
    else
        return Api->Start(SelectedContainer());
}
int TPortoTop::PauseResume() {
    std::string state;
    int ret = Api->GetData(SelectedContainer(), "state", state);
    if (ret)
        return ret;
    if (state == "paused")
        return Api->Resume(SelectedContainer());
    else
        return Api->Pause(SelectedContainer());
}
int TPortoTop::Kill(int signal) {
    return Api->Kill(SelectedContainer(), signal);
}
int TPortoTop::Destroy() {
    return Api->Destroy(SelectedContainer());
}
void TPortoTop::LessPortoctl(std::string container, std::string cmd) {
    std::string s(program_invocation_name);
    s += " get " + container + " " + cmd + " | less";
    int status = system(s.c_str());
    (void)status;
}

int TPortoTop::RunCmdInContainer(TConsoleScreen &screen, std::string cmd) {
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
std::string TPortoTop::SelectedContainer() {
    return ContainerTree->ContainerAt(FirstRow + SelectedRow, MaxLevel);
}
void TPortoTop::AddCommon(int row, const std::string &title, const std::string &var,
                          TPortoContainer &container, int flags) {
    Common.resize(row + 1);
    TPortoValue v(Cache, &container, var, flags);
    Common[row].push_back(TCommonValue(title, v));
}
TPortoTop::TPortoTop(TPortoAPI *api, std::string config) : Api(api),
                                                           RootContainer("/"),
                                                           DotContainer("."),
                                                           PortoContainer("/porto") {
    if (config.size() == 0)
        ConfigFile = std::string(getenv("HOME")) + "/.portotop";
    else
        ConfigFile = config;

    AddCommon(0, "Containers running: ", "porto_stat[running]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "total: ", "porto_stat[created]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Porto errors: ", "porto_stat[errors]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "warnings: ", "porto_stat[warnings]", RootContainer, ValueFlags::Raw);

    AddCommon(1, "Memory: ", "memory_usage", PortoContainer, ValueFlags::Bytes);
    AddCommon(1, "/ ", "memory_usage", RootContainer, ValueFlags::Bytes);
    AddCommon(1, ": ", "memory_usage", PortoContainer, ValueFlags::Bytes | ValueFlags::PartOfRoot |
        ValueFlags::Percents);

    AddCommon(1, "CPU: ", "cpu_usage", PortoContainer, ValueFlags::DfDt | ValueFlags::PartOfRoot |
        ValueFlags::Percents);

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
    RecreateColumns();
}
int TPortoTop::RecreateColumns() {
    Columns.clear();
    AddColumn(TColumn("container", TPortoValue(Cache, ContainerTree.get(), "",
                                               ValueFlags::Container), true));

    for (auto &c : Config)
        AddColumn(c);

    return 0;
}
int TPortoTop::SaveConfig() {
    std::ofstream out(ConfigFile);
    if (out.is_open()) {
        for (auto &s : Config)
            out << s << std::endl;
        return 0;
    } else
        return -1;
}
int TPortoTop::LoadConfig() {
    int ret = 0;
    Config.clear();
    std::ifstream in(ConfigFile);

    if (in.is_open()) {
        for (std::string line; getline(in, line);)
            if (line.size()) {
                Config.push_back(line);
                ret++;
            }

        RecreateColumns();

        return ret;
    }

    return -1;
}

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
                if (top.StartStop())
                    screen.ErrorDialog(*api);
            break;
        case 'p':
        case 'P':
            if (screen.Dialog("Pause/resume container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.PauseResume())
                    screen.ErrorDialog(*api);
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
                if (top.Kill(signal))
                    screen.ErrorDialog(*api);
            break;
        }
        case 'd':
        case 'D':
            if (screen.Dialog("Destroy container " + top.SelectedContainer(),
                              {"No", "Yes"}) == 1)
                if (top.Destroy())
                    screen.ErrorDialog(*api);
            break;
        case '\n':
            top.RunCmdInContainer(screen, "top");
            break;
        case 'b':
        case 'B':
            top.RunCmdInContainer(screen, "bash");
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
