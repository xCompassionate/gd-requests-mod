#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include "Loquibot.h"
#include "GlobalVars.h"

class $modify(MenuLayer) {

	bool init() {
        if (!MenuLayer::init()) return false;

        GlobalVars::getSharedInstance()->isSearchScene = false;

        auto buttonSprite = CCSprite::create("gdrequests.png"_spr);

        auto button = CCMenuItemSpriteExtra::create(buttonSprite, this,
            menu_selector(Loquibot::openLevelMenu));

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        button->setPosition({ winSize.width - 40, winSize.height / 2 - 30 });

        auto menu = this->getChildByID("right-side-menu");
        menu->addChild(button);
        menu->updateLayout();

        return true;
	}
};
