#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <unordered_set>
#include <algorithm>
#include <vector>

using namespace geode::prelude;

static const std::string SERVER = "https://www.gdrequests.org";
static const std::string UPDATE_URL = "https://github.com/xCompassionate/gd-requests-mod/releases/latest/download/icontradict.gd-requests.geode";

static std::unordered_set<std::string> g_queueLevelIds;
static std::unordered_map<std::string, std::string> g_queueLevelNames; // levelId -> requester
static bool g_fetchInProgress = false;
static std::string g_currentQueueLevelId; // tracks which queued level is being played
static size_t g_lastQueueCount = 0;
static bool g_autoOpenFirstLevel = false;

static bool g_blackScreenActive = false;
static bool g_updateChecked = false; // only check once per GD launch
static std::string g_pendingQueueLevelId; // set when a queued level is entered, persists across LevelInfoLayer re-entry

struct QueueEntry {
    std::string name;
    std::string levelId;
    std::string youtubeUrl;
    std::string levelName;
    std::string gdDifficulty;
    std::string note; // optional submission note from the requester
    bool online = false;
};

// Forward Declarations
void fetchAndShowQueue();

// API Helpers

void sendQueueAction(const std::string& endpoint, const std::string& levelId) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + endpoint;
    std::string body = matjson::makeObject({
        {"token", token},
        {"level_id", levelId}
    }).dump();
    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

void sendQueueRemoveYoutube(const std::string& youtubeUrl) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + "/api/queue/remove";
    std::string body = matjson::makeObject({
        {"token", token},
        {"youtube_url", youtubeUrl}
    }).dump();
    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

void sendTimeoutUser(const std::string& username) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + "/api/queue/timeout";
    std::string body = matjson::makeObject({
        {"token", token},
        {"username", username}
    }).dump();
    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [username](web::WebResponse res) {
            if (res.ok()) {
                auto j = res.json();
                int mins = 60;
                if (j && (*j).contains("duration_mins"))
                    mins = (*j)["duration_mins"].asInt().unwrapOr(60);
                Notification::create(
                    fmt::format("{} timed out for {} min", username, mins),
                    NotificationIcon::Warning, 3.f
                )->show();
            }
        }
    );
}

