#include "RequestsLayer.h"
#include "GlobalVars.h"
#include "GDRequestsAPI.h"
#include "ClearAlertProtocol.h"

RequestsLayer* RequestsLayer::create(const std::vector<QueueEntry>& entries) {
    auto ret = new RequestsLayer();
    if (ret && ret->init(entries)) {
        ret->autorelease();
    } else {
        delete ret;
        ret = nullptr;
    }
    return ret;
}

void RequestsLayer::open() {
    GDRequestsAPI::fetchQueue([](std::vector<QueueEntry> entries) {
        auto scene = RequestsLayer::scene(entries);
        auto transition = CCTransitionFade::create(0.5f, scene);
        CCDirector::sharedDirector()->pushScene(transition);
    });
}

bool RequestsLayer::init(const std::vector<QueueEntry>& entries) {
    m_entries = entries;
    GlobalVars::getSharedInstance()->onReqScene = true;

    auto backgroundSprite = CCSprite::create("GJ_gradientBG.png");
    
    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto size = backgroundSprite->getContentSize(); 
    
    backgroundSprite->setScaleX(winSize.width / size.width);
    backgroundSprite->setScaleY(winSize.height / size.height);
    backgroundSprite->setID("background"_spr);
    
    backgroundSprite->setAnchorPoint({0, 0});
    backgroundSprite->setColor({0, 102, 255});
    
    backgroundSprite->setZOrder(-2);
    addChild(backgroundSprite);

    auto backBtn = CCMenuItemSpriteExtra::create(
        CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png"),
        this,
        menu_selector(RequestsLayer::onBack)
    );
    backBtn->setID("exit-button"_spr);

    auto menuBack = CCMenu::create();
    menuBack->addChild(backBtn);
    menuBack->setPosition({25, winSize.height - 25});
    menuBack->setID("exit-menu"_spr);

    // ... (corners setup) ...
    CCSprite* m_cornerBL = CCSprite::createWithSpriteFrameName("GJ_sideArt_001.png");
    m_cornerBL->setPosition({0,0});
    m_cornerBL->setAnchorPoint({0,0});
    addChild(m_cornerBL, -1);
    m_cornerBL->setID("corner-bl"_spr);

    CCSprite* m_cornerTL = CCSprite::createWithSpriteFrameName("GJ_sideArt_001.png");
    m_cornerTL->setPosition({0,winSize.height});
    m_cornerTL->setAnchorPoint({0,1});
    m_cornerTL->setFlipY(true);
    m_cornerTL->setID("corner-ul"_spr);
    addChild(m_cornerTL, -1);

    CCSprite* m_cornerTR = CCSprite::createWithSpriteFrameName("GJ_sideArt_001.png");
    m_cornerTR->setPosition({winSize.width,winSize.height});
    m_cornerTR->setAnchorPoint({1,1});
    m_cornerTR->setFlipX(true);
    m_cornerTR->setFlipY(true);
    m_cornerTR->setID("corner-ur"_spr);
    addChild(m_cornerTR, -1);

    CCSprite* m_cornerBR = CCSprite::createWithSpriteFrameName("GJ_sideArt_001.png");
    m_cornerBR->setPosition({winSize.width,0});
    m_cornerBR->setAnchorPoint({1,0});
    m_cornerBR->setFlipX(true);
    m_cornerBR->setID("corner-br"_spr);
    addChild(m_cornerBR, -1);

    addChild(menuBack);

    CCArray* levels = CCArray::create();
    for (const auto& entry : entries) {
        if (!entry.levelId.empty()) {
            levels->addObject(GDRequestsAPI::parseQueueEntryToLevel(entry));
        }
    }

    CustomListView* customListView = CustomListView::create(levels, nullptr, 240, 356, 0, BoomListType::Level4, 0);

    GJListLayer* listLayer = GJListLayer::create(customListView, "", {191, 114, 62, 255}, 356, 240, 1);
    listLayer->setID("list-layer"_spr);
    listLayer->setAnchorPoint({0.5, 0.5});
    listLayer->ignoreAnchorPointForPosition(false);
    listLayer->setPosition(winSize.width/2, winSize.height/2 - 5);

    CCSprite* levelRequestsLabel = CCSprite::create("levelRequestsLabel.png"_spr);
    levelRequestsLabel->setPosition({winSize.width/2, winSize.height - 28});
    levelRequestsLabel->setScale(0.8f);

    float menuWidth = 60.0f;

    // We don't have a direct "toggle" endpoint in GDRequests, but we can implement it if needed
    // For now, let's keep the UI but maybe disable functionality or link to a relevant setting
    m_toggleQueue = CCMenuItemToggler::createWithSize("GJ_pauseEditorBtn_001.png","GJ_playEditorBtn_001.png", this, menu_selector(RequestsLayer::runToggle), 1.0f);
    m_toggleQueue->setPosition({menuWidth/2, winSize.height/2+24});
    m_toggleQueue->toggle(true); // Default to on

    CCSprite* clearSprite = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
    clearSprite->setScale(0.8f);
    CCMenuItemSpriteExtra* clearQueue = CCMenuItemSpriteExtra::create(clearSprite, this, menu_selector(RequestsLayer::runClear));
    clearQueue->setPosition({menuWidth/2, winSize.height/2-24});

    CCMenu* rightMenu = CCMenu::create();
    rightMenu->setContentSize({menuWidth, winSize.height});
    rightMenu->setPosition({winSize.width, 0});
    rightMenu->ignoreAnchorPointForPosition(false);
    rightMenu->setAnchorPoint({1, 0});

    rightMenu->addChild(clearQueue);
    rightMenu->addChild(m_toggleQueue);

    addChild(rightMenu);
    addChild(listLayer);
    addChild(levelRequestsLabel);

    setKeypadEnabled(true);

    return true;
}



void RequestsLayer::keyBackClicked() {
    CCDirector::sharedDirector()->popSceneWithTransition(0.5f, PopTransition::kPopTransitionFade);
}


void RequestsLayer::onBack(CCObject* object) {
    GlobalVars::getSharedInstance()->onReqScene = false;
    keyBackClicked();
}

CCScene* RequestsLayer::scene(const std::vector<QueueEntry>& entries) {
    auto layer = RequestsLayer::create(entries);
    auto scene = CCScene::create();
    scene->addChild(layer);
    return scene;
}

void RequestsLayer::updateToggle(bool enabled){
    m_toggleQueue->toggle(!enabled);
}

void RequestsLayer::runToggle(CCObject* obj){
    // GDRequests doesn't have a simple toggle endpoint, maybe add one later
    FLAlertLayer::create("Info", "Queue toggling is managed on gdrequests.org dashboard.", "OK")->show();
}

void RequestsLayer::runClear(CCObject* obj){
    geode::createQuickPopup(
        "Clear Queue",
        "Are you sure you want to <cr>remove all</c> pending requests?",
        "Cancel", "Clear All",
        [this](auto, bool btn2) {
            if (btn2) {
                GDRequestsAPI::sendQueueRemoveAll();
                // Optionally refresh UI or pop back
                onBack(nullptr);
            }
        }
    );
}
