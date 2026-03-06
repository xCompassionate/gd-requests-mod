#include <Geode/Geode.hpp>
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

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::unordered_set<std::string> g_queueLevelIds;
static std::unordered_map<std::string, std::string> g_queueLevelNames; // levelId → requester name
static bool g_fetchInProgress = false;
static std::string g_currentQueueLevelId; // level currently being played from queue

// ─── Data ─────────────────────────────────────────────────────────────────────

struct QueueEntry {
    std::string name;
    std::string levelId;
    std::string youtubeUrl;
    bool online = false;
};

// ─── Queue action helper (remove / blacklist / played) ────────────────────────

void sendQueueAction(const std::string& endpoint, const std::string& levelId) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + endpoint;
    std::string body = fmt::format(
        "{{\"token\":\"{}\",\"level_id\":\"{}\"}}",
        jsonEscape(token), jsonEscape(levelId)
    );
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
    std::string body = fmt::format(
        "{{\"token\":\"{}\",\"youtube_url\":\"{}\"}}",
        jsonEscape(token), jsonEscape(youtubeUrl)
    );
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

void sendQueueRemoveAll() {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url  = SERVER + "/api/queue/remove-all";
    std::string body = fmt::format("{{\"token\":\"{}\"}}", jsonEscape(token));
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

// ─── Queue popup (paginated — 5 per page) ────────────────────────────────────

class QueuePopup : public geode::Popup, public FLAlertLayerProtocol {
    std::vector<QueueEntry> m_entries;
    int m_page = 0;
    static constexpr int PER_PAGE = 5;

    bool init(std::vector<QueueEntry> entries) {
        if (!Popup::init(370.f, 295.f)) return false;
        m_entries = std::move(entries);
        setTitle("Request Queue");

        auto sz = m_mainLayer->getContentSize();

        // Dark overlay behind all content
        auto popupOverlay = CCDrawNode::create();
        {
            CCPoint v[] = {{0,0},{sz.width,0},{sz.width,sz.height},{0,sz.height}};
            popupOverlay->drawPolygon(v, 4, {0.0f,0.0f,0.0f,0.45f}, 0.f, {0,0,0,0});
        }
        m_mainLayer->addChild(popupOverlay, -2);

        if (m_entries.empty()) {
            auto lbl = CCLabelBMFont::create("Your queue is empty!", "bigFont.fnt", 280.f);
            lbl->setScale(0.5f);
            lbl->setPosition(sz / 2);
            m_mainLayer->addChild(lbl);
            return true;
        }

        buildPage();
        return true;
    }

    // ── Rebuild current page content ─────────────────────────────────────────
    void buildPage() {
        // Remove previous page content
        if (auto n = m_mainLayer->getChildByTag(900)) n->removeFromParent();
        if (auto n = m_mainLayer->getChildByTag(901)) n->removeFromParent();
        if (auto n = m_mainLayer->getChildByTag(902)) n->removeFromParent();
        if (auto n = m_mainLayer->getChildByTag(903)) n->removeFromParent();
        if (auto n = m_mainLayer->getChildByTag(904)) n->removeFromParent();

        auto sz = m_mainLayer->getContentSize();
        int total = (int)m_entries.size();
        int totalPages = (total + PER_PAGE - 1) / PER_PAGE;
        if (m_page >= totalPages) m_page = totalPages - 1;
        if (m_page < 0) m_page = 0;
        int startIdx = m_page * PER_PAGE;
        int endIdx   = std::min(startIdx + PER_PAGE, total);

        // ── Count label ──────────────────────────────────────────────────────
        auto countLbl = CCLabelBMFont::create(
            fmt::format("{} pending request{}",
                        total, total == 1 ? "" : "s").c_str(),
            "goldFont.fnt", 260.f
        );
        countLbl->setScale(0.35f);
        countLbl->setTag(900);
        countLbl->setPosition({sz.width / 2.f, sz.height - 38.f});
        m_mainLayer->addChild(countLbl);

        // ── Remove All button ────────────────────────────────────────────────
        auto removeAllMenu = CCMenu::create();
        removeAllMenu->setTag(904);
        removeAllMenu->setPosition({0.f, 0.f});

        auto removeAllLbl = CCLabelBMFont::create("Remove All", "bigFont.fnt", 200.f);
        removeAllLbl->setScale(0.28f);
        removeAllLbl->setColor({255, 70, 70});
        auto removeAllBtn = CCMenuItemSpriteExtra::create(
            removeAllLbl, this, menu_selector(QueuePopup::onRemoveAll));
        removeAllBtn->setPosition({sz.width - 50.f, sz.height - 38.f});
        removeAllMenu->addChild(removeAllBtn);
        m_mainLayer->addChild(removeAllMenu);

        // ── Page container (rows + menu) ─────────────────────────────────────
        const float rowH     = 43.f;
        const float fullW    = sz.width - 24.f;
        const float rowLeft  = 12.f;
        const float btnGap   = 3.f;
        const bool  ytEnabled = Mod::get()->getSettingValue<bool>("open-youtube");
        const float topY     = sz.height - 52.f;

        auto container = CCNode::create();
        container->setTag(901);
        container->setContentSize(sz);
        m_mainLayer->addChild(container);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        container->addChild(menu);

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

            // ── Row background ───────────────────────────────────────────────
            auto bg = CCDrawNode::create();
            {
                CCPoint v[] = {{0,0},{fullW,0},{fullW,inner},{0,inner}};
                bg->drawPolygon(v, 4, {0.0f,0.0f,0.0f,0.35f}, 0.f, {0,0,0,0});
            }
            bg->setAnchorPoint({0.f, 0.5f});
            bg->setPosition({rowLeft, rowCY - inner / 2.f});
            container->addChild(bg, -1);

            // ── Position number (absolute, not page-relative) ────────────────
            auto numLbl = CCLabelBMFont::create(
                std::to_string(idx + 1).c_str(), "bigFont.fnt", 30.f
            );
            numLbl->setScale(0.38f);
            numLbl->setPosition({14.f, inner / 2.f});

            // ── Requester name ───────────────────────────────────────────────
            auto nameLbl = CCLabelBMFont::create(e.name.c_str(), "bigFont.fnt", 200.f);
            nameLbl->setScale(0.44f);
            nameLbl->setAnchorPoint({0.f, 0.5f});
            nameLbl->setPosition({28.f, inner / 2.f + 8.f});

            // ── Level ID (or YouTube-only label) ─────────────────────────────
            std::string idText = e.levelId.empty() ? "YouTube request" : ("ID: " + e.levelId);
            auto idLbl = CCLabelBMFont::create(idText.c_str(), "bigFont.fnt", 200.f);
            idLbl->setScale(0.33f);
            idLbl->setColor(e.levelId.empty() ? ccColor3B{255, 70, 70} : ccColor3B{240, 200, 80});
            idLbl->setAnchorPoint({0.f, 0.5f});
            idLbl->setPosition({28.f, inner / 2.f - 9.f});

            // ── Main clickable area ──────────────────────────────────────────
            auto rowNode = CCNode::create();
            rowNode->setContentSize({mainW, inner});
            rowNode->addChild(numLbl);
            rowNode->addChild(nameLbl);
            rowNode->addChild(idLbl);

            auto mainBtn = CCMenuItemSpriteExtra::create(
                rowNode, this, menu_selector(QueuePopup::onEntry)
            );
            mainBtn->setTag(idx);
            mainBtn->setAnchorPoint({0.f, 0.5f});
            mainBtn->setPosition({rowLeft, rowCY});
            menu->addChild(mainBtn);

            // ── Action buttons ───────────────────────────────────────────────
            const float actX = rowLeft + mainW + 4.f;

            if (e.levelId.empty()) {
                // YouTube-only: Remove (top) + Watch Video (bottom)
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.30f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(
                    removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.18f});
                menu->addChild(removeBtn);

                auto watchLbl = CCLabelBMFont::create("Watch Video", "bigFont.fnt", stackW * 3.f);
                watchLbl->setScale(0.27f);
                watchLbl->setColor({255, 70, 70});
                auto watchBtn = CCMenuItemSpriteExtra::create(
                    watchLbl, this, menu_selector(QueuePopup::onWatch));
                watchBtn->setTag(idx);
                watchBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.18f});
                menu->addChild(watchBtn);
            } else {
                // Regular level: Remove (top) + Ban Level (bottom) + optional YT
                auto removeLbl = CCLabelBMFont::create("Remove", "bigFont.fnt", stackW * 3.f);
                removeLbl->setScale(0.30f);
                removeLbl->setColor({255, 140, 40});
                auto removeBtn = CCMenuItemSpriteExtra::create(
                    removeLbl, this, menu_selector(QueuePopup::onRemove));
                removeBtn->setTag(idx);
                removeBtn->setPosition({actX + stackW * 0.5f, rowCY + inner * 0.18f});
                menu->addChild(removeBtn);

                auto banLbl = CCLabelBMFont::create("Ban Level", "bigFont.fnt", stackW * 3.f);
                banLbl->setScale(0.30f);
                banLbl->setColor({220, 30, 30});
                auto banBtn = CCMenuItemSpriteExtra::create(
                    banLbl, this, menu_selector(QueuePopup::onBlacklist));
                banBtn->setTag(idx);
                banBtn->setPosition({actX + stackW * 0.5f, rowCY - inner * 0.18f});
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

        // ── Page navigation (only when multiple pages) ──────────────────────
        if (totalPages > 1) {
            auto navMenu = CCMenu::create();
            navMenu->setTag(902);
            navMenu->setPosition({0.f, 0.f});
            m_mainLayer->addChild(navMenu);

            float navY = 18.f;

            auto pageLbl = CCLabelBMFont::create(
                fmt::format("Page {} of {}", m_page + 1, totalPages).c_str(),
                "goldFont.fnt", 200.f
            );
            pageLbl->setScale(0.3f);
            pageLbl->setTag(903);
            pageLbl->setPosition({sz.width / 2.f, navY});
            m_mainLayer->addChild(pageLbl);

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

    // ── Navigation ───────────────────────────────────────────────────────────
    void onPrevPage(CCObject*) {
        if (m_page > 0) { m_page--; buildPage(); }
    }

    void onNextPage(CCObject*) {
        int totalPages = ((int)m_entries.size() + PER_PAGE - 1) / PER_PAGE;
        if (m_page < totalPages - 1) { m_page++; buildPage(); }
    }

    // ── Row actions ──────────────────────────────────────────────────────────

    // Open level search
    void onEntry(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.levelId.empty()) return;
        onClose(nullptr);
        auto searchObj = GJSearchObject::create(SearchType::Search, e.levelId);
        CCDirector::get()->pushScene(
            CCTransitionFade::create(0.5f, LevelBrowserLayer::scene(searchObj))
        );
    }

    // Remove from queue — stays open, rebuilds page
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
            auto remaining = m_entries;
            onClose(nullptr);
            QueuePopup::create(std::move(remaining))->show();
            return;
        }
        buildPage();
    }

    // Blacklist level — stays open, rebuilds page
    void onBlacklist(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto lvlId = m_entries[idx].levelId;
        g_queueLevelIds.erase(lvlId);
        sendQueueAction("/api/queue/blacklist", lvlId);
        m_entries.erase(m_entries.begin() + idx);
        if (m_entries.empty()) {
            auto remaining = m_entries;
            onClose(nullptr);
            QueuePopup::create(std::move(remaining))->show();
            return;
        }
        buildPage();
    }

    // Remove all — show confirmation first
    void onRemoveAll(CCObject*) {
        auto alert = FLAlertLayer::create(
            this,
            "Remove All",
            "Are you sure you want to <cr>remove all</c> pending requests?",
            "Cancel",
            "Remove All"
        );
        alert->show();
    }

    void FLAlert_Clicked(FLAlertLayer*, bool btn2) override {
        if (!btn2) return;  // Cancel pressed
        sendQueueRemoveAll();
        g_queueLevelIds.clear();
        m_entries.clear();
        // Reopen as empty popup
        onClose(nullptr);
        QueuePopup::create({})->show();
    }

    // Open YouTube showcase
    void onWatch(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx < 0 || idx >= (int)m_entries.size()) return;
        auto& e = m_entries[idx];
        if (e.youtubeUrl.empty()) return;
        std::string url = e.youtubeUrl;
        if (url.rfind("http", 0) != 0) url = "https://" + url;
        CCApplication::sharedApplication()->openURL(url.c_str());
    }

    ~QueuePopup() override {
        g_fetchInProgress = false;
    }

