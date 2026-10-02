// modpacks ui
// got big fast, just adding stuff as needed
#include "ModpacksUI.hpp"
#include "ModpackManager.hpp"
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/web.hpp>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#endif

using namespace geode::prelude;

// little helper to make a small button
static CCMenuItemSpriteExtra* makeButton(
    const char* text, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler cb, float scale = 0.55f
) {
    auto spr = ButtonSprite::create(text);
    spr->setScale(scale);
    return CCMenuItemSpriteExtra::create(spr, target, cb);
}

static std::string modNameOf(Mod* m) {
    return std::string(m->getName().data(), m->getName().size());
}

// just our mods sorted by name so the list isnt random every time
static std::vector<Mod*> sortedUserMods() {
    std::vector<Mod*> out;
    for (auto m : Loader::get()->getAllMods()) {
        if (!m || m->isInternal()) continue;
        if (m->getID() == Mod::get()->getID()) continue;
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](Mod* a, Mod* b) {
        return modNameOf(a) < modNameOf(b);
    });
    return out;
}

static const char* platformKey() {
    // for downloading the right version from the index
#ifdef GEODE_IS_WINDOWS
    return "win";
#elif defined(GEODE_IS_ANDROID)
    return "android64";
#elif defined(GEODE_IS_MACOS)
    return "mac-arm";
#elif defined(GEODE_IS_IOS)
    return "ios";
#else
    return "win";
#endif
}

#ifdef GEODE_IS_WINDOWS
// windows file picker, windows only
// got most of this from youtube
static std::optional<std::filesystem::path> pickGmpkgFile() {
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "Geode Modpack (*.gmpkg)\0*.gmpkg\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = "Load Modpack - choose a .gmpkg file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        return std::filesystem::path(buf);
    }
    return std::nullopt;
}
#endif

struct DownloadResult {
    bool ok = false;
    std::string message;
};

// asks geode index for the download link
// tries exact version first, then same gd version, then latest
static DownloadResult fetchDownloadURL(
    std::string const& id, std::string const& wantVersion,
    std::string const& gameVersion, Mod* owner,
    std::string& outURL
) {
    web::WebRequest req;
    auto res = req.getSync("https://api.geode-sdk.org/v1/mods/" + id, owner);
    if (!res.ok()) {
        return {false, fmt::format("{} (not on index)", id)};
    }
    auto json = res.json();
    if (json.isErr()) return {false, fmt::format("{} (bad index reply)", id)};
    auto versions = json.unwrap()["payload"]["versions"];
    if (!versions.isArray()) return {false, fmt::format("{} (no versions)", id)};

    std::string fallbackURL, latestURL;
    const char* plat = platformKey();
    for (size_t i = 0; i < versions.size(); i++) {
        auto& v = versions[i];
        std::string ver = v["version"].asString().unwrapOr("");
        std::string link = v["download_link"].asString().unwrapOr("");
        if (ver.empty() || link.empty()) continue;
        if (!wantVersion.empty() && ver == wantVersion) {
            outURL = link;
            return {true, ""};
        }
        if (fallbackURL.empty() && v["gd"][plat].asString().unwrapOr("") == gameVersion) {
            fallbackURL = link;
        }
        if (latestURL.empty()) {
            latestURL = link;
        }
    }
    if (!fallbackURL.empty()) {
        outURL = fallbackURL;
        return {true, ""};
    }
    if (!latestURL.empty()) {
        outURL = latestURL;
        return {true, ""};
    }
    return {false, fmt::format("{} (no downloadable version)", id)};
}

// downloads the .geode file into the mods folder
static DownloadResult downloadModFile(
    std::string const& id, std::string const& url, Mod* owner
) {
    web::WebRequest req;
    auto res = req.getSync(url, owner);
    if (!res.ok()) return {false, fmt::format("{} (download failed)", id)};
    auto dest = geode::dirs::getModsDir() / (id + ".geode");
    auto wr = res.into(dest);
    if (wr.isErr()) return {false, fmt::format("{} (could not save)", id)};
    return {true, ""};
}

