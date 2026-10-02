// geode packs
// adds a button to the main menu for saving/loading modpacks

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include "ModpacksUI.hpp"

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <shlobj.h>
#endif

using namespace geode::prelude;

// holds the pack path when u double click a .gmpkg file while gd is closed
static std::string pendingPack;

#ifdef GEODE_IS_WINDOWS

// windows "open with" stuff
// makes double-clicking a .gmpkg file launch gd with --geode:modpack
static void setupFileOpen() {
    std::string exe = (geode::dirs::getGameDir() / "GeometryDash.exe").string();

    for (auto& c : exe) if (c == '/') c = '\\';
    std::string cmd = "\"" + exe + "\" --geode:modpack \"%1\"";

    // writes a string to registry
    auto setVal = [](const char* key, const char* value, const char* data) {
        HKEY h = nullptr;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, nullptr, 0,
                KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
            RegSetValueExA(h, value, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(data), (DWORD)(strlen(data) + 1));
            RegCloseKey(h);
        }
    };

    setVal("Software\\Classes\\.gmpkg", nullptr, "GDModpackFile");
    setVal("Software\\Classes\\GDModpackFile", nullptr, "GD Modpack");
    setVal("Software\\Classes\\GDModpackFile\\DefaultIcon", nullptr, (exe + ",0").c_str());
    setVal("Software\\Classes\\GDModpackFile\\shell\\open\\command", nullptr, cmd.c_str());

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
#endif

$execute {
#ifdef GEODE_IS_WINDOWS
    setupFileOpen();
#endif
    if (auto arg = Loader::get()->getLaunchArgument("modpack")) {
        pendingPack = arg.value();

        // trim quotes bc windows adds them, istg this broke paths for an hour
        if (pendingPack.size() >= 2 && pendingPack.front() == '"' &&
            pendingPack.back() == '"') {
            pendingPack = pendingPack.substr(1, pendingPack.size() - 2);
        }
        log::info("opening modpack from file: {}", pendingPack);
    }
}

class $modify(GeodePacksLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init())
            return false;

        // tries custom icon first, falls back to profile button
        CCSprite* btnSprite = CCSprite::create("savepackbuttonmenu.png"_spr);
        if (!btnSprite)
            btnSprite = CCSprite::createWithSpriteFrameName("GJ_profileButton_001.png");
        if (!btnSprite)
            return true;

        auto button = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(GeodePacksLayer::onGeodePacksButton)
        );
        if (!button)
            return true;

        button->setID("geodepacks-button"_spr);

        // geode's top-right-menu from node-ids
        if (auto topRightMenu = this->getChildByID("top-right-menu")) {
            if (auto menu = typeinfo_cast<CCMenu*>(topRightMenu)) {
                menu->addChild(button);
                menu->updateLayout();
            }
        } else {
            // fallback if node-ids changes, just stick it top right
            auto winSize = CCDirector::get()->getWinSize();
            auto menu = CCMenu::create();
            menu->setPosition(winSize.width - 30.f, winSize.height - 30.f);
            menu->addChild(button);
            menu->setID("geodepacks-button-menu"_spr);
            this->addChild(menu);
        }

        // wait a bit so the menu exists before popping up
        if (!pendingPack.empty()) {
            this->scheduleOnce(
                schedule_selector(GeodePacksLayer::openPendingPack), 0.5f
            );
        }

        return true;
    }

    void openPendingPack(float) {
        if (pendingPack.empty()) return;
        std::filesystem::path p = pendingPack;
        pendingPack.clear();
        openPackFile(std::move(p));
    }

    void onGeodePacksButton(CCObject* sender) {
        openModpacksPopup();
    }
};