void sendQueueRemoveAll() {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + "/api/queue/remove-all";
    std::string body = matjson::makeObject({
        {"token", token}
    }).dump();
    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

// UI Helpers

static constexpr int LOADING_CIRCLE_TAG = 9880;
static constexpr int CONTENT_ROOT_TAG   = 9881;
static constexpr int QUEUE_BADGE_TAG    = 9882;
static constexpr int BLACK_OVERLAY_TAG  = 9883;
static constexpr int BLACK_BTN_TAG      = 9884;

// Black Screen Helpers

static CCPoint blackBtnPosition(CCSize ws, float btnSize) {
    auto pos = Mod::get()->getSettingValue<std::string>("black-btn-position");
    float margin = btnSize / 2.f + 4.f;
    if (pos == "Top Left")      return {margin, ws.height - margin};
    if (pos == "Top Right")     return {ws.width - margin, ws.height - margin};
    if (pos == "Center Left")   return {margin, ws.height / 2.f};
    if (pos == "Center Right")  return {ws.width - margin, ws.height / 2.f};
    if (pos == "Bottom Right")  return {ws.width - margin, margin};
    return {margin, margin}; // Bottom Left default
}

static void applyBlackScreen(bool active) {
    auto pl = PlayLayer::get();
    if (!pl) return;

    if (auto overlay = pl->getChildByTag(BLACK_OVERLAY_TAG)) {
        overlay->setVisible(active);
    }

    // Hide/show ShaderLayer so its effects don't bleed through
    auto* base = static_cast<GJBaseGameLayer*>(pl);
    if (base->m_shaderLayer) {
        base->m_shaderLayer->setVisible(!active);
    }
}

static void toggleBlackScreen() {
    g_blackScreenActive = !g_blackScreenActive;
    applyBlackScreen(g_blackScreenActive);
}

// Silent level existence check — fires after queue is populated
static void silentlyPruneNonexistentLevels(std::vector<std::string> levelIds) {
    if (levelIds.empty()) return;
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    for (auto& lvlId : levelIds) {
        std::string searchUrl = "https://www.boomlings.com/database/getGJLevels21.php";
        std::string body = "str=" + lvlId + "&type=0&secret=Wmfd2893gb7";
        geode::async::spawn(
            [searchUrl, body]() -> web::WebFuture {
                return web::WebRequest()
                    .header("Content-Type", "application/x-www-form-urlencoded")
                    .body(std::vector<uint8_t>(body.begin(), body.end()))
                    .post(searchUrl);
            },
            [lvlId, token](web::WebResponse res) {
                if (!res.ok()) return;
                std::string text = res.string().unwrapOr("");
                // GD returns "-1" when no levels found
                if (text.empty() || text == "-1") {
                    std::string removeUrl = SERVER + "/api/queue/remove";
                    std::string removeBody = matjson::makeObject({
                        {"token", token},
                        {"level_id", lvlId}
                    }).dump();
                    geode::async::spawn(
                        [removeUrl, removeBody]() -> web::WebFuture {
                            return web::WebRequest()
                                .header("Content-Type", "application/json")
                                .body(std::vector<uint8_t>(removeBody.begin(), removeBody.end()))
                                .post(removeUrl);
                        },
                        [](web::WebResponse) {}
                    );
                    g_queueLevelIds.erase(lvlId);
                    g_queueLevelNames.erase(lvlId);
                }
            }
        );
    }
}

// One-time update check against GitHub releases
static void checkForUpdate() {
    if (g_updateChecked) return;
    g_updateChecked = true;
    geode::async::spawn(
        []() -> web::WebFuture {
            return web::WebRequest()
                .header("User-Agent", "gd-requests-mod")
                .get("https://api.github.com/repos/xCompassionate/gd-requests-mod/releases/latest");
        },
        [](web::WebResponse res) {
            if (!res.ok()) return;
            auto jsonRes = res.json();
            if (!jsonRes) return;
            std::string tagName = (*jsonRes)["tag_name"].asString().unwrapOr("");
            if (tagName.empty()) return;
            // Strip leading 'v' for comparison
            std::string tag = tagName;
            if (!tag.empty() && tag[0] == 'v') tag = tag.substr(1);
            std::string current = Mod::get()->getVersion().toNonVString();
            if (tag == current) return;
            geode::createQuickPopup(
                "GD Requests Update",
                fmt::format("A new version (<cy>{}</c>) is available!\nYou have <cr>{}</c>. Update now?", tagName, current),
                "Later", "Update",
                [](FLAlertLayer*, bool btn2) {
                    if (!btn2) return;
                    Notification::create("Downloading update...", NotificationIcon::Info)->show();
                    geode::async::spawn(
                        []() -> web::WebFuture {
                            return web::WebRequest().get(UPDATE_URL);
                        },
                        [](web::WebResponse dlRes) {
                            if (dlRes.ok()) {
                                auto path = Mod::get()->getPackagePath();
                                auto data = dlRes.data();
                                std::ofstream file(path.string(), std::ios::binary);
                                file.write(reinterpret_cast<const char*>(data.data()), data.size());
                                file.close();
                                Notification::create("Updated! Restart to apply!", NotificationIcon::Success)->show();
                            } else {
                                Notification::create("Update failed!", NotificationIcon::Error)->show();
                            }
                        }
                    );
                }
            );
        }
    );
}

// Popup Classes

class QueuePopup : public geode::Popup, public FLAlertLayerProtocol {
    std::vector<QueueEntry> m_entries;
    int m_page = 0;
    bool m_loading = false;
    int m_pendingEntryIdx = -1;
    static constexpr int PER_PAGE = 5;

protected:
    bool init(std::vector<QueueEntry> entries, bool loading) {
        if (!Popup::init(370.f, 295.f)) return false;
        m_entries = std::move(entries);
        m_loading = loading;
        this->setTitle("Request Queue");

        auto sz = m_mainLayer->getContentSize();

        auto popupOverlay = CCDrawNode::create();
        {
            CCPoint v[] = {{0,0},{sz.width,0},{sz.width,sz.height},{0,sz.height}};
            popupOverlay->drawPolygon(v, 4, {0.0f,0.0f,0.0f,0.45f}, 0.f, {0,0,0,0});
        }
        m_mainLayer->addChild(popupOverlay, -2);

        if (m_loading) {
            auto spinnerRoot = CCLayer::create();
            spinnerRoot->setTag(LOADING_CIRCLE_TAG);
            spinnerRoot->setContentSize(m_mainLayer->getContentSize());
            spinnerRoot->setPosition(m_mainLayer->getContentSize() / 2);
            m_mainLayer->addChild(spinnerRoot, 10);
            auto circle = LoadingCircle::create();
            circle->setParentLayer(spinnerRoot);
            circle->show();
            return true;
        }

        if (m_entries.empty()) { buildEmpty(); return true; }
        buildPage();
        return true;
    }

    void buildEmpty() {
        if (auto n = m_mainLayer->getChildByTag(CONTENT_ROOT_TAG)) n->removeFromParent();
        auto sz = m_mainLayer->getContentSize();
        auto root = CCNode::create();
        root->setTag(CONTENT_ROOT_TAG);
        root->setContentSize(sz);
        auto lbl = CCLabelBMFont::create("Your queue is empty!", "bigFont.fnt", 280.f);
        lbl->setScale(0.5f);
        lbl->setPosition(sz / 2);
        root->addChild(lbl);
        m_mainLayer->addChild(root);
    }

    void buildPage() {
        if (auto n = m_mainLayer->getChildByTag(CONTENT_ROOT_TAG)) n->removeFromParent();
        auto sz = m_mainLayer->getContentSize();
        int total = (int)m_entries.size();
        int totalPages = (total + PER_PAGE - 1) / PER_PAGE;
        if (m_page >= totalPages) m_page = totalPages - 1;
        if (m_page < 0) m_page = 0;
        int startIdx = m_page * PER_PAGE;
        int endIdx   = std::min(startIdx + PER_PAGE, total);

        auto root = CCNode::create();
        root->setTag(CONTENT_ROOT_TAG);
        root->setContentSize(sz);
        m_mainLayer->addChild(root);

        auto countLbl = CCLabelBMFont::create(fmt::format("{} pending request{}", total, total == 1 ? "" : "s").c_str(), "goldFont.fnt", 260.f);
        countLbl->setScale(0.35f);
        countLbl->setPosition({sz.width / 2.f, sz.height - 38.f});
        root->addChild(countLbl);

        auto removeAllMenu = CCMenu::create();
        removeAllMenu->setPosition({0.f, 0.f});
        auto removeAllLbl = CCLabelBMFont::create("Remove All", "bigFont.fnt", 200.f);
        removeAllLbl->setScale(0.28f);
        removeAllLbl->setColor({255, 70, 70});
        auto removeAllBtn = CCMenuItemSpriteExtra::create(removeAllLbl, this, menu_selector(QueuePopup::onRemoveAll));
        removeAllBtn->setPosition({sz.width - 50.f, sz.height - 38.f});
        removeAllMenu->addChild(removeAllBtn);
        root->addChild(removeAllMenu);

        const float rowH     = 43.f;
        const float fullW    = sz.width - 24.f;
        const float rowLeft  = 12.f;
        const float btnGap   = 3.f;
        const bool  ytEnabled = Mod::get()->getSettingValue<bool>("open-youtube");
        const float topY     = sz.height - 52.f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        root->addChild(menu);

        for (int idx = startIdx; idx < endIdx; idx++) {
            auto& e = m_entries[idx];
            int localRow = idx - startIdx;
            const bool  hasYT  = !e.levelId.empty() && !e.youtubeUrl.empty() && ytEnabled;
            const float stackW = 62.f;
            const float ytW    = 26.f;
            const float actW   = stackW + (hasYT ? ytW + btnGap : 0.f);
            const float mainW  = fullW - actW - 4.f;
            const float inner  = rowH - 4.f;
            const float rowCY  = topY - localRow * rowH - rowH / 2.f;

            auto bg = CCDrawNode::create();
            {
                CCPoint v[] = {{0,0},{fullW,0},{fullW,inner},{0,inner}};
                bg->drawPolygon(v, 4, {0.0f,0.0f,0.0f,0.35f}, 0.f, {0,0,0,0});
            }
            bg->setAnchorPoint({0.f, 0.5f});
            bg->setPosition({rowLeft, rowCY - inner / 2.f});
            root->addChild(bg, -1);

            auto numLbl = CCLabelBMFont::create(std::to_string(idx + 1).c_str(), "bigFont.fnt", 30.f);
            numLbl->setScale(0.38f);
            numLbl->setPosition({14.f, inner / 2.f});

            std::string topText;
            ccColor3B topColor = {255, 255, 255};
            if (e.levelId.empty()) {
                topText = "YouTube request";
                topColor = {255, 70, 70};
            } else if (!e.levelName.empty()) {
                topText = e.levelName;
            } else {
                topText = "ID: " + e.levelId;
            }
            auto topLbl = CCLabelBMFont::create(topText.c_str(), "bigFont.fnt", 200.f);
            topLbl->setScale(0.40f);
            topLbl->setColor(topColor);
            topLbl->setAnchorPoint({0.f, 0.5f});
            topLbl->setPosition({28.f, inner / 2.f + 8.f});

            std::string bottomText = e.name;
            auto bottomLbl = CCLabelBMFont::create(bottomText.c_str(), "bigFont.fnt", 200.f);
            bottomLbl->setScale(0.30f);
            bottomLbl->setColor({200, 200, 200});
            bottomLbl->setAnchorPoint({0.f, 0.5f});
            bottomLbl->setPosition({28.f, inner / 2.f - 9.f});

            auto rowNode = CCNode::create();
            rowNode->setContentSize({mainW, inner});
            rowNode->addChild(numLbl);
            rowNode->addChild(topLbl);
            rowNode->addChild(bottomLbl);

            if (!e.levelId.empty()) {
                std::string frame = "difficulty_00_btn_001.png";
                if (e.gdDifficulty == "auto") frame = "difficulty_auto_btn_001.png";
                else if (e.gdDifficulty == "easy") frame = "difficulty_01_btn_001.png";
                else if (e.gdDifficulty == "normal") frame = "difficulty_02_btn_001.png";
                else if (e.gdDifficulty == "hard") frame = "difficulty_03_btn_001.png";
                else if (e.gdDifficulty == "harder") frame = "difficulty_04_btn_001.png";
                else if (e.gdDifficulty == "insane") frame = "difficulty_05_btn_001.png";
                else if (e.gdDifficulty == "easy_demon") frame = "difficulty_07_btn_001.png";
                else if (e.gdDifficulty == "medium_demon") frame = "difficulty_08_btn_001.png";
                else if (e.gdDifficulty == "hard_demon") frame = "difficulty_06_btn_001.png";
                else if (e.gdDifficulty == "insane_demon") frame = "difficulty_09_btn_001.png";
                else if (e.gdDifficulty == "extreme_demon") frame = "difficulty_10_btn_001.png";

                auto diffSpr = CCSprite::createWithSpriteFrameName(frame.c_str());
                if (diffSpr) {
                    diffSpr->setScale(0.45f);
                    diffSpr->setAnchorPoint({0.5f, 0.5f});
                    diffSpr->setPosition({14.f + (numLbl->getContentSize().width * 0.38f / 2.f), inner / 2.f}); // Behind number queue
                    diffSpr->setZOrder(0);
                    numLbl->setZOrder(1);
                    rowNode->addChild(diffSpr);
                }
            }

            auto mainBtn = CCMenuItemSpriteExtra::create(rowNode, this, menu_selector(QueuePopup::onEntry));
            mainBtn->setTag(idx);
            mainBtn->setAnchorPoint({0.f, 0.5f});
            mainBtn->setPosition({rowLeft, rowCY});
            menu->addChild(mainBtn);

            const float actX = rowLeft + mainW + 4.f;
            if (e.levelId.empty()) {
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.26f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.28f});
                menu->addChild(removeBtn);

                auto timeoutLbl = CCLabelBMFont::create("Timeout", "bigFont.fnt", stackW * 3.f);
                timeoutLbl->setScale(0.26f);
                timeoutLbl->setColor({255, 200, 50});
                auto timeoutBtn = CCMenuItemSpriteExtra::create(timeoutLbl, this, menu_selector(QueuePopup::onTimeout));
                timeoutBtn->setTag(idx);
                timeoutBtn->setPosition({actX + stackW * 0.5f, rowCY});
                menu->addChild(timeoutBtn);

                auto watchLbl = CCLabelBMFont::create("Watch", "bigFont.fnt", stackW * 3.f);
                watchLbl->setScale(0.26f);
                watchLbl->setColor({255, 70, 70});
                auto watchBtn = CCMenuItemSpriteExtra::create(watchLbl, this, menu_selector(QueuePopup::onWatch));
                watchBtn->setTag(idx);
                watchBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.28f});
                menu->addChild(watchBtn);
            } else {
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.26f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.28f});
                menu->addChild(removeBtn);

                auto timeoutLbl = CCLabelBMFont::create("Timeout", "bigFont.fnt", stackW * 3.f);
                timeoutLbl->setScale(0.26f);
                timeoutLbl->setColor({255, 200, 50});
                auto timeoutBtn = CCMenuItemSpriteExtra::create(timeoutLbl, this, menu_selector(QueuePopup::onTimeout));
                timeoutBtn->setTag(idx);
                timeoutBtn->setPosition({actX + stackW * 0.5f, rowCY});
                menu->addChild(timeoutBtn);

                auto banLbl = CCLabelBMFont::create("Ban Level", "bigFont.fnt", stackW * 3.f);
                banLbl->setScale(0.26f);
                banLbl->setColor({220, 30, 30});
                auto banBtn = CCMenuItemSpriteExtra::create(banLbl, this, menu_selector(QueuePopup::onBlacklist));
                banBtn->setTag(idx);
                banBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.28f});
                menu->addChild(banBtn);

                if (hasYT) {
                    auto ytSpr = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
                    ytSpr->setScale(0.6f);
                    auto ytBtn = CCMenuItemSpriteExtra::create(ytSpr, this, menu_selector(QueuePopup::onWatch));
                    ytBtn->setTag(idx);
                    ytBtn->setPosition({actX + stackW + btnGap + ytW * 0.5f, rowCY});
                    menu->addChild(ytBtn);
                }
            }
        }

        if (totalPages > 1) {
            auto navMenu = CCMenu::create();
            navMenu->setPosition({0.f, 0.f});
            root->addChild(navMenu);
            float navY = 18.f;
            auto pageLbl = CCLabelBMFont::create(fmt::format("Page {} of {}", m_page + 1, totalPages).c_str(), "goldFont.fnt", 200.f);
            pageLbl->setScale(0.3f);
            pageLbl->setPosition({sz.width / 2.f, navY});
            root->addChild(pageLbl);
            if (m_page > 0) {
                auto prevLbl = CCLabelBMFont::create("< Prev", "bigFont.fnt", 100.f);
                prevLbl->setScale(0.35f);
                auto prevBtn = CCMenuItemSpriteExtra::create(prevLbl, this, menu_selector(QueuePopup::onPrevPage));
                prevBtn->setPosition({sz.width * 0.2f, navY});
                navMenu->addChild(prevBtn);
            }
            if (m_page < totalPages - 1) {
                auto nextLbl = CCLabelBMFont::create("Next >", "bigFont.fnt", 100.f);
                nextLbl->setScale(0.35f);
                auto nextBtn = CCMenuItemSpriteExtra::create(nextLbl, this, menu_selector(QueuePopup::onNextPage));
                nextBtn->setPosition({sz.width * 0.8f, navY});
                navMenu->addChild(nextBtn);
            }
        }
    }

    void onPrevPage(CCObject*) { if (m_page > 0) { m_page--; buildPage(); } }
    void onNextPage(CCObject*) { int totalPages = ((int)m_entries.size() + PER_PAGE - 1) / PER_PAGE; if (m_page < totalPages - 1) { m_page++; buildPage(); } }

    void onEntry(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.levelId.empty()) return;

        if (!e.note.empty()) {
            std::string note = e.note;
            std::string levelId = e.levelId;
            FLAlertLayer::create(
                this, "Submission Note",
                note,
                "OK", nullptr, 300.f
            )->show();
            // navigation happens in FLAlertLayerProtocol callback below
            m_pendingEntryIdx = idx;
            return;
        }

        navigateToEntry(idx);
    }

    void FLAlert_Clicked(FLAlertLayer*, bool) {
        if (m_pendingEntryIdx >= 0) {
            int idx = m_pendingEntryIdx;
            m_pendingEntryIdx = -1;
            navigateToEntry(idx);
        }
    }

    void navigateToEntry(int idx) {
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        onClose(nullptr);
        auto searchObj = GJSearchObject::create(SearchType::Search, e.levelId);
        g_autoOpenFirstLevel = true;
        auto scene = LevelBrowserLayer::scene(searchObj);
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, scene));
    }

    void onRemove(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        g_queueLevelIds.erase(e.levelId);
        if (!e.levelId.empty()) sendQueueAction("/api/queue/remove", e.levelId);
        else sendQueueRemoveYoutube(e.youtubeUrl);
        m_entries.erase(m_entries.begin() + idx);
        if (m_entries.empty()) buildEmpty();
        else buildPage();
    }

    void onTimeout(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        sendTimeoutUser(m_entries[idx].name);
    }

    void onBlacklist(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto lvlId = m_entries[idx].levelId;
        g_queueLevelIds.erase(lvlId);
        sendQueueAction("/api/queue/blacklist", lvlId);
        m_entries.erase(m_entries.begin() + idx);
        if (m_entries.empty()) buildEmpty();
        else buildPage();
    }

    void onRemoveAll(CCObject*) {
        geode::createQuickPopup("Remove All", "Are you sure you want to <cr>remove all</c> pending requests?", "Cancel", "Remove All", [this](FLAlertLayer*, bool btn2) {
            if (!btn2) return;
            sendQueueRemoveAll();
            g_queueLevelIds.clear();
            m_entries.clear();
            buildEmpty();
        });
    }

    void onWatch(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.youtubeUrl.empty()) return;
        std::string url = e.youtubeUrl;
        if (url.rfind("http", 0) != 0) url = "https://" + url;
        auto action = Mod::get()->getSettingValue<std::string>("youtube-action");
        if (action == "Copy to Clipboard") {
            geode::utils::clipboard::write(url);
            Notification::create("Copied to clipboard", NotificationIcon::Success)->show();
        } else CCApplication::sharedApplication()->openURL(url.c_str());
    }

    ~QueuePopup() { g_fetchInProgress = false; }

