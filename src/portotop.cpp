#include <unordered_map>

#include "portotop.hpp"
#include "version.hpp"

extern "C" {
#include <getopt.h>
}

static void Usage() {
    std::cout
        << std::endl
        << "Usage: portoctl-top [options...]" << std::endl
        << std::endl
        << "Option: " << std::endl
        << "  -h | --help      print this message" << std::endl
        << "  -c | --cpu       show cpu stat columns" << std::endl
        << "  -m | --memory    show memory stat columns" << std::endl
        << "  -i | --io        show io stat columns" << std::endl
        << "  -n | --network   show network stat columns" << std::endl
        << "  -p | --porto     show porto stat columns" << std::endl
        << "  -f | --filter    containers mask" << std::endl
        << std::endl;
}

static bool ShowAll = true;
static bool ShowCpu = false;
static bool ShowMem = false;
static bool ShowIo = false;
static bool ShowNet = false;
static bool ShowPorto = false;
static std::string ContainersFilter = "***";

static double ParseNumber(const std::string &str) {
    return strtod(str.c_str(), nullptr);
}

static double ParseValue(const std::string &value, bool map) {
    if (!map)
        return ParseNumber(value);

    double ret = 0;
    TUintMap tmp;
    if (!StringToUintMap(value, tmp)) {
        for (auto it: tmp)
            ret += it.second;
    }
    return ret;
}

static double DfDt(double curr, double prev, uint64_t dt) {
    if (dt)
        return 1000.0 * (curr - prev) / dt;
    return 0;
}

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
    start_color();

    init_pair(1, COLOR_BLACK, COLOR_RED);
    init_pair(2, COLOR_BLACK, COLOR_GREEN);
    init_pair(3, COLOR_BLACK, COLOR_BLUE);
    init_pair(4, COLOR_BLACK, COLOR_YELLOW);
    init_pair(5, COLOR_BLACK, COLOR_MAGENTA);
    init_pair(6, COLOR_BLACK, COLOR_CYAN);

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
    pid_t pid = getpgrp();

    if (pid >= 0)
        tcsetpgrp(1, pid);

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
         "<, > - scroll without chaning sorting",
         "tab - expand conteainers tree: first, second, all",
         "@ - go to self container",
         "! - mark selected container",
         "H - hide unmarked containers",
         "",
         "1-9,0 - set update delay to 1s-9s and 10s",
         "space - pause/resume screen updates",
         "u - update screen",
         "",
         "d, del - disable column",
         "backspace - move column left",
         "f - choose columns",
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

