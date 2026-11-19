// Fill out your copyright notice in the Description page of Project Settings.

#include "BoxPhysicsExample.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
void StartBox3DIrisAuthoredVerification();
void StopBox3DIrisAuthoredVerification();
#endif

class FBoxPhysicsExampleModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if WITH_DEV_AUTOMATION_TESTS
		StartBox3DIrisAuthoredVerification();
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_DEV_AUTOMATION_TESTS
		StopBox3DIrisAuthoredVerification();
#endif

	}

};

IMPLEMENT_PRIMARY_GAME_MODULE(FBoxPhysicsExampleModule, BoxPhysicsExample, "BoxPhysicsExample");
