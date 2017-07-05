#include <unordered_map>

#include "portotop.hpp"
#include "version.hpp"

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

static double DfDt(double curr, double prev, uint64_t dt) {
    if (dt)
        return 1000.0 * (curr - prev) / dt;
    return 0;
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
void TConsoleScreen::Erase() {
    erase();
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
    bool done = false;

    int x0 = Width() / 2 - text.length() / 2;
    int y0 = Height() / 2 - 3;

    int w = 0;
    for (auto &b : buttons)
        w += b.length() + 1;
    int x00 = Width() / 2 - w / 2;

    WINDOW *win = newwin(5, std::max((int)text.length(), w) + 4, y0 - 1, std::min(x0, x00) - 2);
    box(win, 0, 0);
    wrefresh(win);

    while (!done) {
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
            done = true;
            break;
        }

        Refresh();
    }

    delwin(win);

    return selected;
}
void TConsoleScreen::ErrorDialog(Porto::Connection &api) {
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
    unsigned int h = lines.size();
    for (auto &l : lines)
        if (l.length() > w)
            w = l.length();
    int x0 = Width() / 2 - w / 2;
    int y0 = Height() / 2 - h / 2;
    bool done = false;

    WINDOW *win = newwin(h + 2, w + 4, y0 - 1, x0 - 2);
    box(win, 0, 0);
    wrefresh(win);

    while (!done) {
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
            done = true;
            break;
        }

        Refresh();
    }

    delwin(win);
}