public:
    static QueuePopup* createLoading() {
        auto p = new QueuePopup();
        if (p->init(std::vector<QueueEntry>{}, true)) { p->autorelease(); return p; }
        CC_SAFE_DELETE(p);
        return nullptr;
    }
    static QueuePopup* create(std::vector<QueueEntry> entries) {
        auto p = new QueuePopup();
        if (p->init(std::move(entries), false)) { p->autorelease(); return p; }
        CC_SAFE_DELETE(p);
        return nullptr;
    }
    void populate(std::vector<QueueEntry> entries) {
        m_loading = false;
        m_entries = std::move(entries);
        if (auto spinnerRoot = m_mainLayer->getChildByTag(LOADING_CIRCLE_TAG)) spinnerRoot->removeFromParent();
        if (m_entries.empty()) buildEmpty();
        else buildPage();
    }
};

struct $modify(GDReqLevelBrowser, LevelBrowserLayer) {
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key, int type) {
        LevelBrowserLayer::loadLevelsFinished(levels, key, type);

        if (!g_autoOpenFirstLevel) return;
        if (!levels || levels->count() == 0) {
            g_autoOpenFirstLevel = false;
            return;
        }

        g_autoOpenFirstLevel = false;

        auto level = static_cast<GJGameLevel*>(levels->objectAtIndex(0));
        if (!level) return;

        auto infoScene = LevelInfoLayer::scene(level, false);
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, infoScene));
    }
};

