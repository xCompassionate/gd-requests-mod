#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <unordered_set>
#include <cmath>
#include <algorithm>
#include <vector>

using namespace geode::prelude;

static const std::string SERVER = "https://www.gdrequests.org";


static std::unordered_set<std::string> g_queueLevelIds;
static std::unordered_map<std::string, std::string> g_queueLevelNames; // levelId -> requester
static bool g_fetchInProgress = false;
static std::string g_currentQueueLevelId; // tracks which queued level is being played

static bool g_blackScreenActive = false;

struct QueueEntry {
    std::string name;
    std::string levelId;
    std::string youtubeUrl;
    std::string levelName;
    std::string gdDifficulty;
    std::string source; // "twitch", "youtube", or empty/unknown
    bool online = false;
};

// fires off a POST to the server
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

// Tag used to identify the loading circle node so we can remove only it
static constexpr int LOADING_CIRCLE_TAG = 9880;
// Tag used to identify content nodes added by buildPage / empty label
static constexpr int CONTENT_ROOT_TAG   = 9881;

// Returns a short colored source tag label for Twitch/YouTube
static std::string sourceTag(const std::string& source) {
    if (source == "twitch") return " [TTV]";
    if (source == "youtube") return " [YT]";
    return "";
}

static ccColor3B sourceColor(const std::string& source) {
    if (source == "twitch") return {145, 70, 255};  // Twitch purple
    if (source == "youtube") return {255, 70, 70};   // YouTube red
    return {180, 180, 200};
}

// popup that shows 5 entries per page
class QueuePopup : public geode::Popup {
    std::vector<QueueEntry> m_entries;
    int m_page = 0;
    bool m_loading = false;
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
            spinnerRoot->setPosition(m_mainLayer->getContentSize() / 2); // center
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
        // remove any previous content root
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