class LoadingPopup : public geode::Popup {
    // installs a pack, enables what we have + downloads the rest
protected:
    std::filesystem::path m_path;
    gmpkg::Pack m_pack;
    std::vector<std::string> m_enabled;
    std::vector<std::string> m_missing;
    std::vector<std::string> m_failed;
    std::vector<std::string> m_downloaded;
    std::atomic<size_t> m_dlDone{0};
    std::atomic<bool> m_dlFinished{false};
    std::atomic<bool> m_workerStarted{false};
    std::mutex m_mutex;
    std::string m_phase = "Starting...";
    ProgressBar* m_bar = nullptr;
    CCLabelBMFont* m_status = nullptr;
    Mod* m_owner = nullptr;

    bool init(std::filesystem::path path) {
        if (!geode::Popup::init(300.f, 150.f)) return false;
        this->setTitle("Loading Modpack");
        m_path = std::move(path);
        m_owner = Mod::get();

        auto man = gmpkg::readManifest(m_path);
        if (man.isErr()) return false;
        m_pack = man.unwrap();

        // enable whatever is already installed right away
        for (auto& m : m_pack.mods) {
            if (Loader::get()->isModInstalled(m.id)) {
                if (auto inst = Loader::get()->getInstalledMod(m.id)) {
                    if (inst->enable().isOk()) m_enabled.push_back(m.id);
                    else m_failed.push_back(m.id + " (error)");
                }
            } else {
                m_missing.push_back(m.id);
            }
        }

        auto size = m_mainLayer->getContentSize();
        std::string packName = m_pack.name;
        if (packName.size() > 26) packName = packName.substr(0, 25) + ".";
        auto nameLabel = CCLabelBMFont::create(packName.c_str(), "bigFont.fnt");
        nameLabel->setScale(0.36f);
        nameLabel->setColor(ccc3(255, 220, 130));
        nameLabel->setPosition({size.width / 2, size.height / 2 + 42});
        m_mainLayer->addChild(nameLabel);

        m_status = CCLabelBMFont::create("Starting...", "bigFont.fnt");
        m_status->setScale(0.38f);
        m_status->setPosition({size.width / 2, size.height / 2 + 16});
        m_mainLayer->addChild(m_status);

        m_bar = ProgressBar::create(ProgressBarStyle::Level);
        m_bar->setScale(0.55f);
        m_bar->showProgressLabel(true);
        m_bar->setPosition({size.width / 2, size.height / 2 - 16});
        m_mainLayer->addChild(m_bar);
        m_bar->updateProgress(0.f);

        if (m_closeBtn) m_closeBtn->setVisible(false);
        this->schedule(schedule_selector(LoadingPopup::tick), 0.05f);
        return true;
    }

    void setProgress(std::string const& label) {
        size_t total = m_missing.empty() ? 1 : m_missing.size();
        float pct = 100.f * (float)std::min(m_dlDone.load(), total) / (float)total;
        m_bar->updateProgress(pct);
        m_status->setString(label.c_str());
    }