// Custom Reinstall Setting

class ReinstallSettingV3 : public SettingV3 {
public:
    static Result<std::shared_ptr<SettingV3>> parse(std::string const& key, std::string const& modID, matjson::Value const& json) {
        auto res = std::make_shared<ReinstallSettingV3>();
        auto root = checkJson(json, "ReinstallSettingV3");
        res->init(key, modID, root);
        res->parseNameAndDescription(root);
        res->parseEnableIf(root);
        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(res));
    }

    bool load(matjson::Value const& json) override { return true; }
    bool save(matjson::Value& json) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}
    SettingNodeV3* createNode(float width) override;
};

class ReinstallSettingNodeV3 : public SettingNodeV3 {
protected:
    ButtonSprite* m_buttonSprite;
    CCMenuItemSpriteExtra* m_button;

    bool init(std::shared_ptr<ReinstallSettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;

        m_buttonSprite = ButtonSprite::create("Reinstall", "goldFont.fnt", "GJ_button_01.png", .8f);
        m_buttonSprite->setScale(.5f);
        m_button = CCMenuItemSpriteExtra::create(
            m_buttonSprite, this, menu_selector(ReinstallSettingNodeV3::onReinstall)
        );
        this->getButtonMenu()->addChildAtPosition(m_button, Anchor::Center);
        this->getButtonMenu()->setContentWidth(60);
        this->getButtonMenu()->updateLayout();

        this->updateState(nullptr);
        return true;
    }