    // wipes old page nodes and draws the current page
    void buildPage() {
        // remove previous content root only — title / close button are unaffected
        if (auto n = m_mainLayer->getChildByTag(CONTENT_ROOT_TAG)) n->removeFromParent();

        auto sz = m_mainLayer->getContentSize();
        int total = (int)m_entries.size();
        int totalPages = (total + PER_PAGE - 1) / PER_PAGE;
        if (m_page >= totalPages) m_page = totalPages - 1;
        if (m_page < 0) m_page = 0;
        int startIdx = m_page * PER_PAGE;
        int endIdx   = std::min(startIdx + PER_PAGE, total);

        // single root node that owns everything on this page
        auto root = CCNode::create();
        root->setTag(CONTENT_ROOT_TAG);
        root->setContentSize(sz);
        m_mainLayer->addChild(root);

        // "X pending requests" label
        auto countLbl = CCLabelBMFont::create(
            fmt::format("{} pending request{}",
                        total, total == 1 ? "" : "s").c_str(),
            "goldFont.fnt", 260.f
        );
        countLbl->setScale(0.35f);
        countLbl->setPosition({sz.width / 2.f, sz.height - 38.f});
        root->addChild(countLbl);

        // remove all button (top right)
        auto removeAllMenu = CCMenu::create();
        removeAllMenu->setPosition({0.f, 0.f});

        auto removeAllLbl = CCLabelBMFont::create("Remove All", "bigFont.fnt", 200.f);
        removeAllLbl->setScale(0.28f);
        removeAllLbl->setColor({255, 70, 70});
        auto removeAllBtn = CCMenuItemSpriteExtra::create(
            removeAllLbl, this, menu_selector(QueuePopup::onRemoveAll));
        removeAllBtn->setPosition({sz.width - 50.f, sz.height - 38.f});
        removeAllMenu->addChild(removeAllBtn);
        root->addChild(removeAllMenu);

        // layout constants
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

            // row bg
            auto bg = CCDrawNode::create();
            {
                CCPoint v[] = {{0,0},{fullW,0},{fullW,inner},{0,inner}};
                bg->drawPolygon(v, 4, {0.0f,0.0f,0.0f,0.35f}, 0.f, {0,0,0,0});
            }
            bg->setAnchorPoint({0.f, 0.5f});
            bg->setPosition({rowLeft, rowCY - inner / 2.f});
            root->addChild(bg, -1);

            // queue position number
            auto numLbl = CCLabelBMFont::create(
                std::to_string(idx + 1).c_str(), "bigFont.fnt", 30.f
            );
            numLbl->setScale(0.38f);
            numLbl->setPosition({14.f, inner / 2.f});

            // top line: level name (or ID fallback) with difficulty color
            std::string topText;
            ccColor3B topColor;
            if (e.levelId.empty()) {
                topText = "YouTube request";
                topColor = {255, 70, 70};
            } else if (!e.levelName.empty()) {
                topText = e.levelName;
                // color by difficulty
                if (e.gdDifficulty == "easy") topColor = {80, 210, 80};
                else if (e.gdDifficulty == "normal") topColor = {80, 180, 255};
                else if (e.gdDifficulty == "hard") topColor = {255, 160, 50};
                else if (e.gdDifficulty == "harder") topColor = {255, 100, 100};
                else if (e.gdDifficulty == "insane") topColor = {255, 60, 180};
                else if (e.gdDifficulty.find("demon") != std::string::npos) topColor = {255, 40, 40};
                else if (e.gdDifficulty == "auto") topColor = {200, 200, 100};
                else topColor = {240, 200, 80};
            } else {
                topText = "ID: " + e.levelId;
                topColor = {240, 200, 80};
            }
            auto topLbl = CCLabelBMFont::create(topText.c_str(), "bigFont.fnt", 200.f);
            topLbl->setScale(0.40f);
            topLbl->setColor(topColor);
            topLbl->setAnchorPoint({0.f, 0.5f});
            topLbl->setPosition({28.f, inner / 2.f + 8.f});

            // bottom line: requester name + source tag + difficulty
            std::string bottomText = e.name + sourceTag(e.source);
            if (!e.levelId.empty() && !e.gdDifficulty.empty() && e.gdDifficulty != "na") {
                std::string diff = e.gdDifficulty;
                for (auto& ch : diff) if (ch == '_') ch = ' ';
                bool cap = true;
                for (auto& ch : diff) {
                    if (cap && ch >= 'a' && ch <= 'z') ch -= 32;
                    cap = (ch == ' ');
                }
                bottomText += " | " + diff;
            }
            auto bottomLbl = CCLabelBMFont::create(bottomText.c_str(), "bigFont.fnt", 200.f);
            bottomLbl->setScale(0.30f);
            bottomLbl->setColor(sourceColor(e.source));
            bottomLbl->setAnchorPoint({0.f, 0.5f});
            bottomLbl->setPosition({28.f, inner / 2.f - 9.f});

            // clickable row area
            auto rowNode = CCNode::create();
            rowNode->setContentSize({mainW, inner});
            rowNode->addChild(numLbl);
            rowNode->addChild(topLbl);
            rowNode->addChild(bottomLbl);

            auto mainBtn = CCMenuItemSpriteExtra::create(
                rowNode, this, menu_selector(QueuePopup::onEntry)
            );
            mainBtn->setTag(idx);
            mainBtn->setAnchorPoint({0.f, 0.5f});
            mainBtn->setPosition({rowLeft, rowCY});
            menu->addChild(mainBtn);

            // action buttons on the right side
            const float actX = rowLeft + mainW + 4.f;

            if (e.levelId.empty()) {
                // youtube-only entry: remove, timeout, watch
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.26f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(
                    removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.28f});
                menu->addChild(removeBtn);

                auto timeoutLbl = CCLabelBMFont::create("Timeout", "bigFont.fnt", stackW * 3.f);
                timeoutLbl->setScale(0.26f);
                timeoutLbl->setColor({255, 200, 50});
                auto timeoutBtn = CCMenuItemSpriteExtra::create(
                    timeoutLbl, this, menu_selector(QueuePopup::onTimeout));
                timeoutBtn->setTag(idx);
                timeoutBtn->setPosition({actX + stackW * 0.5f, rowCY});
                menu->addChild(timeoutBtn);

                auto watchLbl = CCLabelBMFont::create("Watch", "bigFont.fnt", stackW * 3.f);
                watchLbl->setScale(0.26f);
                watchLbl->setColor({255, 70, 70});
                auto watchBtn = CCMenuItemSpriteExtra::create(
                    watchLbl, this, menu_selector(QueuePopup::onWatch));
                watchBtn->setTag(idx);
                watchBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.28f});
                menu->addChild(watchBtn);
            } else {
                // normal level entry: Remove, Timeout, Ban stacked
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.26f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(
                    removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.28f});
                menu->addChild(removeBtn);

                auto timeoutLbl = CCLabelBMFont::create("Timeout", "bigFont.fnt", stackW * 3.f);
                timeoutLbl->setScale(0.26f);
                timeoutLbl->setColor({255, 200, 50});
                auto timeoutBtn = CCMenuItemSpriteExtra::create(
                    timeoutLbl, this, menu_selector(QueuePopup::onTimeout));
                timeoutBtn->setTag(idx);
                timeoutBtn->setPosition({actX + stackW * 0.5f, rowCY});
                menu->addChild(timeoutBtn);

                auto banLbl = CCLabelBMFont::create("Ban Level", "bigFont.fnt", stackW * 3.f);
                banLbl->setScale(0.26f);
                banLbl->setColor({220, 30, 30});
                auto banBtn = CCMenuItemSpriteExtra::create(
                    banLbl, this, menu_selector(QueuePopup::onBlacklist));
                banBtn->setTag(idx);
                banBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.28f});
                menu->addChild(banBtn);

                if (hasYT) {
                    auto ytLbl = CCLabelBMFont::create("YT", "bigFont.fnt", ytW * 3.f);
                    ytLbl->setScale(0.30f);
                    ytLbl->setColor({255, 70, 70});
                    auto ytBtn = CCMenuItemSpriteExtra::create(
                        ytLbl, this, menu_selector(QueuePopup::onWatch));
                    ytBtn->setTag(idx);
                    ytBtn->setPosition({actX + stackW + btnGap + ytW * 0.5f, rowCY});
                    menu->addChild(ytBtn);
                }
            }
        }

        // prev/next buttons when there's more than one page
        if (totalPages > 1) {
            auto navMenu = CCMenu::create();
            navMenu->setPosition({0.f, 0.f});
            root->addChild(navMenu);

            float navY = 18.f;

            auto pageLbl = CCLabelBMFont::create(
                fmt::format("Page {} of {}", m_page + 1, totalPages).c_str(),
                "goldFont.fnt", 200.f
            );
            pageLbl->setScale(0.3f);
            pageLbl->setPosition({sz.width / 2.f, navY});
            root->addChild(pageLbl);

            if (m_page > 0) {
                auto prevLbl = CCLabelBMFont::create("< Prev", "bigFont.fnt", 100.f);
                prevLbl->setScale(0.35f);
                auto prevBtn = CCMenuItemSpriteExtra::create(
                    prevLbl, this, menu_selector(QueuePopup::onPrevPage));
                prevBtn->setPosition({sz.width * 0.2f, navY});
                navMenu->addChild(prevBtn);
            }

            if (m_page < totalPages - 1) {
                auto nextLbl = CCLabelBMFont::create("Next >", "bigFont.fnt", 100.f);
                nextLbl->setScale(0.35f);
                auto nextBtn = CCMenuItemSpriteExtra::create(
                    nextLbl, this, menu_selector(QueuePopup::onNextPage));
                nextBtn->setPosition({sz.width * 0.8f, navY});
                navMenu->addChild(nextBtn);
            }
        }
    }

    void onPrevPage(CCObject*) {
        if (m_page > 0) { m_page--; buildPage(); }
    }

    void onNextPage(CCObject*) {
        int totalPages = ((int)m_entries.size() + PER_PAGE - 1) / PER_PAGE;
        if (m_page < totalPages - 1) { m_page++; buildPage(); }
    }

    // click a row to search for that level
    void onEntry(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.levelId.empty()) return;
        onClose(nullptr);
        auto searchObj = GJSearchObject::create(SearchType::Search, e.levelId);
        auto director = CCDirector::get();
        // pop back to main menu first so back doesn't cycle through old levels
        director->popToRootScene();
        director->pushScene(
            CCTransitionFade::create(0.5f, LevelBrowserLayer::scene(searchObj))
        );
    }

    // remove entry, stay in popup
    void onRemove(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        g_queueLevelIds.erase(e.levelId);
        if (!e.levelId.empty()) {
            sendQueueAction("/api/queue/remove", e.levelId);
        } else {
            sendQueueRemoveYoutube(e.youtubeUrl);
        }
        m_entries.erase(m_entries.begin() + idx);
        if (m_entries.empty()) {
            buildEmpty();
            return;
        }
        buildPage();
    }

    // timeout a user
    void onTimeout(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        sendTimeoutUser(m_entries[idx].name);
    }

    // ban a level — delays the actual server call by 5s so the streamer can cancel
    void onBlacklist(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto lvlId = m_entries[idx].levelId;
        auto displayName = m_entries[idx].levelName.empty()
            ? ("ID " + lvlId)
            : m_entries[idx].levelName;
        g_queueLevelIds.erase(lvlId);
        m_entries.erase(m_entries.begin() + idx);

        // shared cancel flag — set to true if UNBAN is clicked before 5s elapses
        auto cancelled = std::make_shared<bool>(false);

        // show toast immediately
        Notification::create(
            fmt::format("{} is banned", displayName),
            NotificationIcon::Warning, 5.f
        )->show();

        // floating UNBAN button — cancels the ban before it reaches the server
        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            auto ws = CCDirector::get()->getWinSize();

            auto unbanLbl = CCLabelBMFont::create("UNBAN", "bigFont.fnt");
            unbanLbl->setColor({100, 220, 255});
            unbanLbl->setScale(0.45f);

            struct CancelHelper : public CCObject {
                std::shared_ptr<bool> cancelled;
                std::string name;
                CCMenu* menu = nullptr;
                void onCancel(CCObject*) {
                    *cancelled = true;
                    Notification::create(
                        fmt::format("{} ban cancelled", name),
                        NotificationIcon::Success, 2.f
                    )->show();
                    if (menu) menu->removeFromParent();
                }
            };
            auto* helper = new CancelHelper();
            helper->autorelease();
            helper->cancelled = cancelled;
            helper->name      = displayName;

            auto menu = CCMenu::create();
            menu->addChild(CCMenuItemSpriteExtra::create(
                unbanLbl, helper, menu_selector(CancelHelper::onCancel)
            ));
            menu->setPosition({ws.width / 2.f + 80.f, 35.f});
            helper->menu = menu;
            scene->addChild(menu, 9999);
            menu->runAction(CCSequence::create(
                CCDelayTime::create(5.f),
                CCCallFunc::create(menu, callfunc_selector(CCNode::removeFromParent)),
                nullptr
            ));
        }

        // fire the actual ban after 5s — only if not cancelled
        // CCCallFunc doesn't accept lambdas; use a helper CCObject with a method
        struct BanHelper : public CCObject {
            std::shared_ptr<bool> cancelled;
            std::string levelId;
            void fireBan(CCObject*) {
                if (!*cancelled)
                    sendQueueAction("/api/queue/blacklist", levelId);
            }
        };
        auto lvlIdCopy = lvlId;
        auto* banHelper = new BanHelper();
        banHelper->autorelease();
        banHelper->cancelled = cancelled;
        banHelper->levelId   = lvlIdCopy;
        // Retain so it survives the 5s delay uwu
        banHelper->retain();
        auto banNode = CCNode::create();
        CCDirector::get()->getRunningScene()->addChild(banNode);
        banNode->runAction(CCSequence::create(
            CCDelayTime::create(5.f),
            CCCallFuncN::create(banHelper, callfuncN_selector(BanHelper::fireBan)),
            CCCallFunc::create(banNode, callfunc_selector(CCNode::removeFromParent)),
            nullptr
        ));
        banHelper->release();

        if (m_entries.empty()) {
            buildEmpty();
            return;
        }
        buildPage();
    }


    // confirm before nuking the whole queue
    void onRemoveAll(CCObject*) {
        geode::createQuickPopup(
            "Remove All",
            "Are you sure you want to <cr>remove all</c> pending requests?",
            "Cancel", "Remove All",
            [this](auto, bool btn2) {
                if (!btn2) return;
                sendQueueRemoveAll();
                g_queueLevelIds.clear();
                m_entries.clear();
                buildEmpty();
            }
        );
    }

    // open a youtube link in browser
    void onWatch(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.youtubeUrl.empty()) return;
        std::string url = e.youtubeUrl;
        if (url.rfind("http", 0) != 0) url = "https://" + url;
        CCApplication::sharedApplication()->openURL(url.c_str());
    }

    ~QueuePopup() {
        g_fetchInProgress = false;
    }