void TConsoleScreen::ColumnsMenu(std::vector<TColumn> &columns) {
    const int MENU_SPACING = 2;

    const char CHECKED[] = " [*]  ";
    const char UNCHECKED[] = " [ ]  ";
    const int CHECKBOX_SIZE = strlen(CHECKED);

    const int BOX_BORDER = 2;


    int title_width = 0, desc_width = 0;

    for (auto &col : columns) {
        title_width = std::max(title_width, (int)col.Title.length());
        desc_width = std::max(desc_width, (int)col.Description.length());
    }

    int menu_width = title_width + desc_width + MENU_SPACING;
    int win_width = menu_width + BOX_BORDER + CHECKBOX_SIZE + MENU_SPACING;

    const int menu_lines = std::min((int)columns.size(),
                                    std::max(1, Height() - 6));

    const int win_height = menu_lines + BOX_BORDER + 2 + 1;

    int x0 = Width() / 2 - win_width / 2;
    int y0 = Height() / 2 - win_height / 2;

    WINDOW *win = newwin(win_height, win_width, y0, x0);

    box(win, 0, 0);
    wrefresh(win);

    std::vector<ITEM *> items;

    for (auto &col : columns) {
        auto item = new_item(col.Title.c_str(), col.Description.c_str());
        items.push_back(item);
    }

    items.push_back(NULL);

    mvwprintw(win, 1, 2, "Select displayed columns:");

    MENU *menu = new_menu(items.data());
    WINDOW *sub = derwin(win, menu_lines, menu_width, 3, BOX_BORDER / 2 + CHECKBOX_SIZE);

    set_menu_win(menu, win);
    set_menu_sub(menu, sub);
    set_menu_mark(menu, "");
    set_menu_format(menu, menu_lines, 1);
    set_menu_spacing(menu, MENU_SPACING, 0, 0);

    post_menu(menu);

    bool done = false;

    while (!done) {
        for (int i = 0; i < menu_lines; i++) {
            bool hidden = columns[top_row(menu) + i].Hidden;
            mvwprintw(win, 3 + i, 1, hidden ? UNCHECKED : CHECKED);
        }

        wrefresh(win);

        switch(Getch()) {
            case KEY_DOWN:
                menu_driver(menu, REQ_DOWN_ITEM);
                break;
            case KEY_UP:
                menu_driver(menu, REQ_UP_ITEM);
                break;
            case KEY_NPAGE:
                menu_driver(menu, REQ_SCR_DPAGE);
                break;
            case KEY_PPAGE:
                menu_driver(menu, REQ_SCR_UPAGE);
                break;
            case KEY_HOME:
                menu_driver(menu, REQ_FIRST_ITEM);
                break;
            case KEY_END:
                menu_driver(menu, REQ_LAST_ITEM);
                break;
            case 'f':
            case 'q':
            case 'Q':
            case '\n':
                done = true;
                break;
            case ' ':
                {
                    auto &value = columns[item_index(current_item(menu))].Hidden;
                    value = !value;
                }
                break;
        }
    }

    unpost_menu(menu);
    free_menu(menu);

    for (auto &item : items)
        if (item)
            free_item(item);

    delwin(sub);
    delwin(win);
    Refresh();
}

///////////////////////////////////////////////////////

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

TPortoValue& TPortoValue::operator=(const TPortoValue &src) {
    if (this != &src) {
        Cache = src.Cache;
        Container = src.Container;
        Variable = src.Variable;
        Flags = src.Flags;
        AsString = src.AsString;
        AsNumber = src.AsNumber;
        Multiplier = src.Multiplier;
    }
    return *this;
}

TPortoValue::TPortoValue(const TPortoValue &src, std::shared_ptr<TPortoContainer> &container) :
    Cache(src.Cache), Container(container), Variable(src.Variable), Flags(src.Flags),
    Multiplier(src.Multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::TPortoValue(std::shared_ptr<TPortoValueCache> &cache,
                         std::shared_ptr <TPortoContainer> &container,
                         const std::string &variable, int flags, double multiplier) :
    Cache(cache), Container(container), Variable(variable), Flags(flags),
    Multiplier(multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::~TPortoValue() {
    if (Cache && Container)
        Cache->Unregister(Container->GetName(), Variable);

    Container = nullptr;
}

void TPortoValue::Process() {
    if (!Container) {
        AsString = "";
        return;
    }

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
    else if (Flags & ValueFlags::Cores)
        AsString = StringFormat("%.1fc", AsNumber);
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
std::shared_ptr<TPortoContainer> TPortoContainer::GetParent(int level) {
    auto parent = Parent.lock();
    if (parent) {
        if (parent->GetLevel() == level)
            return parent;
        else
            return parent->GetParent(level);
    } else
        return nullptr;
}

std::shared_ptr<TPortoContainer> TPortoContainer::ContainerTree(Porto::Connection &api) {
    std::vector<std::string> containers;
    int ret = api.List(containers, ContainersFilter);
    if (ret)
        return nullptr;

    std::shared_ptr<TPortoContainer> root = nullptr;
    std::shared_ptr<TPortoContainer> curr = nullptr;
    std::shared_ptr<TPortoContainer> prev = nullptr;
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
        auto pos = parent.size();

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

    root = std::make_shared<TPortoContainer>("/");
    prev = root;
    root->Tag = self_absolute_name == "/" ? PortoTreeTags::Self : PortoTreeTags::None;

    for (auto &c : containers) {
        if (c == "/")
            continue;

        curr = std::make_shared<TPortoContainer>(c);

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

        auto parent = curr->Parent.lock();
        if (!parent)
            return nullptr;

        parent->Children.push_back(curr);
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
void TPortoContainer::ForEach(std::function<void (
                              std::shared_ptr<TPortoContainer> &)> fn, int maxlevel) {

    auto self = shared_from_this();

    if (Level <= maxlevel)
        fn(self);
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
    auto ret = shared_from_this();
    int i = 0;
    ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            if (i++ == n)
                ret = row;
        }, max_level);
    return ret->GetName();
}
int TPortoContainer::ChildrenCount() {
    return Children.size();
}


TColumn::TColumn(std::string title, std::string desc,
                 TPortoValue var, bool left_aligned, bool hidden) :

    RootValue(var), LeftAligned(left_aligned), Hidden(hidden),
    Title(title), Description(desc) {

    Width = title.length();
}
int TColumn::PrintTitle(int x, int y, TConsoleScreen &screen) {
    screen.PrintAt(Title, x, y, Width, LeftAligned,
                   A_BOLD | (Selected ? A_STANDOUT : 0));
    return Width;
}
int TColumn::Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, int attr) {
    std::string p = At(row).GetValue();
    screen.PrintAt(p, x, y, Width, LeftAligned, attr);
    return Width;
}
void TColumn::Update(std::shared_ptr<TPortoContainer> &tree, int maxlevel) {
    tree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            TPortoValue val(RootValue, row);
            Cache.insert(std::make_pair(row->GetName(), val));
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
    Children.sort([&] (std::shared_ptr<TPortoContainer> &row1,
                       std::shared_ptr<TPortoContainer> &row2) {
            return column.At(*row1) < column.At(*row2);
        });
    for (auto &c : Children)
        c->SortTree(column);
}

void TPortoTop::PrintTitle(int y, TConsoleScreen &screen) {
    int x = FirstX;
    for (auto &c : Columns)
        if (!c.Hidden)
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
            p = Cache->Version;
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;

            p = "Update: ";
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = Paused ? "paused" : StringFormatDuration(Delay);
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
        }

        y++;
        x = 0;
    }
    return y;
}