    void updateState(CCNode* invoker) override {
        SettingNodeV3::updateState(invoker);
        auto shouldEnable = this->getSetting()->shouldEnable();
        m_button->setEnabled(shouldEnable);
        m_buttonSprite->setCascadeColorEnabled(true);
        m_buttonSprite->setCascadeOpacityEnabled(true);
        m_buttonSprite->setOpacity(shouldEnable ? 255 : 155);
        m_buttonSprite->setColor(shouldEnable ? ccWHITE : ccGRAY);
    }

    void onReinstall(CCObject*) {
        Notification::create("Downloading latest release...", NotificationIcon::Info)->show();
        geode::async::spawn(
            []() -> web::WebFuture {
                return web::WebRequest().get(UPDATE_URL);
            },
            [](web::WebResponse dlRes) {
                if (dlRes.ok()) {
                    auto path = Mod::get()->getPackagePath();
                    auto data = dlRes.data();
                    std::ofstream file(path.string(), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(data.data()), data.size());
                    file.close();
                    Notification::create("Reinstalled! Restart to apply!", NotificationIcon::Success)->show();
                } else {
                    Notification::create("Reinstall failed!", NotificationIcon::Error)->show();
                }
            }
        );
    }

    void onCommit() override {}
    void onResetToDefault() override {}

public:
    static ReinstallSettingNodeV3* create(std::shared_ptr<ReinstallSettingV3> setting, float width) {
        auto ret = new ReinstallSettingNodeV3();
        if (ret->init(setting, width)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

SettingNodeV3* ReinstallSettingV3::createNode(float width) {
    return ReinstallSettingNodeV3::create(
        std::static_pointer_cast<ReinstallSettingV3>(shared_from_this()),
        width
    );
}

// Queue Polling

void pollQueue() {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string queueUrl = SERVER + "/api/queue/" + token;
    geode::async::spawn(
        [queueUrl]() -> web::WebFuture {
            return web::WebRequest().get(queueUrl);
        },
        [](web::WebResponse res) {
            if (!res.ok()) return;
            auto jsonRes = res.json();
            if (!jsonRes) return;
            size_t count = 0;
            if ((*jsonRes).contains("requests") && (*jsonRes)["requests"].isArray()) {
                count = (*jsonRes)["requests"].asArray().unwrap().size();
            }
            g_lastQueueCount = count;
            
            if (auto menu = MenuLayer::get()) {
                if (auto btn = menu->getChildByIDRecursive("gd-requests-btn")) {
                    if (auto existing = btn->getChildByTag(QUEUE_BADGE_TAG)) existing->removeFromParent();
                    if (count > 0) {
                        auto badge = CCSprite::createWithSpriteFrameName("GJ_downloadsIcon_001.png"); // Temporary, better use a red circle
                        badge->setColor({255, 50, 50});
                        badge->setScale(0.5f);
                        badge->setPosition({btn->getContentSize().width - 5.f, btn->getContentSize().height - 5.f});
                        badge->setTag(QUEUE_BADGE_TAG);
                        
                        auto lbl = CCLabelBMFont::create(std::to_string(count).c_str(), "bigFont.fnt");
                        lbl->setScale(0.4f);
                        lbl->setPosition(badge->getContentSize() / 2);
                        badge->addChild(lbl);
                        
                        btn->addChild(badge);
                    }
                }
            }
        }
    );
}

// Mod Hooks

void fetchAndShowQueue() {
    checkForUpdate();
    if (g_fetchInProgress) return;
    g_fetchInProgress = true;
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) {
        g_fetchInProgress = false;
        geode::createQuickPopup("GD Requests", "No token set! Would you like to set one up?", "No", "Setup", [](FLAlertLayer*, bool btn2) {
            if (btn2) CCApplication::sharedApplication()->openURL("https://www.gdrequests.org/geode");
        });
        return;
    }
    auto popup = QueuePopup::createLoading();
    popup->show();
    popup->retain();
    std::string queueUrl = SERVER + "/api/queue/" + token;
    geode::async::spawn(
        [queueUrl]() -> web::WebFuture { return web::WebRequest().get(queueUrl); },
        [token, popup](web::WebResponse res) {
            if (!res.ok()) {
                g_fetchInProgress = false;
                popup->release();
                popup->removeFromParentAndCleanup(true);
                FLAlertLayer::create("GD Requests", "Could not reach the server.", "OK")->show();
                return;
            }
            auto jsonRes = res.json();
            if (!jsonRes) { g_fetchInProgress = false; popup->release(); popup->removeFromParentAndCleanup(true); return; }
            std::vector<QueueEntry> entries;
            std::vector<std::string> levelIdsToCheck;
            g_queueLevelIds.clear();
            g_queueLevelNames.clear();
            auto& json = *jsonRes;
            if (json.contains("requests") && json["requests"].isArray()) {
                for (auto& item : json["requests"]) {
                    QueueEntry qe;
                    qe.name = item["name"].asString().unwrapOr("Unknown");
                    qe.levelId = item["level_id"].asString().unwrapOr("");
                    qe.youtubeUrl = item["youtube_url"].asString().unwrapOr("");
                    qe.levelName = item["level_name"].asString().unwrapOr("");
                    qe.gdDifficulty = item["gd_difficulty"].asString().unwrapOr("");
                    qe.note = item["note"].asString().unwrapOr("");
                    if (!qe.levelId.empty() || !qe.youtubeUrl.empty()) {
                        if (!qe.levelId.empty()) {
                            g_queueLevelIds.insert(qe.levelId);
                            g_queueLevelNames[qe.levelId] = qe.name;
                            levelIdsToCheck.push_back(qe.levelId);
                        }
                        entries.push_back(std::move(qe));
                    }
                }
            }
            popup->populate(std::move(entries));
            popup->release();
            silentlyPruneNonexistentLevels(std::move(levelIdsToCheck));
        }
    );
}

struct $modify(GDReqMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto rightMenu = this->getChildByID("right-side-menu");
        if (!rightMenu) return true;
        CCNode* btnContent;
        auto logo = CCSprite::create("logo.png"_spr);
        if (logo) {
            const float targetSize = 35.f;
            logo->setScale(targetSize / logo->getContentSize().width);
            btnContent = logo;
        } else {
            auto lbl = CCLabelBMFont::create("GD Req", "goldFont.fnt", 160.f);
            lbl->setScale(0.7f);
            btnContent = lbl;
        }
        auto btn = CCMenuItemSpriteExtra::create(btnContent, this, menu_selector(GDReqMenuLayer::onOpenRequests));
        btn->setID("gd-requests-btn");
        rightMenu->addChild(btn);
        rightMenu->updateLayout();

        this->schedule(schedule_selector(GDReqMenuLayer::updateBadge), 5.f);
        return true;
    }
    void updateBadge(float) { pollQueue(); }
    void onOpenRequests(CCObject*) { fetchAndShowQueue(); }
};

struct $modify(GDReqPlayLayer, PlayLayer) {
    struct Fields {
        CCMenuItemSpriteExtra* m_blackBtn = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        std::string lvlId = std::to_string(level->m_levelID);
        g_blackScreenActive = false;