public:
    static QueuePopup* createLoading() { // show popup directly
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
        if (auto spinnerRoot = m_mainLayer->getChildByTag(LOADING_CIRCLE_TAG))
            spinnerRoot->removeFromParent();
        if (m_entries.empty()) { buildEmpty(); return; }
        buildPage();
    }
};

// grabs the queue from the server, checks chatter status, then shows the popup
void fetchAndShowQueue() {
    if (g_fetchInProgress) return;
    g_fetchInProgress = true;

    auto token = Mod::get()->getSettingValue<std::string>("creator-token");

    if (token.empty()) {
        g_fetchInProgress = false;
        FLAlertLayer::create(
            "GD Requests",
            "No token set! Go to Mods > GD Requests > Settings and paste your creator token from gdrequests.org.",
            "OK"
        )->show();
        return;
    }

    // Show the popup immediately with a loading spinner
    auto popup = QueuePopup::createLoading();
    popup->show();
    // Keep a retained ref so we can populate it after the fetch
    popup->retain();

    std::string queueUrl = SERVER + "/api/queue/" + token;

    geode::async::spawn(
        [queueUrl]() -> web::WebFuture {
            return web::WebRequest().get(queueUrl);
        },
        [token, popup](web::WebResponse res) {
            if (!res.ok()) {
                g_fetchInProgress = false;
                popup->release();
                popup->removeFromParentAndCleanup(true);
                std::string msg = res.code() == 404
                    ? "Creator token not recognised. Double-check the token in Mods > GD Requests > Settings — copy it again from gdrequests.org."
                    : "Could not reach the server. Check your internet connection.";
                FLAlertLayer::create("GD Requests", msg.c_str(), "OK")->show();
                return;
            }

            auto jsonRes = res.json();
            if (!jsonRes) {
                g_fetchInProgress = false;
                popup->release();
                popup->removeFromParentAndCleanup(true);
                FLAlertLayer::create("GD Requests", "Invalid server response.", "OK")->show();
                return;
            }

            std::vector<QueueEntry> entries;
            g_queueLevelIds.clear();
            g_queueLevelNames.clear();

            auto& json = *jsonRes;
            if (json.contains("requests") && json["requests"].isArray()) {
                for (auto& item : json["requests"]) {
                    QueueEntry qe;
                    qe.name         = item["name"].asString().unwrapOr("Unknown");
                    qe.levelId      = item["level_id"].asString().unwrapOr("");
                    qe.youtubeUrl   = item["youtube_url"].asString().unwrapOr("");
                    qe.levelName    = item["level_name"].asString().unwrapOr("");
                    qe.gdDifficulty = item["gd_difficulty"].asString().unwrapOr("");
                    qe.source       = item["source"].asString().unwrapOr(""); // "twitch" or "youtube"
                    if (!qe.levelId.empty() || !qe.youtubeUrl.empty()) {
                        if (!qe.levelId.empty()) {
                            g_queueLevelIds.insert(qe.levelId);
                            g_queueLevelNames[qe.levelId] = qe.name;
                        }
                        entries.push_back(std::move(qe));
                    }
                }
            }

            if (entries.empty()) {
                popup->populate({});
                popup->release();
                return;
            }

            // check which requesters are currently in chat
            std::string names;
            for (auto& e : entries) {
                if (!names.empty()) names += ",";
                names += e.name;
            }
            std::string statusUrl =
                SERVER + "/api/chatter-status?token=" + token + "&names=" + names;

            geode::async::spawn(
                [statusUrl]() -> web::WebFuture {
                    return web::WebRequest().get(statusUrl);
                },
                [entries = std::move(entries), popup](web::WebResponse statusRes) mutable {
                    if (statusRes.ok()) {
                        auto sJson = statusRes.json();
                        if (sJson) {
                            for (auto& e : entries) {
                                if ((*sJson).contains(e.name))
                                    e.online = (*sJson)[e.name].asBool().unwrapOr(false);
                            }
                        }
                    }
                    popup->populate(std::move(entries));
                    popup->release();
                }
            );
        }
    );
}