void TPortoTop::Update() {
    for (auto &column : Columns)
        column.ClearCache();
    ContainerTree = TPortoContainer::ContainerTree(*Api);
    if (!ContainerTree)
        return;
    for (auto &column : Columns)
        column.Update(ContainerTree, MaxLevel);
    Cache->Update(*Api);
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
        if (!column.Hidden)
            width += column.GetWidth();

    if (Columns[0].GetWidth() > screen.Width() / 2)
        Columns[0].SetWidth(screen.Width() / 2);

    int at_row = 1 + PrintCommon(screen);

    MaxRows = 0;
    ContainerTree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            if (SelectedContainer == "self" && (row->Tag & PortoTreeTags::Self))
                SelectedContainer = row->GetName();
            if (row->GetName() == SelectedContainer)
                SelectedRow = MaxRows;
            MaxRows++;
        }, MaxLevel);
    DisplayRows = std::min(screen.Height() - at_row, MaxRows);

    PrintTitle(at_row - 1, screen);
    int y = 0;
    int hiddenRows = 0;
    SelectedContainer = "";
    std::set<std::string> containers;
    ContainerTree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            containers.insert(row->GetName());
            if (y >= FirstRow && y < MaxRows) {
                if (!FilterMode || RowColor.find(row->GetName()) != RowColor.end()) {
                    bool selected = (!FilterMode && y == SelectedRow) || (FilterMode && (y - hiddenRows) == SelectedRow);
                    if (selected)
                        SelectedContainer = row->GetName();
                    int x = FirstX;

                    int attr = 0;
                    if (selected)
                        attr |= A_REVERSE;
                    auto col = RowColor.find(row->GetName());
                    if (col != RowColor.end() && !FilterMode)
                        attr |= COLOR_PAIR(col->second);

                    for (auto &c : Columns) {
                        if (!c.Hidden)
                            x += 1 + c.Print(*row, x, at_row + y - FirstRow - hiddenRows,
                                             screen, attr);
                    }
                } else {
                    hiddenRows++;
                }
            }
            y++;
        }, MaxLevel);

    std::set<std::string> destroyedContainers;
    for (const auto &it : RowColor) {
       if (containers.find(it.first) == containers.end())
           destroyedContainers.insert(it.first);
    }

    for (const auto &container : destroyedContainers)
        RowColor.erase(container);

    screen.Refresh();
}