        // If re-entering a level that was already marked as the active queue level, restore the ID
        if (g_pendingQueueLevelId == lvlId) {
            g_currentQueueLevelId = lvlId;
        } else {
            g_currentQueueLevelId.clear();
            g_pendingQueueLevelId.clear();
        }

        if (!g_queueLevelIds.empty() && g_queueLevelIds.count(lvlId)) {
            g_currentQueueLevelId = lvlId;
            g_pendingQueueLevelId = lvlId;
            std::string requester = g_queueLevelNames.count(lvlId) ? g_queueLevelNames[lvlId] : "Unknown";
            g_queueLevelIds.erase(lvlId);
            g_queueLevelNames.erase(lvlId);
            sendQueueAction("/api/queue/played", lvlId);
            if (Mod::get()->getSettingValue<bool>("show-toast")) Notification::create(fmt::format("Now playing: ID {} by {}", lvlId, requester), NotificationIcon::None, 3.f)->show();
        }

        auto ws = CCDirector::get()->getWinSize();

        // Black overlay — covers the entire screen above everything
        auto overlay = CCLayerColor::create({0, 0, 0, 255});
        overlay->setContentSize(ws);
        overlay->setPosition({0.f, 0.f});
        overlay->setTag(BLACK_OVERLAY_TAG);
        overlay->setVisible(false);
        overlay->setZOrder(9999);
        this->addChild(overlay);