static void toggleBlackScreen() {
    auto pl = PlayLayer::get();
    if (!pl) return;

    g_blackScreenActive = !g_blackScreenActive;

    // Use m_uiLayer so the black screen is above camera/shader layers
    // and static camera triggers can't move or clip it
    auto uiLayer = pl->m_uiLayer;
    if (!uiLayer) return;

    auto existing = uiLayer->getChildByTag(9871);
    if (g_blackScreenActive) {
        if (existing) return;
        auto ws = CCDirector::get()->getWinSize();
        auto black = CCLayerColor::create({0, 0, 0, 255}, ws.width, ws.height);
        black->setTag(9871);
        black->setPosition({0, 0});
        uiLayer->addChild(black, 9990);
    } else {
        if (existing) existing->removeFromParent();
    }
}

static bool shouldShowBlackScreenButton() {
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_IOS)
    return true;
#else
    return Mod::get()->getSettingValue<bool>("always-show-black-btn");
#endif
}

// auto-marks a level as played when the player enters it from the queue
struct $modify(GDReqPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        std::string lvlId = std::to_string(level->m_levelID);
        g_currentQueueLevelId.clear();
        g_blackScreenActive = false;
        // black screen node lives in m_uiLayer which is built during init,
        // so nothing to remove here — the flag reset above is sufficient

        if (!g_queueLevelIds.empty() && g_queueLevelIds.count(lvlId)) {
            g_currentQueueLevelId = lvlId;

            std::string requester = "Unknown";
            auto it = g_queueLevelNames.find(lvlId);
            if (it != g_queueLevelNames.end()) requester = it->second;

            g_queueLevelIds.erase(lvlId);
            g_queueLevelNames.erase(lvlId);
            sendQueueAction("/api/queue/played", lvlId);

            if (Mod::get()->getSettingValue<bool>("show-toast")) {
                std::string toastMsg = fmt::format("Now playing: ID {} by {}", lvlId, requester);
                Notification::create(toastMsg, NotificationIcon::None, 3.f)->show();
            }
        }

        if (shouldShowBlackScreenButton() && !Mod::get()->getSettingValue<bool>("hide-black-btn") && !g_currentQueueLevelId.empty()) {
            auto ws = CCDirector::get()->getWinSize();
            float targetSize = static_cast<float>(Mod::get()->getSettingValue<int64_t>("black-btn-size"));
            float half = targetSize / 2.f + 8.f;
            auto posStr = Mod::get()->getSettingValue<std::string>("black-btn-position");

            float bx, by;
            if (posStr == "Top Left")          { bx = half;              by = ws.height - half; }
            else if (posStr == "Top Right")    { bx = ws.width - half;   by = ws.height - half; }
            else if (posStr == "Center Left")  { bx = half;              by = ws.height / 2.f;  }
            else if (posStr == "Center Right") { bx = ws.width - half;   by = ws.height / 2.f;  }
            else if (posStr == "Bottom Right") { bx = ws.width - half;   by = half;             }
            else                               { bx = half;              by = half;             } // Bottom Left (default)

            auto btnSpr = CCSprite::create("black-toggle.png"_spr);
            if (!btnSpr) {
                auto fallback = CCLabelBMFont::create("B", "bigFont.fnt");
                fallback->setScale(0.6f);
                btnSpr = CCSprite::create();
                btnSpr->addChild(fallback);
            } else {
                btnSpr->setScale(targetSize / btnSpr->getContentSize().width);
            }

            auto btn = CCMenuItemSpriteExtra::create(
                btnSpr, this, menu_selector(GDReqPlayLayer::onBlackScreenBtn)
            );
            btn->setTag(9872);

            auto menu = CCMenu::create();
            menu->setPosition({bx, by});
            menu->addChild(btn);
            addChild(menu, 9999);
        }

        return true;
    }

    void onBlackScreenBtn(CCObject*) {
        toggleBlackScreen();
    }
};

