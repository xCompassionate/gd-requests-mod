#pragma once

#ifndef __CLEARALERT_HPP
#define __CLEARALERT_HPP

#include <Geode/Geode.hpp>
#include "GDRequestsAPI.h"
#include "GlobalVars.h"

class ClearAlertProtocol : public FLAlertLayerProtocol {

public:

	void FLAlert_Clicked(FLAlertLayer*, bool btn2) {
		if (btn2) {
			GDRequestsAPI::sendQueueRemoveAll();
			
			CCDirector::sharedDirector()->popSceneWithTransition(0.5f, PopTransition::kPopTransitionFade);
		}
	};

};

#endif