        // Black screen toggle button
        bool shouldHide = Mod::get()->getSettingValue<bool>("hide-black-btn");
        bool alwaysShow = Mod::get()->getSettingValue<bool>("always-show-black-btn");
#if !defined(GEODE_IS_ANDROID) && !defined(GEODE_IS_IOS)
        bool showBtn = alwaysShow && !shouldHide;
#else
        bool showBtn = !shouldHide;
#endif
        if (showBtn) {
            float btnSize = static_cast<float>(Mod::get()->getSettingValue<int64_t>("black-btn-size"));
            auto blackSpr = CCSprite::create("black-toggle.png"_spr);
            if (!blackSpr) {
                blackSpr = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png");
            }
            blackSpr->setContentSize({btnSize, btnSize});
            blackSpr->setScale(1.f);

            auto menu = CCMenu::create();
            menu->setPosition({0.f, 0.f});
            menu->setZOrder(10000);

            auto blackBtn = CCMenuItemSpriteExtra::create(blackSpr, this, menu_selector(GDReqPlayLayer::onBlackScreenBtn));
            blackBtn->setTag(BLACK_BTN_TAG);
            CCPoint btnPos = blackBtnPosition(ws, btnSize);
            blackBtn->setPosition(btnPos);
            menu->addChild(blackBtn);
            this->addChild(menu);
            m_fields->m_blackBtn = blackBtn;
        }