    void tick(float) {
        // downloads run on a thread, here just update the bar
        if (!m_workerStarted.exchange(true)) {
            if (m_missing.empty()) {
                m_dlFinished.store(true);
            } else {
                this->startDownloads();
                return;
            }
        }
        std::string phase;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            phase = m_phase;
        }
        this->setProgress(phase);
        if (m_dlFinished.load()) {
            this->unschedule(schedule_selector(LoadingPopup::tick));
            this->finish();
        }
    }

    void startDownloads() {
        // needs its own thread or the popup freezes
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_phase = "Downloading...";
        }
        gmpkg::Pack pack = m_pack;
        std::vector<std::string> ids = m_missing;
        Mod* owner = m_owner;
        std::string gameVer = Loader::get()->getGameVersion();
        std::unordered_map<std::string, std::string> want;
        for (auto& e : pack.mods) want[e.id] = e.version;
        std::thread([this, ids, want, gameVer, owner] {
            for (auto& id : ids) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_phase = fmt::format("Downloading {}...", id);
                }
                std::string url;
                auto meta = fetchDownloadURL(id, want.count(id) ? want.at(id) : "",
                    gameVer, owner, url);
                if (!meta.ok) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_failed.push_back(meta.message);
                } else {
                    auto dl = downloadModFile(id, url, owner);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (dl.ok) m_downloaded.push_back(id);
                    else m_failed.push_back(dl.message);
                }
                m_dlDone.fetch_add(1);
            }
            m_dlFinished.store(true);
        }).detach();
    }

    void finish() {
        size_t okCount = m_enabled.size() + m_downloaded.size();
        std::string msg;
        if (m_failed.empty()) {
            msg = fmt::format(
                "Installed all {} mods in this pack. "
                "Restart Geometry Dash for changes to take effect?",
                okCount
            );
        } else {
            msg = fmt::format("Installed {}/{} mods", okCount, m_pack.mods.size());
            msg += ". Problems: ";
            for (size_t i = 0; i < m_failed.size() && i < 5; i++) {
                if (i) msg += ", ";
                msg += m_failed[i];
            }
            if (m_failed.size() > 5) msg += "...";
            msg += ". Restart Geometry Dash for changes to take effect?";
        }
        this->onClose(nullptr);
        geode::createQuickPopup(
            "Modpack Installed", msg, "Not Now", "Restart Now",
            [](FLAlertLayer*, bool btn2) {
                if (btn2) geode::utils::game::restart(true);
            }
        );
    }