void TPortoTop::MarkRow() {
    if (RowColor.count(SelectedContainer)) {
        RowColor.erase(SelectedContainer);
    } else {
        RowColor[SelectedContainer] = NextColor;
        if (++NextColor > 6)
            NextColor = 1;
    }
}

void TPortoTop::HideRows() {
    FilterMode ^= true;
}

void TPortoTop::AddColumn(const TColumn &c) {
    Columns.push_back(c);
}
bool TPortoTop::AddColumn(std::string title, std::string signal,
                          std::string desc, bool hidden) {

    int flags = ValueFlags::Raw;
    size_t off = 0;
    std::string data;

    if (signal == "state")
        flags = ValueFlags::State;

    if (signal.length() > 4 && signal[0] == 'S' && signal[1] == '(') {
        off = signal.find(')');
        data = signal.substr(2, off == std::string::npos ?
                           std::string::npos : off - 2);
        flags |= ValueFlags::Map;
        if (off != std::string::npos)
            off++;
    } else {
        off = signal.find('\'');
        if (off == std::string::npos)
            off = signal.rfind(' ');
        if (off == std::string::npos)
            off = signal.find('%');

        data = signal.substr(0, off);
    }

    double multiplier = 1;

    if (off != std::string::npos) {
        for (; off < signal.length(); off++) {
            switch (signal[off]) {
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
            case 'c':
            case 'C':
                flags |= ValueFlags::Cores;
                break;
            case ' ':
                break;
            default:
                {
                    char *endp;
                    multiplier = strtod(signal.c_str() + off, &endp);
                    off = endp - signal.c_str();
                    flags |= ValueFlags::Multiplier;
                }
                break;
            }
        }
    }

    TPortoValue v(Cache, RootContainer, data, flags, multiplier);
    Columns.push_back(TColumn(title, desc, v, false, hidden));
    return true;
}

void TPortoTop::ChangeSelection(int x, int y, TConsoleScreen &screen) {
    SelectedRow += y;

    if (SelectedRow < 0)
        SelectedRow = 0;

    if (FilterMode && SelectedRow >= (int)RowColor.size())
        SelectedRow = RowColor.size() - 1;

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
    while (Columns[SelectedColumn].Hidden && x < 0 && SelectedColumn > 0)
        SelectedColumn--;
    while (Columns[SelectedColumn].Hidden && SelectedColumn < (int)Columns.size() - 1)
        SelectedColumn++;
    while (Columns[SelectedColumn].Hidden && SelectedColumn > 0)
        SelectedColumn--;
    Columns[SelectedColumn].Highlight(true);

    if (x)
        Sort();

    if (y)
        SelectedContainer = "";

    if (x) {
        int i = 0;
        int _x = FirstX;
        for (auto &c : Columns) {
            if (i == SelectedColumn && _x <= 0) {
                FirstX -= _x;
                _x = 0;
            }
            if (!c.Hidden)
                _x += c.GetWidth() + 1;
            if (i == SelectedColumn && _x > screen.Width()) {
                FirstX -= _x - screen.Width();
                _x = screen.Width();
            }
            i++;
        }
        if (FirstX < 0 && _x < screen.Width())
            FirstX += std::min(screen.Width() - _x, -FirstX);
    }
}