        return true;
    }

    void onBlackScreenBtn(CCObject*) {
        toggleBlackScreen();
    }

    void onExit() {
        if (g_pendingQueueLevelId == std::to_string(this->m_level->m_levelID))
            g_pendingQueueLevelId.clear();
        g_currentQueueLevelId.clear();
        PlayLayer::onExit();
    }
};

struct $modify(GDReqShaderLayer, ShaderLayer) {
    void visit() {
        if (g_blackScreenActive) return;
        ShaderLayer::visit();
    }
};

struct $modify(GDReqPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (g_currentQueueLevelId.empty()) return;
        auto ws = CCDirector::get()->getWinSize();
        auto removeSpr = CCLabelBMFont::create("Remove", "bigFont.fnt");
        removeSpr->setColor({255, 140, 40}); removeSpr->setScale(0.6f);
        auto banSpr = CCLabelBMFont::create("Ban Level", "bigFont.fnt");
        banSpr->setColor({220, 30, 30}); banSpr->setScale(0.6f);
        auto removeBtn = CCMenuItemSpriteExtra::create(removeSpr, this, menu_selector(GDReqPauseLayer::onRemoveFromQueue));
        auto banBtn = CCMenuItemSpriteExtra::create(banSpr, this, menu_selector(GDReqPauseLayer::onBanFromQueue));
        float btnY = ws.height * 0.07f;
        float rW = removeSpr->getContentSize().width * removeSpr->getScale();
        float bW = banSpr->getContentSize().width * banSpr->getScale();
        float gap = 12.f; float midX = ws.width / 2.f; float totalW = rW + gap + bW; float startX = midX - totalW / 2.f;
        auto menu = CCMenu::create(); menu->setPosition({0.f, 0.f});
        removeBtn->setPosition({startX + rW / 2.f, btnY});
        banBtn->setPosition({startX + rW + gap + bW / 2.f, btnY});
        menu->addChild(removeBtn); menu->addChild(banBtn);
        addChild(menu, 10);
    }
    void onRemoveFromQueue(CCObject*) { sendQueueAction("/api/queue/remove", g_currentQueueLevelId); }
    void onBanFromQueue(CCObject*) { sendQueueAction("/api/queue/blacklist", g_currentQueueLevelId); }
};

$on_mod(Loaded) {
    (void)Mod::get()->registerCustomSettingType("reinstall-button", &ReinstallSettingV3::parse);
    listenForKeybindSettingPresses("open-queue-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) fetchAndShowQueue();
    });
    listenForKeybindSettingPresses("black-screen-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat && PlayLayer::get()) toggleBlackScreen();
    });
}
