#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include "GlobalVars.h"
#include "GDRequestsAPI.h"
#include "RequestsLayer.h"

using namespace geode::prelude;

// Helper to toggle black screen
static void toggleBlackScreen() {
    auto pl = PlayLayer::get();
    if (!pl) return;

    auto gv = GlobalVars::getSharedInstance();
    gv->blackScreenActive = !gv->blackScreenActive;

    auto uiLayer = pl->m_uiLayer;
    if (!uiLayer) return;

    auto existing = uiLayer->getChildByTag(9871);
    if (gv->blackScreenActive) {
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

class $modify(GDReqPlayLayer, PlayLayer) {
    struct Fields {
        bool m_isQueued = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto gv = GlobalVars::getSharedInstance();
        std::string lvlId = std::to_string(level->m_levelID);
        gv->currentQueueLevelId.clear();
        gv->blackScreenActive = false;

        if (gv->queueLevelIds.count(lvlId)) {
            gv->currentQueueLevelId = lvlId;
            m_fields->m_isQueued = true;

            std::string requester = "Unknown";
            if (gv->queueLevelNames.count(lvlId)) requester = gv->queueLevelNames[lvlId];

            gv->queueLevelIds.erase(lvlId);
            gv->queueLevelNames.erase(lvlId);
            GDRequestsAPI::sendQueueAction("/api/queue/played", lvlId);

            if (Mod::get()->getSettingValue<bool>("show-toast")) {
                Notification::create(
                    fmt::format("Now playing: ID {} by {}", lvlId, requester),
                    NotificationIcon::None, 3.f
                )->show();
            }
        }

        // Black screen button logic
        bool showBtn = false;
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_IOS)
        showBtn = true;
#else
        showBtn = Mod::get()->getSettingValue<bool>("always-show-black-btn");
#endif
        if (showBtn && !Mod::get()->getSettingValue<bool>("hide-black-btn") && !gv->currentQueueLevelId.empty()) {
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
            else                               { bx = half;              by = half;             } 

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

class $modify(GDReqPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto gv = GlobalVars::getSharedInstance();
        if (gv->currentQueueLevelId.empty()) return;

        auto ws = CCDirector::get()->getWinSize();

        auto removeSpr = CCLabelBMFont::create("Remove", "bigFont.fnt");
        removeSpr->setColor({255, 140, 40});
        removeSpr->setScale(0.6f);

        auto banSpr = CCLabelBMFont::create("Ban Level", "bigFont.fnt");
        banSpr->setColor({220, 30, 30});
        banSpr->setScale(0.6f);

        auto removeBtn = CCMenuItemSpriteExtra::create(
            removeSpr, this, menu_selector(GDReqPauseLayer::onRemoveFromQueue));
        auto banBtn = CCMenuItemSpriteExtra::create(
            banSpr, this, menu_selector(GDReqPauseLayer::onBanFromQueue));

        float btnY = ws.height * 0.15f;
        float rW = removeSpr->getContentSize().width * removeSpr->getScale();
        float bW = banSpr->getContentSize().width * banSpr->getScale();
        float gap = 15.f;
        float midX = ws.width / 2.f;
        float totalW = rW + gap + bW;
        float startX = midX - totalW / 2.f;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        removeBtn->setPosition({startX + rW / 2.f, btnY});
        banBtn->setPosition({startX + rW + gap + bW / 2.f, btnY});
        menu->addChild(removeBtn);
        menu->addChild(banBtn);
        addChild(menu, 10);
    }

    void onRemoveFromQueue(CCObject*) {
        auto gv = GlobalVars::getSharedInstance();
        if (gv->currentQueueLevelId.empty()) return;
        GDRequestsAPI::sendQueueAction("/api/queue/remove", gv->currentQueueLevelId);
        gv->currentQueueLevelId.clear();
        Notification::create("Removed from queue", NotificationIcon::Success)->show();
    }

    void onBanFromQueue(CCObject*) {
        auto gv = GlobalVars::getSharedInstance();
        if (gv->currentQueueLevelId.empty()) return;
        
        std::string lvlId = gv->currentQueueLevelId;
        geode::createQuickPopup(
            "Ban Level",
            fmt::format("Are you sure you want to <cr>ban ID {}</c>?", lvlId),
            "Cancel", "Ban",
            [lvlId](auto, bool btn2) {
                if (btn2) {
                    GDRequestsAPI::sendQueueAction("/api/queue/blacklist", lvlId);
                    GlobalVars::getSharedInstance()->currentQueueLevelId.clear();
                }
            }
        );
    }
};