void TConsoleScreen::HelpDialog() {
    std::vector<std::string> help =
        {std::string("portoctl top ") + PORTO_VERSION + " " + PORTO_REVISION,
         "",
         "left, right, home, end - change sorting/scroll",
         "up, down, page up, page down - select container/scroll",
         "tab - expand conteainers tree: first, second, all",
         "",
         "1-9,0 - set update delay to 1s-9s and 10s",
         "space - pause/resume screen updates",
         "u - update screen",
         "",
         "g - get properties",
         "o - show stdout",
         "e - show stderr",
         "t - run top in container",
         "b - run bash in container",
         "",
         "S - start/stop container",
         "P - pause/resume container",
         "K - kill container",
         "D - destroy container",
         "",
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

uint64_t TPortoValueCache::GetDt() {
    return Time[CacheSelector] - Time[!CacheSelector];
}

int TPortoValueCache::Update(Porto::Connection &api) {
    std::vector<std::string> _containers;
    for (auto &iter : Containers)
        _containers.push_back(iter.first);

    std::vector<std::string> _variables;
    for (auto &iter : Variables)
        _variables.push_back(iter.first);

    CacheSelector = !CacheSelector;
    Cache[CacheSelector].clear();
    int ret = api.Get(_containers, _variables, Cache[CacheSelector],
                      Porto::GetFlags::Sync | Porto::GetFlags::Real);
    Time[CacheSelector] = GetCurrentTimeMs();

    api.GetVersion(Version, Revision);

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

void TPortoValue::Process() {
    if (Flags == ValueFlags::Container) {
        std::string name = Container->GetName();
        std::string tab = "", tag = "";

        int level = Container->GetLevel();

        if (name != "/")
            name = name.substr(1 + name.rfind('/'));

        tab = std::string(level, ' ');

        if (Container->Tag & PortoTreeTags::Self)
            tag = "@ ";

        else if (level)
            tag = Container->ChildrenCount() ? "+ " : "- ";

        AsString = tab + tag + name;
        return;
    }

    AsString = Cache->GetValue(Container->GetName(), Variable, false);

    if (Flags == ValueFlags::State) {
        AsNumber = 0;
        if (AsString == "running")
            AsNumber = 1000;
        else if (AsString == "meta")
            AsNumber = 500;
        else if (AsString == "starting")
            AsNumber = 300;
        else if (AsString == "paused")
            AsNumber = 200;
        else if (AsString == "dead")
            AsNumber = 100;
        if (Container)
            AsNumber += Container->ChildrenCount();
        return;
    }

    if (Flags == ValueFlags::Raw || AsString.length() == 0) {
        AsNumber = -1;
        return;
    }

    AsNumber = ParseValue(AsString, Flags & ValueFlags::Map);

    if (Flags & ValueFlags::DfDt) {
        std::string old = Cache->GetValue(Container->GetName(), Variable, true);
        if (old.length() == 0)
            old = AsString;
        AsNumber = DfDt(AsNumber, ParseValue(old, Flags & ValueFlags::Map), Cache->GetDt());
    }

    if (Flags & ValueFlags::PartOfRoot) {
        std::string root_raw = Cache->GetValue("/", Variable, false);
        double root_number;

        root_number = ParseValue(root_raw, Flags & ValueFlags::Map);

        if (Flags & ValueFlags::DfDt) {
            std::string old = Cache->GetValue("/", Variable, true);
            if (old.length() == 0)
                old = root_raw;
            root_number = DfDt(root_number, ParseValue(old, Flags & ValueFlags::Map), Cache->GetDt());
        }

        AsNumber = PartOf(AsNumber, root_number);
    }

    if (Flags & ValueFlags::Multiplier)
        AsNumber /= Multiplier;

    if (Flags & ValueFlags::Percents)
        AsString = StringFormat("%.1f", AsNumber * 100);
    else if (Flags & ValueFlags::Seconds)
        AsString = StringFormatDuration(AsNumber * 1000);
    else if (Flags & ValueFlags::Bytes)
        AsString = StringFormatSize(AsNumber);
    else
        AsString = StringFormat("%g", AsNumber);
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
    if (Container == "/") {
        Level = 0;
    } else {
        auto unprefixed = container.substr(strlen(ROOT_PORTO_NAMESPACE));
        Level = 1 + std::count(unprefixed.begin(), unprefixed.end(), '/');
    }
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

TPortoContainer* TPortoContainer::ContainerTree(Porto::Connection &api) {
    std::vector<std::string> containers;
    int ret = api.List(containers);
    if (ret)
        return nullptr;

    TPortoContainer *root = nullptr;
    TPortoContainer *curr = nullptr;
    TPortoContainer *prev = nullptr;
    int level = 0;

    std::string self_absolute_name;
    ret = api.GetProperty("self", "absolute_name", self_absolute_name);
    if (ret)
        return nullptr;

    std::string self_porto_ns;
    ret = api.GetProperty("self", "absolute_namespace", self_porto_ns);
    if (ret)
        return nullptr;

    for (auto &ct : containers)
        ct = self_porto_ns + ct;

    if (self_absolute_name != "/") {
        auto parent = self_absolute_name;
        int pos = parent.size();

        do {
            auto self_parent = parent.substr(0, pos);

            if (self_parent != "/porto" &&
                std::find(containers.begin(), containers.end(), self_parent)
                          == containers.end()) {

                containers.push_back(self_parent);
            }

            pos = pos ? parent.rfind("/", pos - 1) : std::string::npos;
        } while (pos != std::string::npos && pos);
    }

    std::sort(containers.begin(), containers.end());

    root = new TPortoContainer("/");
    prev = root;
    root->Tag = self_absolute_name == "/" ? PortoTreeTags::Self : PortoTreeTags::None;

    for (auto &c : containers) {
        if (c == "/")
            continue;

        curr = new TPortoContainer(c);

        if (c == self_absolute_name)
            curr->Tag |= PortoTreeTags::Self;

        level = curr->GetLevel();
        if (level > prev->GetLevel())
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
void TPortoContainer::ForEach(std::function<void (TPortoContainer&)> fn, int maxlevel) {
    if (Level <= maxlevel)
        fn(*this);
    if (Level < maxlevel)
        for (auto &c : Children)
            c->ForEach(fn, maxlevel);
}
int TPortoContainer::GetMaxLevel() {
    int level = Level;
    for (auto &c : Children)
        if (c->GetMaxLevel() > level)
            level = c->GetMaxLevel();
    return level;
}
std::string TPortoContainer::ContainerAt(int n, int max_level) {
    TPortoContainer *ret = this;
    int i = 0;
    ForEach([&] (TPortoContainer &row) {
            if (i++ == n)
                ret = &row;
        }, max_level);
    return ret->GetName();
}
int TPortoContainer::ChildrenCount() {
    return Children.size();
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
                   A_BOLD | (Selected ? A_STANDOUT : 0));
    return Width;
}
int TColumn::Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, bool selected) {
    std::string p = At(row).GetValue();
    screen.PrintAt(p, x, y, Width, LeftAligned, selected ? A_REVERSE : 0);
    return Width;
}
void TColumn::Update(TPortoContainer* tree, int maxlevel) {
    tree->ForEach([&] (TPortoContainer &row) {
            TPortoValue val(RootValue, &row);
            Cache.insert(std::make_pair(row.GetName(), val));
        }, maxlevel);
}
TPortoValue& TColumn::At(TPortoContainer &row) {
    return Cache[row.GetName()];
}
void TColumn::Highlight(bool enable) {
    Selected = enable;
}
void TColumn::Process() {
    for (auto &iter : Cache) {
        iter.second.Process();

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
        if (!y) {
            std::string p = "Version: ";
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = Cache.Version;
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;

            p = "Update: ";
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = Paused ? "paused" : StringFormatDuration(Delay);
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;
        }
        y++;
        x = 0;
    }
    return y;
}

void TPortoTop::Update() {
    for (auto &column : Columns)
        column.ClearCache();
    ContainerTree.reset(TPortoContainer::ContainerTree(*Api));
    if (!ContainerTree)
        return;
    for (auto &column : Columns)
        column.Update(ContainerTree.get(), MaxLevel);
    Cache.Update(*Api);
    Process();
}

void TPortoTop::Process() {
    for (auto &column : Columns)
        column.Process();
    for (auto &line : Common)
        for (auto &item : line)
            item.GetValue().Process();
    Sort();
}

void TPortoTop::Sort() {
    if (ContainerTree)
        ContainerTree->SortTree(Columns[SelectedColumn]);
}

void TPortoTop::Print(TConsoleScreen &screen) {

    screen.Erase();

    if (!ContainerTree)
        return;

    int width = 0;
    for (auto &column : Columns)
        width += column.GetWidth();

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

    int at_row = 1 + PrintCommon(screen);

    MaxRows = 0;
    ContainerTree->ForEach([&] (TPortoContainer &row) {
            if (SelectedContainer == "self" && (row.Tag & PortoTreeTags::Self))
                SelectedContainer = row.GetName();
            if (row.GetName() == SelectedContainer)
                SelectedRow = MaxRows;
            MaxRows++;
        }, MaxLevel);
    DisplayRows = std::min(screen.Height() - at_row, MaxRows);
    ChangeSelection(0, 0, screen);

    PrintTitle(at_row - 1, screen);
    int y = 0;
    SelectedContainer = "";
    ContainerTree->ForEach([&] (TPortoContainer &row) {
            if (y >= FirstRow && y < MaxRows) {
                bool selected = y == SelectedRow;
                if (selected)
                    SelectedContainer = row.GetName();
                int x = FirstX;
                for (auto &c : Columns)
                    x += 1 + c.Print(row, x, at_row + y - FirstRow, screen, selected);
            }
            y++;
        }, MaxLevel);
    screen.Refresh();
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

    if (desc == "state")
        flags = ValueFlags::State;

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
            case '/':
                flags |= ValueFlags::PartOfRoot;
                break;
            case '%':
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

void TPortoTop::ChangeSelection(int x, int y, TConsoleScreen &screen) {
    SelectedRow += y;

    if (SelectedRow < 0)
        SelectedRow = 0;

    if (SelectedRow >= MaxRows)
        SelectedRow = MaxRows - 1;

    if (SelectedRow < FirstRow)
        FirstRow = SelectedRow;

    if (SelectedRow >= FirstRow + DisplayRows)
        FirstRow = SelectedRow - DisplayRows + 1;

    if (FirstRow + DisplayRows > MaxRows)
        FirstRow = MaxRows - DisplayRows;

    Columns[SelectedColumn].Highlight(false);
    SelectedColumn += x;
    if (SelectedColumn < 0) {
        SelectedColumn = 0;
    } else if (SelectedColumn > (int)Columns.size() - 1) {
        SelectedColumn = Columns.size() - 1;
    }
    Columns[SelectedColumn].Highlight(true);

    if (x)
        Sort();

    if (y)
        SelectedContainer = "";

    if (x == 0 && y == 0) {
        int i = 0;
        int x = FirstX;
        for (auto &c : Columns) {
            if (i == SelectedColumn && x <= 0) {
                FirstX -= x;
                x = 0;
            }
            x += c.GetWidth() + 1;
            if (i == SelectedColumn && x > screen.Width()) {
                FirstX -= x - screen.Width();
                x = screen.Width();
            }
            i++;
        }
        if (FirstX < 0 && x < screen.Width())
            FirstX += std::min(screen.Width() - x, -FirstX);
    }
}
void TPortoTop::Expand() {
    if (MaxLevel == 1)
        MaxLevel = 2;
    else if (MaxLevel == 2)
        MaxLevel = 100;
    else
        MaxLevel = 1;
    Update();
}
int TPortoTop::StartStop() {
    std::string state;
    int ret = Api->GetProperty(SelectedContainer, "state", state);
    if (ret)
        return ret;
    if (state == "running" || state == "dead" || state == "meta")
        return Api->Stop(SelectedContainer);
    else
        return Api->Start(SelectedContainer);
}
int TPortoTop::PauseResume() {
    std::string state;
    int ret = Api->GetProperty(SelectedContainer, "state", state);
    if (ret)
        return ret;
    if (state == "paused")
        return Api->Resume(SelectedContainer);
    else
        return Api->Pause(SelectedContainer);
}
int TPortoTop::Kill(int signal) {
    return Api->Kill(SelectedContainer, signal);
}
int TPortoTop::Destroy() {
    return Api->Destroy(SelectedContainer);
}
void TPortoTop::LessPortoctl(std::string container, std::string cmd) {
    std::string s(program_invocation_name);
    s += " get " + container + " " + cmd + " | less";
    int status = system(s.c_str());
    (void)status;
}

int TPortoTop::RunCmdInContainer(TConsoleScreen &screen, std::string cmd) {
    bool enter = (SelectedContainer != "/" && SelectedContainer != "self");
    int ret = -1;

    screen.Save();
    switch (fork()) {
    case -1:
        ret = errno;
        break;
    case 0:
    {
        if (enter)
            exit(execlp(program_invocation_name, program_invocation_name,
                        "shell", SelectedContainer.c_str(), cmd.c_str(), nullptr));
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
void TPortoTop::AddCommon(int row, const std::string &title, const std::string &var,
                          TPortoContainer &container, int flags, double multiplier) {
    Common.resize(row + 1);
    TPortoValue v(Cache, &container, var, flags, multiplier);
    Common[row].push_back(TCommonValue(title, v));
}
TPortoTop::TPortoTop(Porto::Connection *api, const std::vector<std::string> &args) : Api(api), RootContainer("/") {
    AddCommon(0, "Containers running: ", "porto_stat[running]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "of ", "porto_stat[containers]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Volumes: ", "porto_stat[volumes]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Clients: ", "porto_stat[clients]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Errors: ", "porto_stat[errors]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Warnings: ", "porto_stat[warnings]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "RPS: ", "porto_stat[requests_completed]", RootContainer, ValueFlags::DfDt);
    AddCommon(0, "Uptime: ", "porto_stat[slave_uptime]", RootContainer, ValueFlags::Seconds);

    Config = {
        "state: state",
        "time: time s",

        /* CPU */
        "policy: cpu_policy",
        "cpu%: cpu_usage'% 1e9",
        "sys%: cpu_usage_system'% 1e9",
        "wait%: cpu_wait'% 1e9",
        "limit: cpu_limit",
        "g-e: cpu_guarantee",

        /* Memory */
        "memory: memory_usage b",
        "anon: anon_usage b",
        "cache: cache_usage b",
        "limit: memory_limit b",
        "g-e: memory_guarantee b",

        /* I/O */
        "maj/s: major_faults'",
        "read b/s: io_read[fs]' b",
        "write b/s: io_write[fs]' b",

        /* Network */
        "net tx: S(net_bytes) 'b",
        "net rx: S(net_rx_bytes) 'b",
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

static bool exit_immediatly = false;
void exit_handler(int unused) {
    exit_immediatly = true;
}

int portotop(Porto::Connection *api, const std::vector<std::string> &args) {
    Signal(SIGINT, exit_handler);
    Signal(SIGTERM, exit_handler);
    Signal(SIGTTOU, SIG_IGN);
    Signal(SIGTTIN, SIG_IGN);

    TPortoTop top(api, args);

    top.SelectedContainer = "self";

    top.Update();

    /* Main loop */
    TConsoleScreen screen;

    bool first = true;

    screen.SetTimeout(top.FirstDelay);

    while (true) {
        if (exit_immediatly)
            break;

        top.Print(screen);

        int button = screen.Getch();
        switch (button) {
        case ERR:
            if (!top.Paused)
                top.Update();
            break;
        case 'q':
        case 'Q':
            return EXIT_SUCCESS;
            break;
        case KEY_UP:
            top.ChangeSelection(0, -1, screen);
            break;
        case KEY_PPAGE:
            top.ChangeSelection(0, -10, screen);
            break;
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
        case KEY_HOME:
            top.ChangeSelection(-1000, 0, screen);
            break;
        case KEY_END:
            top.ChangeSelection(1000, 0, screen);
            break;
        case '\t':
            top.Expand();
            break;
        case ' ':
            top.Paused = !top.Paused;
            break;
        case 'S':
            if (screen.Dialog("Start/stop container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.StartStop())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 'P':
            if (screen.Dialog("Pause/resume container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.PauseResume())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 'K':
        {
            int signal = -1;
            switch (screen.Dialog("Kill container " + top.SelectedContainer,
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
            if (signal > 0) {
                if (top.Kill(signal))
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        }
        case 'D':
            if (screen.Dialog("Destroy container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.Destroy())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 't':
            top.RunCmdInContainer(screen, "top");
            break;
        case 'b':
            top.RunCmdInContainer(screen, "bash");
            break;
        case 'g':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "");
            screen.Restore();
            break;
        case 'o':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "stdout");
            screen.Restore();
            break;
        case 'e':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "stderr");
            screen.Restore();
            break;
        case '0':
            top.Delay = 10000;
            top.Paused = false;
            screen.SetTimeout(top.Delay);
            break;
        case '1'...'9':
            top.Delay = (button - '0') * 1000;
            top.Paused = false;
            screen.SetTimeout(top.Delay);
            break;
        case 'u':
            top.Update();
            screen.Clear();
            break;
        case 0:
        case KEY_RESIZE:
        case KEY_MOUSE:
            break;
        case 'h':
        case '?':
        default:
            screen.HelpDialog();
            break;
        }

        if (first) {
            first = false;
            screen.SetTimeout(top.Delay);
        }
    }

    return EXIT_SUCCESS;
}