public:
    static QueuePopup* create(std::vector<QueueEntry> entries) {
        auto p = new QueuePopup();
        if (p->init(std::move(entries))) { p->autorelease(); return p; }
        CC_SAFE_DELETE(p);
        return nullptr;
    }
};

// ─── HTTP fetch (queue → chatter status → show popup) ────────────────────────

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

    std::string queueUrl = SERVER + "/api/queue/" + token;

    geode::async::spawn(
        [queueUrl]() -> web::WebFuture {
            return web::WebRequest().get(queueUrl);
        },
        [token](web::WebResponse res) {
            if (!res.ok()) {
                g_fetchInProgress = false;
                std::string msg = res.code() == 404
                    ? "Creator token not recognised. Double-check the token in Mods > GD Requests > Settings — copy it again from gdrequests.org."
                    : "Could not reach the server. Check your internet connection.";
                FLAlertLayer::create("GD Requests", msg.c_str(), "OK")->show();
                return;
            }

            auto jsonRes = res.json();
            if (!jsonRes) {
                g_fetchInProgress = false;
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
                    qe.name       = item["name"].asString().unwrapOr("Unknown");
                    qe.levelId    = item["level_id"].asString().unwrapOr("");
                    qe.youtubeUrl = item["youtube_url"].asString().unwrapOr("");
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
                QueuePopup::create(std::move(entries))->show();
                return;
            }

            // Chatter online status
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
                [entries = std::move(entries)](web::WebResponse statusRes) mutable {
                    if (statusRes.ok()) {
                        auto sJson = statusRes.json();
                        if (sJson) {
                            for (auto& e : entries) {
                                if ((*sJson).contains(e.name))
                                    e.online = (*sJson)[e.name].asBool().unwrapOr(false);
                            }
                        }
                    }
                    QueuePopup::create(std::move(entries))->show();
                }
            );
        }
    );
}