void TPortoTop::ChangeView(int x, int y) {
    FirstX += x;
    if (FirstX > 0)
        FirstX = 0;
    FirstRow += y;
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
                          std::shared_ptr<TPortoContainer> &container,
                          int flags, double multiplier) {
    Common.resize(row + 1);
    TPortoValue v(Cache, container, var, flags, multiplier);
    Common[row].push_back(TCommonValue(title, v));
}
TPortoTop::TPortoTop(Porto::Connection *api, const std::vector<std::string> &args) :
    Api(api),
    Cache(std::make_shared<TPortoValueCache>()),
    RootContainer(std::make_shared<TPortoContainer>("/")) {

    (void)args;

    AddCommon(0, "Containers running: ", "porto_stat[running]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "of ", "porto_stat[containers]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Volumes: ", "porto_stat[volumes]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Networks: ", "porto_stat[networks]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Clients: ", "porto_stat[clients]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Uptime: ", "porto_stat[porto_uptime]", RootContainer, ValueFlags::Seconds);

    AddCommon(1, "Started: ", "porto_stat[containers_started]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "Failed: ", "porto_stat[containers_failed_start]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "Errors: ", "porto_stat[errors]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "Warnings: ", "porto_stat[warnings]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "Unknown: ", "porto_stat[fail_system]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "OOM: ", "porto_stat[containers_oom]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "CPS: ", "porto_stat[clients_connected]", RootContainer, ValueFlags::DfDt);
    AddCommon(1, "RPS: ", "porto_stat[requests_completed]", RootContainer, ValueFlags::DfDt);
    AddCommon(1, "FPS: ", "porto_stat[requests_failed]", RootContainer, ValueFlags::DfDt);
    AddCommon(1, "VAL: ", "porto_stat[fail_invalid_value]", RootContainer, ValueFlags::Raw);
    AddCommon(1, "CMD: ", "porto_stat[fail_invalid_command]", RootContainer, ValueFlags::Raw);

    AddColumn(TColumn("Container", "Container name",
              TPortoValue(Cache, ContainerTree, "absolute_name", ValueFlags::Container), true, false));

    AddColumn("State", "state", "Current state");
    AddColumn("Time", "time s", "Time elapsed since start or death");

    /* CPU */
    if (ShowAll || ShowCpu) {
        AddColumn("Cpu%", "cpu_usage'% 1e9", "Cpu usage in core%");
        AddColumn("Sys%", "cpu_usage_system'% 1e9", "System cpu usage in core%");
        AddColumn("Wait%", "cpu_wait'% 1e9", "Cpu wait time in core%");
        AddColumn("Thld%", "cpu_throttled'% 1e9", "Cpu throttled time in core%");

        AddColumn("Bcpu%", "cpu_burst_usage'% 1e9", "Cpu burst usage time in core%");
        AddColumn("Uwait%", "cpu_unconstrained_wait'% 1e9", "Cpu unconstrained wait time in core%");

        AddColumn("C pol", "cpu_policy", "Cpu scheduler policy");
        AddColumn("C g-e", "cpu_guarantee c", "Cpu guarantee in cores");
        AddColumn("C lim", "cpu_limit c", "Cpu limit in cores");

        AddColumn("Ct lim", "cpu_limit_total c", "Cpu total limit in cores");
        AddColumn("Ct g-e", "cpu_guarantee_total c", "Cpu total guarantee in cores");

        AddColumn("Threads", "thread_count", "Threads count");
    }

    /* Memory */
    if (ShowAll || ShowMem) {
        AddColumn("Memory", "memory_usage b", "Memory usage");
        AddColumn("M g-e", "memory_guarantee b", "Memory guarantee");
        AddColumn("M lim", "memory_limit b", "Memory limit");
        AddColumn("M r-d/s", "memory_reclaimed' b", "Memory reclaimed");

        AddColumn("Anon", "anon_usage b", "Anonymous memory usage");
        AddColumn("A lim", "anon_limit b", "Anonymous memory limit");

        AddColumn("Cache", "cache_usage b", "Cache memory usage");
        AddColumn("Shmem", "shmem_usage b", "Shmem and tmpfs usage");
        AddColumn("MLock", "mlock_usage b", "Locked memory");

        AddColumn("Mt lim", "memory_limit_total b", "Memory total limit");
        AddColumn("Mt g-e", "memory_guarantee_total b", "Memory total guarantee");

        AddColumn("OOM", "porto_stat[container_oom]", "OOM count");
    }

    /* I/O */
    if (ShowAll || ShowIo){
        AddColumn("Maj/s", "major_faults'", "Major page fault count");
        AddColumn("Min/s", "minor_faults'", "Minor page fault count");

        AddColumn("IO load", "io_time[hw]' 1e9", "Average disk queue depth");

        AddColumn("IO op/s", "io_ops[hw]'", "IO operations per second");
        AddColumn("IO Read b/s", "io_read[hw]' b", "IO bytes read from disk");
        AddColumn("IO Write b/s", "io_write[hw]' b", "IO bytes written to disk");

        AddColumn("FS op/s", "io_ops[fs]'", "IO operations by fs");
        AddColumn("FS read b/s", "io_read[fs]' b", "IO bytes read by fs");
        AddColumn("FS write b/s", "io_write[fs]' b", "IO bytes written by fs");
    }

    /* Network */
    if (ShowAll || ShowNet) {
        AddColumn("RX Lim", "net_rx_limit[default] b", "Default network RX limit");
        AddColumn("TX g-e", "net_guarantee[default] b", "Default network TX guarantee");
        AddColumn("TX lim", "net_limit[default] b", "Default network TX limit");

        AddColumn("Net RX", "net_rx_bytes[Uplink]' b", "Uplink bytes received");
        AddColumn("Net TX", "net_bytes[Uplink]' b", "Uplink bytes transmitted");
        AddColumn("Pkt RX", "net_rx_packets[Uplink]'", "Uplink packets received");
        AddColumn("Pkt TX", "net_packets[Uplink]'", "Uplink packets transmitted");
    }

    /* Porto */
    if (ShowAll || ShowPorto) {
        AddColumn("Porto", "enable_porto", "Porto access level");
        AddColumn("Cli", "porto_stat[container_clients]", "Porto clients");
        AddColumn("RPS", "porto_stat[container_requests]'", "Porto requests/s");
    }
}

static bool exit_immediatly = false;
void exit_handler(int) {
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
        case '<':
            top.ChangeView(1, 0);
            break;
        case '>':
            top.ChangeView(-1, 0);
            break;
        case '\t':
            top.Expand();
            break;
        case ' ':
            top.Paused = !top.Paused;
            break;
        case 'f':
            screen.ColumnsMenu(top.Columns);
            break;
        case KEY_DC:
        case 'd':
            if (top.SelectedColumn > 0)
                top.Columns[top.SelectedColumn].Hidden ^= true;
            break;
        case KEY_BACKSPACE:
            if (top.SelectedColumn > 1) {
                top.SelectedColumn--;
                std::swap(top.Columns[top.SelectedColumn],
                          top.Columns[top.SelectedColumn + 1]);
            }
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
        case '!':
            top.MarkRow();
            break;
        case 'H':
            top.HideRows();
            top.ChangeSelection(0, -1000, screen);
            break;
        case '@':
            top.SelectedContainer = "self";
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

int main(int argc, char *argv[]) {
    int opt = 0;

    static struct option long_options[] = {
        {"help",     no_argument,       0,  'h' },
        {"cpu",      no_argument,       0,  'c' },
        {"memory",   no_argument,       0,  'm' },
        {"io",       no_argument,       0,  'i' },
        {"network",  no_argument,       0,  'n' },
        {"porto",    no_argument,       0,  'p' },
        {"filter",   required_argument, 0,  'f' },
        {0,          0,                 0,   0   }
    };

    int long_index =0;
    while ((opt = getopt_long(argc, argv,"hcminpf:", long_options, &long_index )) != -1) {
        switch (opt) {
            case 'h':
                Usage();
                return EXIT_SUCCESS;
            case 'c':
                ShowCpu = true;
                ShowAll = false;
                break;
            case 'm':
                ShowMem = true;
                ShowAll = false;
                break;
            case 'i':
                ShowIo = true;
                ShowAll = false;
                break;
            case 'n':
                ShowNet = true;
                ShowAll = false;
                break;
            case 'p':
                ShowPorto = true;
                ShowAll = false;
                break;
            case 'f':
                ContainersFilter = optarg;
                break;
            default:
                Usage();
                return EXIT_FAILURE;
        }
    }

    const std::vector<std::string> args(argv + 1, argv + argc);
    Porto::Connection api;

    return portotop(&api, args);
}