// adds remove/ban buttons to the pause menu for queued levels
struct $modify(GDReqPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto token = Mod::get()->getSettingValue<std::string>("creator-token");
        if (token.empty()) return;
        if (g_currentQueueLevelId.empty()) return;

        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return;

        auto ws = CCDirector::get()->getWinSize();

        auto removeSpr = CCLabelBMFont::create("Remove",    "bigFont.fnt");
        removeSpr->setColor({255, 140, 40});
        removeSpr->setScale(0.6f);

        auto banSpr = CCLabelBMFont::create("Ban Level", "bigFont.fnt");
        banSpr->setColor({220, 30, 30});
        banSpr->setScale(0.6f);

        auto removeBtn = CCMenuItemSpriteExtra::create(
            removeSpr, this, menu_selector(GDReqPauseLayer::onRemoveFromQueue));
        auto banBtn = CCMenuItemSpriteExtra::create(
            banSpr, this, menu_selector(GDReqPauseLayer::onBanFromQueue));

        float btnY  = ws.height * 0.07f;
        float rW    = removeSpr->getContentSize().width * removeSpr->getScale();
        float bW    = banSpr->getContentSize().width    * banSpr->getScale();
        float gap   = 12.f;
        float midX  = ws.width / 2.f;
        float totalW = rW + gap + bW;
        float startX = midX - totalW / 2.f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        removeBtn->setPosition({startX + rW / 2.f,              btnY});
        banBtn->setPosition(   {startX + rW + gap + bW / 2.f,   btnY});
        menu->addChild(removeBtn);
        menu->addChild(banBtn);
        addChild(menu, 10);
    }

    void onRemoveFromQueue(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return;
        std::string lvlId = std::to_string(pl->m_level->m_levelID);
        g_queueLevelIds.erase(lvlId);
        sendQueueAction("/api/queue/remove", lvlId);
    }

    void onBanFromQueue(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return;
        std::string lvlId = std::to_string(pl->m_level->m_levelID);
        std::string displayName = "ID " + lvlId;
        g_queueLevelIds.erase(lvlId);

        // shared cancel flag
        auto cancelled = std::make_shared<bool>(false);

        Notification::create(
            fmt::format("{} is banned", displayName),
            NotificationIcon::Warning, 5.f
        )->show();

        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            auto ws = CCDirector::get()->getWinSize();

            auto unbanLbl = CCLabelBMFont::create("UNBAN", "bigFont.fnt");
            unbanLbl->setColor({100, 220, 255});
            unbanLbl->setScale(0.45f);

            struct CancelHelper : public CCObject {
                std::shared_ptr<bool> cancelled;
                std::string name;
                CCMenu* menu = nullptr;
                void onCancel(CCObject*) {
                    *cancelled = true;
                    Notification::create(
                        fmt::format("{} ban cancelled", name),
                        NotificationIcon::Success, 2.f
                    )->show();
                    if (menu) menu->removeFromParent();
                }
            };
            auto* helper = new CancelHelper();
            helper->autorelease();
            helper->cancelled = cancelled;
            helper->name      = displayName;

            auto menu = CCMenu::create();
            menu->addChild(CCMenuItemSpriteExtra::create(
                unbanLbl, helper, menu_selector(CancelHelper::onCancel)
            ));
            menu->setPosition({ws.width / 2.f + 80.f, 35.f});
            helper->menu = menu;
            scene->addChild(menu, 9999);
            menu->runAction(CCSequence::create(
                CCDelayTime::create(5.f),
                CCCallFunc::create(menu, callfunc_selector(CCNode::removeFromParent)),
                nullptr
            ));
        }

        struct BanHelper2 : public CCObject {
            std::shared_ptr<bool> cancelled;
            std::string levelId;
            void fireBan(CCObject*) {
                if (!*cancelled)
                    sendQueueAction("/api/queue/blacklist", levelId);
            }
        };
        auto lvlIdCopy = lvlId;
        auto* banHelper = new BanHelper2();
        banHelper->autorelease();
        banHelper->cancelled = cancelled;
        banHelper->levelId   = lvlIdCopy;
        banHelper->retain();
        auto banNode = CCNode::create();
        CCDirector::get()->getRunningScene()->addChild(banNode);
        banNode->runAction(CCSequence::create(
            CCDelayTime::create(5.f),
            CCCallFuncN::create(banHelper, callfuncN_selector(BanHelper2::fireBan)),
            CCCallFunc::create(banNode, callfunc_selector(CCNode::removeFromParent)),
            nullptr
        ));
        banHelper->release();
    }
};

// main menu button — added into the existing right-side-menu so layout handles positioning
struct $modify(GDReqMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // FIX: get the right-side-menu that Geode/NodeIDs sets up, and add our button there.
        // updateLayout() repositions all children automatically — no manual coordinate math needed.
        auto rightMenu = this->getChildByID("right-side-menu");
        if (!rightMenu) return true; // safety: nothing to attach to

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

        auto btn = CCMenuItemSpriteExtra::create(
            btnContent, this, menu_selector(GDReqMenuLayer::onOpenRequests)
        );
        btn->setID("gd-requests-btn");

        rightMenu->addChild(btn);
        rightMenu->updateLayout();

        return true;
    }

    void onOpenRequests(CCObject*) {
        fetchAndShowQueue();
    }
};

// hotkey to open queue from anywhere
$on_mod(Loaded) {
    listenForKeybindSettingPresses("open-queue-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        auto token = Mod::get()->getSettingValue<std::string>("creator-token");
        if (!token.empty()) {
            fetchAndShowQueue();
        }
    });

    listenForKeybindSettingPresses("black-screen-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        toggleBlackScreen();
    });
}