// ─── PlayLayer hook — auto-mark played when a queued level is entered ─────────

struct $modify(GDReqPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        std::string lvlId = std::to_string(level->m_levelID);
        g_currentQueueLevelId.clear();

        if (!g_queueLevelIds.empty() && g_queueLevelIds.count(lvlId)) {
            g_currentQueueLevelId = lvlId;

            std::string requester = "Unknown";
            auto it = g_queueLevelNames.find(lvlId);
            if (it != g_queueLevelNames.end()) requester = it->second;

            g_queueLevelIds.erase(lvlId);
            g_queueLevelNames.erase(lvlId);
            sendQueueAction("/api/queue/played", lvlId);

            // Toast notification (if enabled)
            if (Mod::get()->getSettingValue<bool>("show-toast")) {
                std::string toastMsg = fmt::format("Now playing: ID {} by {}", lvlId, requester);
                Notification::create(toastMsg, NotificationIcon::None, 3.f)->show();
            }
        }
        return true;
    }
};

// ─── PauseLayer hook — ban/remove a queued level while paused ─────────────────

struct $modify(GDReqPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto token = Mod::get()->getSettingValue<std::string>("creator-token");
        if (token.empty()) return;

        // Only show buttons if this level came from the queue
        if (g_currentQueueLevelId.empty()) return;

        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return;

        auto ws = CCDirector::get()->getWinSize();

        // Plain text labels — no background, fully transparent
        auto removeSpr = CCLabelBMFont::create("Remove",    "bigFont.fnt");
        removeSpr->setColor({255, 140, 40});   // orange
        removeSpr->setScale(0.6f);

        auto banSpr = CCLabelBMFont::create("Ban Level", "bigFont.fnt");
        banSpr->setColor({220, 30, 30});       // red
        banSpr->setScale(0.6f);

        auto removeBtn = CCMenuItemSpriteExtra::create(
            removeSpr, this, menu_selector(GDReqPauseLayer::onRemoveFromQueue));
        auto banBtn = CCMenuItemSpriteExtra::create(
            banSpr, this, menu_selector(GDReqPauseLayer::onBanFromQueue));

        // Below the sliders, near the bottom of the screen
        float btnY  = ws.height * 0.07f;
        float rW    = removeSpr->getContentSize().width * removeSpr->getScale();
        float bW    = banSpr->getContentSize().width    * banSpr->getScale();
        float gap   = 12.f;
        float midX  = ws.width / 2.f;

        // Center the pair: start left edge at midX - half of total group width
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
        g_queueLevelIds.erase(lvlId);
        sendQueueAction("/api/queue/blacklist", lvlId);
    }
};

// ─── MenuLayer hook ───────────────────────────────────────────────────────────

struct $modify(GDReqMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto ws = CCDirector::get()->getWinSize();

        // Logo button — square icon scaled to 65 GD units
        auto logo = CCSprite::create("logo.png"_spr);
        CCNode* btnContent;
        if (logo) {
            const float targetSize = 50.f;
            float scale = targetSize / logo->getContentSize().width;
            logo->setScale(scale);
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

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({ws.width - 42.f, ws.height - 42.f});
        addChild(menu, 10);

        return true;
    }

    void onOpenRequests(CCObject*) {
        fetchAndShowQueue();
    }
};

// Keybind listener for opening the queue
$on_mod(Loaded) {
    listenForKeybindSettingPresses("open-queue-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        auto token = Mod::get()->getSettingValue<std::string>("creator-token");
        if (!token.empty()) {
            fetchAndShowQueue();
        }
    });
}
