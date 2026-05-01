#include "GDRequestsAPI.h"
#include "Loquibot.h"
#include "BlockLevelAlertProtocol.h"
#include "BlockCreatorAlertProtocol.h"
#include "BlockRequesterAlertProtocol.h"
#include <signal.h>
#include "GlobalVars.h"
#include <Geode/Geode.hpp>
#include "RequestsLayer.h"
#include "GJGameLevel.h"
#include "BlockMenu.h"
#include "YouTubeMenu.h"
#include "LevelInfoLayer.h"

using namespace cocos2d;

Loquibot* Loquibot::instance = nullptr;

void Loquibot::hideButtons(CCObject* obj) {
    Loquibot::getSharedInstance()->m_isClickable = false;

    CCScene* currentScene = CCDirector::sharedDirector()->getRunningScene();
	LevelInfoLayer* layer = typeinfo_cast<LevelInfoLayer*>(currentScene->getChildren()->objectAtIndex(0));
    if (layer) {
        auto menu = layer->getChildByID("main-button-menu"_spr);
        if(menu){
            menu->setVisible(false);

            auto winSize = CCDirector::sharedDirector()->getWinSize();

            CCSprite* loadingSprite = CCSprite::create("loadingCircle.png");

            loadingSprite->setPosition({ winSize.width - 75, winSize.height / 2 + 32.5f });
            loadingSprite->setID("loading_sprite"_spr);
            loadingSprite->setScale(0.4);
            loadingSprite->setBlendFunc({ GL_ONE, GL_ONE_MINUS_CONSTANT_ALPHA });

            loadingSprite->runAction(CCRepeatForever::create(
                CCRotateBy::create(1.0f, 360)
                )
            );
            layer->addChild(loadingSprite);
        }
    }
}

void Loquibot::copyRequesterName(CCObject* obj) {
    std::string requester = GlobalVars::getSharedInstance()->requester;
    clipboard::write(requester);
}

void Loquibot::showButtons() {
    CCScene* currentScene = CCDirector::sharedDirector()->getRunningScene();
    Loquibot::getSharedInstance()->m_isClickable = true;

	LevelInfoLayer* layer = typeinfo_cast<LevelInfoLayer*>(currentScene->getChildren()->objectAtIndex(0));
    
    if(layer){
        auto menu = layer->getChildByID("main-button-menu"_spr);
        if(menu){
            menu->setVisible(true);
            layer->removeChildByID("loading_sprite"_spr);
        }
    }
}

void Loquibot::goToLevel(CCObject* obj) {
    if (!Loquibot::getSharedInstance()->m_isClickable) return;
    
    hideButtons(obj);
    GDRequestsAPI::fetchQueue([this, obj](std::vector<QueueEntry> entries) {
        showButtons();
        if (entries.empty()) {
            FLAlertLayer::create("GD Requests", "The queue is empty!", "OK")->show();
            return;
        }
        
        // Pick the first level request
        for (const auto& e : entries) {
            if (!e.levelId.empty()) {
                auto searchObj = GJSearchObject::create(SearchType::Search, e.levelId);
                auto scene = LevelBrowserLayer::scene(searchObj);
                CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
                return;
            }
        }
        FLAlertLayer::create("GD Requests", "No level requests in queue.", "OK")->show();
    });
}

void Loquibot::goToNextLevel(CCObject* obj) {
    // For GDRequests, "Next" usually means the first item as well, 
    // but maybe we skip the currently playing one if we track it.
    goToLevel(obj);
}

void Loquibot::goToTopLevel(CCObject* obj) {
    goToLevel(obj);
}


void Loquibot::goToRandomLevel(CCObject* obj) {
    if (!Loquibot::getSharedInstance()->m_isClickable) return;
    
    hideButtons(obj);
    GDRequestsAPI::fetchQueue([this, obj](std::vector<QueueEntry> entries) {
        showButtons();
        std::vector<QueueEntry> levels;
        for (const auto& e : entries) if (!e.levelId.empty()) levels.push_back(e);
        
        if (levels.empty()) {
            FLAlertLayer::create("GD Requests", "No level requests in queue.", "OK")->show();
            return;
        }
        
        auto& e = levels[rand() % levels.size()];
        auto searchObj = GJSearchObject::create(SearchType::Search, e.levelId);
        auto scene = LevelBrowserLayer::scene(searchObj);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    });
}

void Loquibot::goToUndoLevel(CCObject* obj) {
    // GDRequests doesn't have an "undo" endpoint easily accessible like this
    FLAlertLayer::create("Info", "Undo feature is coming soon.", "OK")->show();
}


void Loquibot::blockLevel(CCObject*) {
    auto gv = GlobalVars::getSharedInstance();
    if (gv->currentID == 0) return;
    
    geode::createQuickPopup(
        "Block Level",
        fmt::format("Are you sure you want to <cr>block ID {}</c>?", gv->currentID),
        "Cancel", "Block",
        [gv](auto, bool btn2) {
            if (btn2) {
                GDRequestsAPI::sendQueueAction("/api/queue/blacklist", std::to_string(gv->currentID));
            }
        }
    );
}

void Loquibot::blockCreator(CCObject*) {
    // GDRequests doesn't support blocking by creator name directly via API yet
    FLAlertLayer::create("Info", "Creator blocking is coming soon.", "OK")->show();
}

void Loquibot::blockRequester(CCObject*) {
    auto gv = GlobalVars::getSharedInstance();
    if (gv->requester.empty()) return;

    geode::createQuickPopup(
        "Timeout User",
        fmt::format("Are you sure you want to <cr>timeout {}</c>?", gv->requester),
        "Cancel", "Timeout",
        [gv](auto, bool btn2) {
            if (btn2) {
                GDRequestsAPI::sendTimeoutUser(gv->requester);
            }
        }
    );
}

void Loquibot::goToMainScene(CCObject*) {
    auto layer = MenuLayer::create();
    auto scene = CCScene::create();
    scene->addChild(layer);
    auto transition = CCTransitionFade::create(0.5f, scene);
    CCDirector::sharedDirector()->pushScene(transition);
}

void Loquibot::openLevelMenu(CCObject*){
    RequestsLayer::open();
}

void Loquibot::openYoutube(CCObject*){
    // This was for selecting YT from a specific level info
    // We can implement it if we track which level has YT
}

void Loquibot::showBlockMenu(CCObject*){
    BlockMenu* menu = BlockMenu::create();
    menu->show();
}

void Loquibot::showYouTube(LevelInfoLayer* LevelInfoLayer){
    if(LevelInfoLayer){
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        CCNode* title = LevelInfoLayer->getChildByID("title-label");
        float titleWidth = title->getContentSize().width;

        auto youtubeButtonSprite = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
        auto youtubeButton = CCMenuItemSpriteExtra::create(youtubeButtonSprite, LevelInfoLayer,
            menu_selector(Loquibot::openYoutube));
        youtubeButton->ignoreAnchorPointForPosition(true);

        CCMenu* youtubeButtonMenu = CCMenu::create();
        youtubeButtonMenu->setContentSize({30,30});
        youtubeButtonMenu->ignoreAnchorPointForPosition(false);
        youtubeButtonMenu->setPosition({title->getPositionX() + (titleWidth * title->getScaleX())/2.0f + 20, title->getPositionY()-2});
        youtubeButtonMenu->setScale(0.5f);
        youtubeButtonMenu->addChild(youtubeButton);

        LevelInfoLayer->addChild(youtubeButtonMenu);
    }
}