public:
    static LoadingPopup* create(std::filesystem::path path) {
        auto ret = new LoadingPopup();
        if (ret->init(std::move(path))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void openPackFile(std::filesystem::path path) {
    if (auto pop = LoadingPopup::create(std::move(path))) {
        pop->show();
    } else {
        Notification::create("couldnt read that pack", NotificationIcon::Error)->show();
    }
}

// little < > arrows for paging
static CCMenuItemSpriteExtra* makeArrowButton(
    bool right, cocos2d::CCObject* target, cocos2d::SEL_MenuHandler cb
) {
    CCNode* spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
    if (spr) {
        spr->setScale(0.55f);
        if (right) static_cast<CCSprite*>(spr)->setFlipX(true);
    } else {
        auto label = CCLabelBMFont::create(right ? ">" : "<", "bigFont.fnt");
        label->setScale(0.6f);
        spr = label;
    }
    return CCMenuItemSpriteExtra::create(spr, target, cb);
}

class CreateModpackPopup : public geode::Popup {
    // the pick-your-mods screen
    // 5 per page, more looked cramped
protected:
    static constexpr size_t PAGE_SIZE = 5;
    TextInput* m_nameInput = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    CCMenu* m_rowMenu = nullptr;
    CCLayer* m_rowLayer = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    std::vector<Mod*> m_allMods;
    std::unordered_set<std::string> m_selected;
    size_t m_page = 0;
    geode::Function<void()> m_onDone;

    void addListPanel(float x, float y, float w, float h) {
        auto panel = CCScale9Sprite::create("GJ_square02.png");
        panel->setContentSize({w, h});
        panel->setPosition({x + w / 2, y + h / 2});
        m_mainLayer->addChild(panel);
    }

    bool init(geode::Function<void()> onDone) {
        if (!geode::Popup::init(380.f, 280.f)) return false;
        this->setTitle("Create Pack");
        m_onDone = std::move(onDone);

        auto size = m_mainLayer->getContentSize();

        m_hint = CCLabelBMFont::create("", "bigFont.fnt");
        m_hint->setScale(0.34f);
        m_hint->setColor(ccc3(255, 220, 130));
        m_hint->setPosition({size.width / 2, size.height - 38});
        m_mainLayer->addChild(m_hint);

        auto nameLabel = CCLabelBMFont::create("Name:", "bigFont.fnt");
        nameLabel->setScale(0.4f);
        nameLabel->setAnchorPoint({0, 0.5f});
        nameLabel->setPosition({28, size.height - 60});
        m_mainLayer->addChild(nameLabel);

        m_nameInput = TextInput::create(228.f, "My Modpack");
        m_nameInput->setMaxCharCount(48);
        m_nameInput->setPosition({size.width / 2 + 42, size.height - 60});
        m_mainLayer->addChild(m_nameInput);

        auto divider = CCScale9Sprite::create("GJ_square02.png");
        divider->setContentSize({330, 3});
        divider->setPosition({size.width / 2, size.height - 78});
        m_mainLayer->addChild(divider);

        this->addListPanel(25, 48, 330, 140);

        m_rowLayer = CCLayer::create();
        m_mainLayer->addChild(m_rowLayer);
        m_rowMenu = CCMenu::create();
        m_rowMenu->setPosition({0, 0});
        m_mainLayer->addChild(m_rowMenu);

        m_allMods = sortedUserMods();
        // preselect enabled ones, thats usually what people want
        for (auto m : m_allMods) {
            if (m->isOrWillBeEnabled()) {
                m_selected.emplace(m->getID().data(), m->getID().size());
            }
        }
        this->showPage();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto allBtn = makeButton("All", this, menu_selector(CreateModpackPopup::onSelectAll), 0.5f);
        allBtn->setPosition({38, 24});
        menu->addChild(allBtn);

        auto noneBtn = makeButton("None", this, menu_selector(CreateModpackPopup::onSelectNone), 0.5f);
        noneBtn->setPosition({96, 24});
        menu->addChild(noneBtn);

        auto prev = makeArrowButton(false, this, menu_selector(CreateModpackPopup::onPrevPage));
        prev->setPosition({150, 24});
        menu->addChild(prev);

        m_pageLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_pageLabel->setScale(0.3f);
        m_pageLabel->setPosition({190, 24});
        m_mainLayer->addChild(m_pageLabel);

        auto next = makeArrowButton(true, this, menu_selector(CreateModpackPopup::onNextPage));
        next->setPosition({230, 24});
        menu->addChild(next);

        auto blueSpr = ButtonSprite::create("Create Pack", "bigFont.fnt", "GJ_button_02.png");
        blueSpr->setScale(0.55f);
        auto createBtn = CCMenuItemSpriteExtra::create(
            blueSpr, this, menu_selector(CreateModpackPopup::onCreate)
        );
        createBtn->setPosition({size.width - 72, 24});
        menu->addChild(createBtn);

        this->updatePageLabel();
        return true;
    }

    size_t pageCount() const {
        return std::max<size_t>(1, (m_allMods.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    void updatePageLabel() {
        if (m_pageLabel) {
            m_pageLabel->setString(
                fmt::format("{}/{}", m_page + 1, this->pageCount()).c_str()
            );
        }
        if (m_hint) {
            m_hint->setString(
                fmt::format("Pick mods ({} selected):", m_selected.size()).c_str()
            );
        }
    }

    void showPage() {
        m_rowLayer->removeAllChildrenWithCleanup(true);
        m_rowMenu->removeAllChildrenWithCleanup(true);

        float px = 25, py = 48, pw = 330, ph = 140;
        float rowH = 27.f;

        if (m_allMods.empty()) {
            auto empty = CCLabelBMFont::create("No other mods installed.", "bigFont.fnt");
            empty->setScale(0.4f);
            empty->setPosition({px + pw / 2, py + ph / 2});
            m_rowLayer->addChild(empty);
            return;
        }

        size_t start = m_page * PAGE_SIZE;
        size_t end = std::min(start + PAGE_SIZE, m_allMods.size());
        for (size_t i = start; i < end; i++) {
            size_t row = i - start;
            float y = py + ph - 15 - row * rowH;
            Mod* mod = m_allMods[i];
            std::string id(mod->getID().data(), mod->getID().size());

            if (auto logo = geode::createModLogo(mod)) {
                limitNodeSize(logo, {20, 20}, 1.f, 0.1f);
                logo->setPosition({px + 18, y});
                m_rowLayer->addChild(logo);
            }

            std::string label = modNameOf(mod);
            if (label.size() > 18) label = label.substr(0, 17) + ".";
            label += " (";
            label += mod->getVersion().toVString();
            label += ")";
            auto text = CCLabelBMFont::create(label.c_str(), "bigFont.fnt");
            text->setScale(0.34f);
            text->setAnchorPoint({0, 0.5f});
            text->setPosition({px + 36, y});
            m_rowLayer->addChild(text);

            bool on = m_selected.count(id) > 0;
            auto offSpr = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
            auto onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
            if (offSpr && onSpr) {
                offSpr->setScale(0.55f);
                onSpr->setScale(0.55f);
                auto tog = CCMenuItemToggler::create(
                    offSpr, onSpr, this, menu_selector(CreateModpackPopup::onRowToggle)
                );
                tog->setSizeMult(1.6f);
                tog->setPosition({px + pw - 24, y});
                tog->setTag((int)i);
                tog->toggle(on);
                m_rowMenu->addChild(tog);
            } else {
                // fallback if sprites missing
                auto state = CCLabelBMFont::create(on ? "ON" : "OFF", "bigFont.fnt");
                state->setScale(0.42f);
                state->setColor(on ? ccc3(0, 255, 0) : ccc3(160, 160, 160));
                auto btn = CCMenuItemSpriteExtra::create(
                    state, this, menu_selector(CreateModpackPopup::onRowToggle)
                );
                btn->setSizeMult(1.4f);
                btn->setPosition({px + pw - 26, y});
                btn->setTag((int)i);
                m_rowMenu->addChild(btn);
            }
        }
    }

    void onRowToggle(CCObject* sender) {
        int i = sender->getTag();
        if (i < 0 || (size_t)i >= m_allMods.size()) return;
        Mod* mod = m_allMods[(size_t)i];
        std::string id(mod->getID().data(), mod->getID().size());
        if (auto tog = typeinfo_cast<CCMenuItemToggler*>(sender)) {
            if (tog->isToggled()) m_selected.insert(id);
            else m_selected.erase(id);
        } else if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            auto state = static_cast<CCLabelBMFont*>(btn->getNormalImage());
            if (m_selected.count(id)) {
                m_selected.erase(id);
                state->setString("OFF");
                state->setColor(ccc3(160, 160, 160));
            } else {
                m_selected.insert(id);
                state->setString("ON");
                state->setColor(ccc3(0, 255, 0));
            }
        }
        this->updatePageLabel();
    }

    void onPrevPage(CCObject*) {
        if (m_page > 0) {
            m_page--;
            this->showPage();
            this->updatePageLabel();
        }
    }

    void onNextPage(CCObject*) {
        if (m_page + 1 < this->pageCount()) {
            m_page++;
            this->showPage();
            this->updatePageLabel();
        }
    }

    void setAll(bool on) {
        m_selected.clear();
        if (on) {
            for (auto m : m_allMods) {
                m_selected.emplace(m->getID().data(), m->getID().size());
            }
        }
        this->showPage();
        this->updatePageLabel();
    }

    void onSelectAll(CCObject*) { this->setAll(true); }
    void onSelectNone(CCObject*) { this->setAll(false); }

    void onCreate(CCObject*) {
        std::string name = m_nameInput->getString().c_str();
        name = gmpkg::sanitizeName(name);
        if (name.empty()) {
            Notification::create("give your pack a name first", NotificationIcon::Error)->show();
            return;
        }
        gmpkg::Pack pack;
        pack.name = name;
        pack.gameVersion = Loader::get()->getGameVersion();
        pack.created = (long long)std::time(nullptr);
        for (auto m : m_allMods) {
            std::string id(m->getID().data(), m->getID().size());
            if (m_selected.count(id)) {
                pack.mods.push_back({id, m->getVersion().toVString()});
            }
        }
        if (pack.mods.empty()) {
            Notification::create("pick at least one mod", NotificationIcon::Error)->show();
            return;
        }
        // packs are just id lists now, no files bundled
        auto res = gmpkg::savePack(pack);
        if (res.isErr()) {
            Notification::create("couldnt save that pack", NotificationIcon::Error)->show();
            return;
        }
        Notification::create(
            fmt::format("saved \"{}\" ({} mods)", pack.name, pack.mods.size()),
            NotificationIcon::Success
        )->show();
        auto cb = std::move(m_onDone);
        this->onClose(nullptr);
        if (cb) cb();
    }

public:
    static CreateModpackPopup* create(geode::Function<void()> onDone) {
        auto ret = new CreateModpackPopup();
        if (ret->init(std::move(onDone))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class ModpacksPopup : public geode::Popup {
    // main list, shows saved packs
protected:
    static constexpr size_t PAGE_SIZE = 4;
    CCMenu* m_rowMenu = nullptr;
    CCLayer* m_rowLayer = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    std::vector<gmpkg::PackInfo> m_packs;
    size_t m_page = 0;
    // blocks the button spam that reopens the dialog after cancel
    bool m_dialogOpen = false;

    bool init() {
        if (!geode::Popup::init(340.f, 244.f)) return false;
        this->setTitle("Geode Packs");

        auto size = m_mainLayer->getContentSize();

        auto hint = CCLabelBMFont::create("Save, share and install modpacks:", "bigFont.fnt");
        hint->setScale(0.32f);
        hint->setColor(ccc3(255, 220, 130));
        hint->setPosition({size.width / 2, size.height - 36});
        m_mainLayer->addChild(hint);

        auto panel = CCScale9Sprite::create("GJ_square02.png");
        panel->setContentSize({300, 112});
        panel->setPosition({size.width / 2, 56 + 56});
        m_mainLayer->addChild(panel);

        m_rowLayer = CCLayer::create();
        m_mainLayer->addChild(m_rowLayer);
        m_rowMenu = CCMenu::create();
        m_rowMenu->setPosition({0, 0});
        m_mainLayer->addChild(m_rowMenu);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto createBtn = makeButton("Create Pack", this, menu_selector(ModpacksPopup::onCreate), 0.45f);
        createBtn->setPosition({72, 28});
        menu->addChild(createBtn);

        auto loadBtn = makeButton("Load Pack", this, menu_selector(ModpacksPopup::onImportFile), 0.45f);
        loadBtn->setPosition({170, 28});
        menu->addChild(loadBtn);

        CCMenuItemSpriteExtra* folderBtn = nullptr;
        if (auto folderSpr = CCSprite::createWithSpriteFrameName("gj_folderBtn_001.png")) {
            folderSpr->setScale(0.62f);
            folderBtn = CCMenuItemSpriteExtra::create(
                folderSpr, this, menu_selector(ModpacksPopup::onFolder)
            );
        } else {
            folderBtn = makeButton("Folder", this, menu_selector(ModpacksPopup::onFolder), 0.45f);
        }
        folderBtn->setPosition({size.width - 38, 28});
        menu->addChild(folderBtn);

        m_pageLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_pageLabel->setScale(0.3f);
        m_pageLabel->setPosition({size.width / 2, 48});
        m_mainLayer->addChild(m_pageLabel);

        auto prev = makeArrowButton(false, this, menu_selector(ModpacksPopup::onPrevPage));
        prev->setPosition({10, 108});
        menu->addChild(prev);

        auto next = makeArrowButton(true, this, menu_selector(ModpacksPopup::onNextPage));
        next->setPosition({size.width - 10, 108});
        menu->addChild(next);

        this->refresh();
        return true;
    }

    size_t pageCount() const {
        return std::max<size_t>(1, (m_packs.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    void refresh() {
        m_packs = gmpkg::listPacks();
        if (m_page >= this->pageCount()) m_page = this->pageCount() - 1;

        m_rowLayer->removeAllChildrenWithCleanup(true);
        m_rowMenu->removeAllChildrenWithCleanup(true);

        float px = 20, py = 56, pw = 300, ph = 112;
        float rowH = 27.f;

        if (m_packs.empty()) {
            auto empty = CCLabelBMFont::create("No modpacks yet!", "bigFont.fnt");
            empty->setScale(0.4f);
            empty->setPosition({px + pw / 2, py + ph / 2 + 8});
            m_rowLayer->addChild(empty);
            auto empty2 = CCLabelBMFont::create(
                "hit Create Pack below to make one.", "bigFont.fnt"
            );
            empty2->setScale(0.32f);
            empty2->setPosition({px + pw / 2, py + ph / 2 - 12});
            m_rowLayer->addChild(empty2);
        } else {
            size_t start = m_page * PAGE_SIZE;
            size_t end = std::min(start + PAGE_SIZE, m_packs.size());
            for (size_t i = start; i < end; i++) {
                float y = py + ph - 15 - (i - start) * rowH;

                std::string name = m_packs[i].name;
                if (name.size() > 18) name = name.substr(0, 17) + ".";
                auto label = CCLabelBMFont::create(
                    fmt::format("{} ({} mods)", name, m_packs[i].modCount).c_str(), "bigFont.fnt"
                );
                label->setScale(0.34f);
                label->setAnchorPoint({0, 0.5f});
                label->setPosition({px + 8, y});
                m_rowLayer->addChild(label);

                CCMenuItemSpriteExtra* dl = nullptr;
                if (auto dlSpr = CCSprite::createWithSpriteFrameName("GJ_downloadBtn_001.png")) {
                    dlSpr->setScale(0.42f);
                    dl = CCMenuItemSpriteExtra::create(
                        dlSpr, this, menu_selector(ModpacksPopup::onExportRow)
                    );
                } else {
                    dl = makeButton("DL", this, menu_selector(ModpacksPopup::onExportRow), 0.42f);
                }
                dl->setPosition({px + pw - 88, y});
                dl->setTag((int)i);
                m_rowMenu->addChild(dl);

                auto load = makeButton("Load", this, menu_selector(ModpacksPopup::onLoadRow), 0.42f);
                load->setPosition({px + pw - 52, y});
                load->setTag((int)i);
                m_rowMenu->addChild(load);

                CCMenuItemSpriteExtra* del = nullptr;
                if (auto delSpr = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png")) {
                    delSpr->setScale(0.36f);
                    del = CCMenuItemSpriteExtra::create(
                        delSpr, this, menu_selector(ModpacksPopup::onDeleteRow)
                    );
                } else {
                    del = makeButton("X", this, menu_selector(ModpacksPopup::onDeleteRow), 0.42f);
                }
                del->setPosition({px + pw - 16, y});
                del->setTag((int)i);
                m_rowMenu->addChild(del);
            }
        }
        m_pageLabel->setString(
            fmt::format("{}/{} - {} packs", m_page + 1, this->pageCount(),
                m_packs.size()).c_str()
        );
    }

    void onPrevPage(CCObject*) {
        if (m_page > 0) {
            m_page--;
            this->refresh();
        }
    }

    void onNextPage(CCObject*) {
        if (m_page + 1 < this->pageCount()) {
            m_page++;
            this->refresh();
        }
    }

    void onCreate(CCObject*) {
        CreateModpackPopup::create([this] { this->refresh(); })->show();
    }

    void onLoadRow(CCObject* sender) {
        int i = sender->getTag();
        if (i < 0 || (size_t)i >= m_packs.size()) return;
        this->applyPack(m_packs[(size_t)i].path);
    }

    void onDeleteRow(CCObject* sender) {
        int i = sender->getTag();
        if (i < 0 || (size_t)i >= m_packs.size()) return;
        // no confirm for now, just deletes
        std::error_code ec;
        std::filesystem::remove(m_packs[(size_t)i].path, ec);
        this->refresh();
    }

    void onExportRow(CCObject* sender) {
        if (m_dialogOpen) return;
        int i = sender->getTag();
        if (i < 0 || (size_t)i >= m_packs.size()) return;
        m_dialogOpen = true;
        this->exportPack(m_packs[(size_t)i].path, m_packs[(size_t)i].name);
        this->scheduleOnce(schedule_selector(ModpacksPopup::unlockDialog), 0.3f);
    }

    void unlockDialog(float) {
        m_dialogOpen = false;
    }

    void exportPack(std::filesystem::path src, std::string const& name) {
#ifdef GEODE_IS_WINDOWS
        // windows save dialog
        std::string defName = gmpkg::sanitizeName(name);
        for (auto& c : defName) if (c == ' ') c = '_';
        defName += ".gmpkg";
        char buf[MAX_PATH] = {};
        strncpy(buf, defName.c_str(), MAX_PATH - 1);
        OPENFILENAMEA save = {};
        save.lStructSize = sizeof(save);
        save.hwndOwner = nullptr;
        save.lpstrFile = buf;
        save.nMaxFile = MAX_PATH;
        save.lpstrFilter = "Geode Modpack (*.gmpkg)\0*.gmpkg\0All Files (*.*)\0*.*\0";
        save.nFilterIndex = 1;
        save.lpstrDefExt = "gmpkg";
        save.lpstrTitle = "Save modpack file to share";
        save.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameA(&save)) {
            // closed it without picking, not an error
            if (CommDlgExtendedError() == 0) {
                Notification::create("download cancelled", NotificationIcon::Info)->show();
            }
            return;
        }
        std::error_code ec;
        std::filesystem::copy_file(src, std::filesystem::path(buf),
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            Notification::create("couldnt save there, try a different folder", NotificationIcon::Error)->show();
            return;
        }
        Notification::create(
            fmt::format("saved \"{}\" - send it to your friends", name),
            NotificationIcon::Success
        )->show();
#else
        Notification::create("export only works on windows rn", NotificationIcon::Info)->show();
#endif
    }

    void onImportFile(CCObject*) {
        if (m_dialogOpen) return;
#ifdef GEODE_IS_WINDOWS
        m_dialogOpen = true;
        auto picked = pickGmpkgFile();
        this->scheduleOnce(schedule_selector(ModpacksPopup::unlockDialog), 0.3f);
        if (!picked) {
            if (CommDlgExtendedError() == 0) {
                Notification::create("import cancelled", NotificationIcon::Info)->show();
            }
            return;
        }
        auto res = gmpkg::loadPack(*picked);
        if (res.isErr()) {
            Notification::create("that file isnt a valid .gmpkg", NotificationIcon::Error)->show();
            return;
        }
        auto dest = gmpkg::pathFor(res.unwrap().name);
        std::error_code ec;
        std::filesystem::copy_file(*picked, dest,
            std::filesystem::copy_options::overwrite_existing, ec);
        this->refresh();
        this->applyPack(*picked);
#else
        Notification::create("import only works on windows rn", NotificationIcon::Info)->show();
#endif
    }

    void onFolder(CCObject*) {
        // opens the packs folder in explorer
#ifdef GEODE_IS_WINDOWS
        auto dir = gmpkg::packsDir().string();
        ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        Notification::create(gmpkg::packsDir().string(), NotificationIcon::Info)->show();
#endif
    }

    void applyPack(std::filesystem::path path) {
        openPackFile(std::move(path));
    }

public:
    static ModpacksPopup* create() {
        auto ret = new ModpacksPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void openModpacksPopup() {
    ModpacksPopup::create()->show();
}
